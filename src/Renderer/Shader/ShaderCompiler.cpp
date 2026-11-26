#include "Core/Log.h"
#include "ShaderCompiler.h"
#include "DxilLibraryCache.h"
#include "ShaderVariants.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    // Cooked-game port (2026-11-26): while an offline cook compiles on this thread (ShaderCompiler::ProductionArgumentsScope),
    // the ReSTIRPT/ tree's development switches read no environment variable, so a packaged game's DXIL never depends on the
    // environment the Editor ran Build Game in.
    thread_local bool t_productionArguments = false;

    // A development switch's environment value, or null while a production scope is active on this thread.
    const char* DevelopmentSwitch(const char* name)
    {
        if(t_productionArguments)
        {
            return nullptr;
        }

        return std::getenv(name);
    }

    // The adaptive light sampler (default since the user decision of 2026-11-23): the ReSTIRPT/ tree compiles with
    // RESTIR_PT_LIST_PROTOTYPE (per-receiver light-grid list proposals in the ReGIR kernel) unless
    // ENGINE_RESTIR_PT_LIST_PROTOTYPE=0.
    bool UsesListPrototype(const std::filesystem::path& shaderPath)
    {
        const char* value = DevelopmentSwitch("ENGINE_RESTIR_PT_LIST_PROTOTYPE");

        if(value != nullptr && std::string(value) == "0")
        {
            return false;
        }

        for(const std::filesystem::path& component : shaderPath)
        {
            if(component == "ReSTIRPT")
            {
                return true;
            }
        }

        return false;
    }

    std::filesystem::path DependencyPath(const wchar_t* path)
    {
        std::error_code error;
        const std::filesystem::path absolute = std::filesystem::absolute(path, error);
        return (error ? std::filesystem::path(path) : absolute).lexically_normal();
    }

    void RecordDependency(std::vector<ShaderSourceDependency>& dependencies, const wchar_t* path, IDxcBlob* source)
    {
        ShaderSourceDependency dependency{};
        dependency.path = DependencyPath(path);

        dependency.exists = source != nullptr;

        if(source)
        {
            const char* begin = static_cast<const char*>(source->GetBufferPointer());
            dependency.contents.assign(begin, begin + source->GetBufferSize());
        }

        for(const ShaderSourceDependency& existing : dependencies)
        {
            if(existing.path == dependency.path && existing.exists == dependency.exists &&
                existing.contents == dependency.contents)
            {
                return;
            }
        }

        // Preserve conflicting observations of a path. No current filesystem state can satisfy both,
        // so freshness checks and persistent publication reject a compilation with mixed inputs.
        dependencies.push_back(std::move(dependency));
    }

    // Synchronous callers log as they always have; a worker collects the text instead, because it must
    // never call into Engine logging.
    void ReportLibraryError(std::string* diagnostics, std::string_view message)
    {
        if(!diagnostics)
        {
            Log::Error(message);
            return;
        }

        if(!diagnostics->empty())
        {
            diagnostics->push_back('\n');
        }

        diagnostics->append(message);
    }

    // Forwards to the default handler and records every lookup, including the misses: a file appearing
    // earlier in the search order would change which source an include resolves to. Lives on the stack
    // for one Compile call, which releases every reference it takes before returning.
    //
    // A shader variant (ShaderVariants) included by a file is served from its generated text. It is recorded as an
    // existing dependency with that text, which no file can match, so such a compilation is never answered from or
    // stored in the persistent cache: the variant table is not part of the including file's cache identity.
    class RecordingIncludeHandler final : public IDxcIncludeHandler
    {
        public:
        RecordingIncludeHandler(IDxcIncludeHandler* inner, std::vector<ShaderSourceDependency>& dependencies,
            IDxcUtils* utils, const wchar_t* shaderRoot) :
            m_inner(inner), m_dependencies(dependencies), m_utils(utils), m_shaderRoot(shaderRoot)
        {
        }

        HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR filename, IDxcBlob** includeSource) override
        {
            const ShaderVariant* variant = m_shaderRoot && *m_shaderRoot != L'\0' ?
                ShaderVariants::FindUnder(m_shaderRoot, filename) : nullptr;

            if(variant)
            {
                return LoadVariant(*variant, filename, includeSource);
            }

            const HRESULT result = m_inner->LoadSource(filename, includeSource);
            RecordDependency(m_dependencies, filename, SUCCEEDED(result) ? *includeSource : nullptr);
            return result;
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override
        {
            if(id == __uuidof(IDxcIncludeHandler) || id == __uuidof(IUnknown))
            {
                *object = static_cast<IDxcIncludeHandler*>(this);
                return S_OK;
            }

            *object = nullptr;
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override
        {
            return 1;
        }

        ULONG STDMETHODCALLTYPE Release() override
        {
            return 1;
        }

        private:
        HRESULT LoadVariant(const ShaderVariant& variant, LPCWSTR filename, IDxcBlob** includeSource)
        {
            const std::optional<std::string> text = ShaderVariants::Source(variant);

            if(!text)
            {
                return E_FAIL;
            }

            IDxcBlobEncoding* blob = nullptr;
            const HRESULT result = m_utils->CreateBlob(text->data(), static_cast<UINT32>(text->size()), DXC_CP_UTF8, &blob);

            if(FAILED(result))
            {
                return result;
            }

            RecordDependency(m_dependencies, filename, blob);
            *includeSource = blob;
            return S_OK;
        }

        IDxcIncludeHandler* m_inner = nullptr;
        std::vector<ShaderSourceDependency>& m_dependencies;
        IDxcUtils* m_utils = nullptr;
        const wchar_t* m_shaderRoot = nullptr;
    };

    // The variant at path under shaderRoot (ShaderVariants), with the text it compiles as; nothing for a file.
    struct VariantSource
    {
        const ShaderVariant* variant = nullptr;
        std::string text;
    };

    // A variant whose base chain is broken reports it here; the compilation then fails as for a missing file.
    std::optional<VariantSource> FindVariantSource(const wchar_t* path, const wchar_t* shaderRoot)
    {
        if(!path || !shaderRoot || *shaderRoot == L'\0')
        {
            return VariantSource{};
        }

        const ShaderVariant* variant = ShaderVariants::FindUnder(shaderRoot, path);

        if(!variant)
        {
            return VariantSource{};
        }

        std::optional<std::string> text = ShaderVariants::Source(*variant);

        if(!text)
        {
            return std::nullopt;
        }

        return VariantSource{variant, std::move(*text)};
    }

    // The main source: a variant's generated text, or the file DXC reads as before.
    HRESULT LoadMainSource(IDxcUtils* utils, const wchar_t* path, const VariantSource& variant, IDxcBlobEncoding** source)
    {
        if(variant.variant)
        {
            return utils->CreateBlob(variant.text.data(), static_cast<UINT32>(variant.text.size()), DXC_CP_UTF8, source);
        }

        return utils->LoadFile(path, nullptr, source);
    }
}

namespace
{
    // dxcompiler.dll is found by name, so whichever copy the loader resolved is checked against the pinned
    // release (cmake/DXC.cmake): major.minor, commit count, commit hash and release string, which together
    // identify one build. Returns an empty string when the loaded compiler is the pinned one.
    std::string DescribeCompilerIdentityMismatch(IDxcCompiler3* compiler)
    {
        const std::string expected = std::string("expected DXC ") + ENGINE_DXC_VERSION + " (commit " +
            std::to_string(ENGINE_DXC_COMMIT_COUNT) + "-" + ENGINE_DXC_COMMIT_HASH + ")";

        IDxcVersionInfo3* versionInfo = nullptr;

        if(FAILED(compiler->QueryInterface(IID_PPV_ARGS(&versionInfo))))
        {
            return "The loaded dxcompiler.dll does not report version information; " + expected + ".";
        }

        IDxcVersionInfo2* commitInfo = nullptr;

        if(FAILED(versionInfo->QueryInterface(IID_PPV_ARGS(&commitInfo))))
        {
            versionInfo->Release();
            return "The loaded dxcompiler.dll does not report commit information; " + expected + ".";
        }

        UINT32 major = 0;
        UINT32 minor = 0;
        UINT32 commitCount = 0;
        char* commitHash = nullptr;
        char* releaseVersion = nullptr;
        const bool queried = SUCCEEDED(commitInfo->GetVersion(&major, &minor)) &&
            SUCCEEDED(commitInfo->GetCommitInfo(&commitCount, &commitHash)) &&
            SUCCEEDED(versionInfo->GetCustomVersionString(&releaseVersion));

        const std::string detectedHash = commitHash ? commitHash : "";
        const std::string detectedRelease = releaseVersion ? releaseVersion : "";
        CoTaskMemFree(commitHash);
        CoTaskMemFree(releaseVersion);
        commitInfo->Release();
        versionInfo->Release();

        if(!queried)
        {
            return "The loaded dxcompiler.dll failed to report its version; " + expected + ".";
        }

        const bool pinned = major == ENGINE_DXC_VERSION_MAJOR && minor == ENGINE_DXC_VERSION_MINOR &&
            commitCount == ENGINE_DXC_COMMIT_COUNT && detectedHash == ENGINE_DXC_COMMIT_HASH &&
            detectedRelease == ENGINE_DXC_VERSION;

        if(pinned)
        {
            return {};
        }

        return "Wrong DirectX Shader Compiler: the loaded dxcompiler.dll reports " +
            (detectedRelease.empty() ? std::to_string(major) + "." + std::to_string(minor) : detectedRelease) +
            " (commit " + std::to_string(commitCount) + "-" + detectedHash + "); " + expected +
            ". Rebuild Engine to redeploy the pinned dxcompiler.dll and dxil.dll beside the executable.";
    }
}

bool ShaderCompiler::Initialize()
{
    return Initialize(nullptr);
}

bool ShaderCompiler::Initialize(std::string& diagnostics)
{
    return Initialize(&diagnostics);
}

bool ShaderCompiler::Initialize(std::string* diagnostics)
{
    HRESULT result = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&m_utils));

    if (FAILED(result))
    {
        ReportLibraryError(diagnostics, "Cannot create the DXC utilities (DxcCreateInstance). HRESULT: " +
            std::to_string(result));
        return false;
    }

    result = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_compiler));

    if (FAILED(result))
    {
        m_utils->Release();
        m_utils = nullptr;

        ReportLibraryError(diagnostics, "Cannot create the DXC compiler (DxcCreateInstance). HRESULT: " +
            std::to_string(result));
        return false;
    }

    // Never compile with an unpinned compiler: there is no fallback, so a mismatch leaves no compiler.
    const std::string mismatch = DescribeCompilerIdentityMismatch(m_compiler);

    if(!mismatch.empty())
    {
        Shutdown();
        ReportLibraryError(diagnostics, mismatch);
        return false;
    }

    return true;
}

void ShaderCompiler::Shutdown()
{
    if (m_compiler)
    {
        m_compiler->Release();
        m_compiler = nullptr;
    }

    if (m_utils)
    {
        m_utils->Release();

        m_utils = nullptr;
    }
}

bool ShaderCompiler::Compile(
    const ShaderCompileDescription& description,
    ShaderCompileResult& output)
{
    output = {};

    if (!m_utils || !m_compiler)
    {
        return false;
    }

    if (!description.path || !description.entryPoint)
    {
        return false;
    }

    const wchar_t* profile = GetShaderProfile(description.stage);

    if (!profile)
    {
        return false;
    }

    std::filesystem::path shaderPath = description.path;

    std::wstring shaderDirectory = shaderPath.parent_path().wstring();

    std::vector<const wchar_t*> arguments = {description.path, L"-E", description.entryPoint, L"-T", profile, L"-HV",
                                             L"2021",          L"-I", shaderDirectory.c_str()};

    // NVIDIA SHARC's compute shaders (its resolve) store fp16 data like its libraries.
    if(UsesNative16BitTypes(description.path))
    {
        arguments.push_back(L"-enable-16bit-types");
    }

    // M5.1 T-2.1: configuration B's ReSTIRPT/ tree with strict IEEE only on request (UsesStrictFloatingPoint; A1).
    if(UsesStrictFloatingPoint(description.path))
    {
        arguments.push_back(L"-Gis");
    }

    // M5.6 B3a: the ReSTIRPT/ tree's pre-B3a BSDF evaluation on request (UsesUnfusedBsdf).
    if(UsesUnfusedBsdf(description.path))
    {
        arguments.push_back(L"-D");
        arguments.push_back(L"RESTIR_PT_UNFUSED_BSDF=1");
    }

    // Audit T1-c: the ReSTIRPT/ tree's diagnostic counters only on request (UsesDiagnosticCounters).
    if(UsesDiagnosticCounters(description.path))
    {
        arguments.push_back(L"-D");
        arguments.push_back(L"RESTIR_PT_COUNTERS=1");
    }

    if(DisablesSmallLightPath(description.path))
    {
        arguments.push_back(L"-D");
        arguments.push_back(L"RESTIR_PT_NO_SMALL_LIGHTS=1");
    }

    if(UsesListPrototype(description.path))
    {
        arguments.push_back(L"-D");
        arguments.push_back(L"RESTIR_PT_LIST_PROTOTYPE=1");
    }

    // Shader variants (ShaderVariants) compile from their generated text instead of a file.
    const std::optional<VariantSource> variant = FindVariantSource(description.path, description.shaderRoot);

    if(!variant)
    {
        Log::Error("Shader variant has an invalid base chain: " + std::filesystem::path(description.path).string());
        return false;
    }

    // M0c.5 Stage 5.4: the persistent cache, for the same target and arguments. A miss compiles exactly as before.
    const auto cacheStart = std::chrono::steady_clock::now();
    const std::string target = std::filesystem::path(profile).string() + ":" + std::filesystem::path(description.entryPoint).string();
    const DxilLibraryCache cache(description.usePersistentCache, description.path, description.shaderRoot, target, arguments,
        variant->text);
    std::vector<ShaderSourceDependency> dependencies;
    output.cacheHit = cache.Load(m_utils, output.bytecode, dependencies);
    output.cacheReadMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cacheStart).count();

    if (output.cacheHit)
    {
        return true;
    }

    output.bytecode.clear();
    dependencies.clear();

    IDxcBlobEncoding* source = nullptr;

    HRESULT result = LoadMainSource(m_utils, description.path, *variant, &source);

    if (FAILED(result))
    {
        Log::Error("Cannot read required shader source: " + std::filesystem::path(description.path).string() +
            " (HRESULT " + std::to_string(result) + "). Keep the bundled assets/Shaders directory beside Engine.exe.");
        return false;
    }

    DxcBuffer sourceBuffer{};
    sourceBuffer.Ptr = source->GetBufferPointer();

    sourceBuffer.Size = source->GetBufferSize();
    sourceBuffer.Encoding = DXC_CP_UTF8;

    IDxcIncludeHandler* includeHandler = nullptr;

    result = m_utils->CreateDefaultIncludeHandler(&includeHandler);

    if (FAILED(result))
    {
        source->Release();
        return false;
    }

    IDxcResult* compileResult = nullptr;

    // Every file read and include lookup, for the cache entry (as CompileLibrary records them). A variant's text is in
    // the cache identity instead: it has no file to check.
    if(!variant->variant)
    {
        RecordDependency(dependencies, description.path, source);
    }

    RecordingIncludeHandler recordingIncludeHandler(includeHandler, dependencies, m_utils, description.shaderRoot);

    const auto compileStart = std::chrono::steady_clock::now();
    output.dxcInvoked = true;
    result =
        m_compiler->Compile(&sourceBuffer, arguments.data(), static_cast<UINT32>(arguments.size()),
                            &recordingIncludeHandler, IID_PPV_ARGS(&compileResult));
    output.dxcMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compileStart).count();

    includeHandler->Release();

    if (FAILED(result))
    {
        source->Release();
        return false;
    }

    HRESULT compileStatus = S_OK;
    result = compileResult->GetStatus(&compileStatus);

    if (FAILED(result))
    {
        Log::Error("Failed to retrieve DXC shader compilation status (GetStatus). HRESULT: " + std::to_string(result));
        compileResult->Release();
        source->Release();
        return false;
    }

    IDxcBlobUtf8* errors = nullptr;

    result = compileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);

    if (SUCCEEDED(result) && errors && errors->GetStringLength() > 0)
    {
        Log::Error(errors->GetStringPointer());
    }

    if (errors)
    {
        errors->Release();
    }

    if (FAILED(compileStatus))
    {
        compileResult->Release();

        source->Release();
        return false;
    }

    IDxcBlob* object = nullptr;

    result = compileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);

    if (FAILED(result))
    {
        compileResult->Release();
        source->Release();
        return false;
    }

    const char* bytecodeBegin = static_cast<const char*>(object->GetBufferPointer());
    const char* bytecodeEnd = bytecodeBegin + object->GetBufferSize();

    output.bytecode.assign(bytecodeBegin, bytecodeEnd);

    object->Release();

    compileResult->Release();
    source->Release();

    // Diagnostics (SHARC-in-ReSTIR PT investigation, off by default): ENGINE_DXIL_DUMP_DIR=<directory> writes each compiled
    // ReSTIRPT/ shader's DXIL there as <path>-<entry>.dxil, for offline disassembly (dxc -dumpbin).
    if(const char* dumpDirectory = std::getenv("ENGINE_DXIL_DUMP_DIR"); dumpDirectory && *dumpDirectory &&
        std::wstring_view(description.path).find(L"ReSTIRPT") != std::wstring_view::npos)
    {
        std::wstring name = std::wstring(description.path) + L"-" + description.entryPoint + L".dxil";

        for(wchar_t& character : name)
        {
            if(character == L'/' || character == L'\\' || character == L':')
            {
                character = L'_';
            }
        }

        std::error_code error;
        std::filesystem::create_directories(dumpDirectory, error);
        std::ofstream dump(std::filesystem::path(dumpDirectory) / name, std::ios::binary);
        dump.write(reinterpret_cast<const char*>(output.bytecode.data()), static_cast<std::streamsize>(output.bytecode.size()));
    }

    // Only a successful compilation is stored, so a failing source is never answered from the cache.
    output.cacheWritten = cache.Store(output.bytecode, dependencies);

    return true;
}

bool ShaderCompiler::CompileLibrary(
    const ShaderLibraryCompileDescription& description,
    ShaderLibraryCompileResult& output)
{
    return CompileLibrary(description, output, nullptr);
}

bool ShaderCompiler::CompileLibrary(
    const ShaderLibraryCompileDescription& description,
    ShaderLibraryCompileResult& output,
    std::string& diagnostics)
{
    diagnostics.clear();
    return CompileLibrary(description, output, &diagnostics);
}

bool ShaderCompiler::CompileLibrary(
    const ShaderLibraryCompileDescription& description,
    ShaderLibraryCompileResult& output,
    std::string* diagnostics)
{
    output.bytecode.clear();
    output.dependencies.clear();
    output.profile = description.profile;
    output.cacheStatus = ShaderLibraryCompileResult::CacheStatus::Disabled;
    output.cacheWritten = false;
    output.dxcInvoked = false;
    output.cacheReadMilliseconds = 0.0;
    output.dxcMilliseconds = 0.0;
    output.cacheWriteMilliseconds = 0.0;

    if (!m_utils || !m_compiler)
    {
        return false;
    }

    if (!description.path)
    {
        return false;
    }

    const auto cacheStart = std::chrono::steady_clock::now();
    std::filesystem::path shaderPath = description.path;
    std::wstring shaderDirectory = shaderPath.parent_path().wstring();
    const std::wstring profile = std::filesystem::path(GetLibraryTarget(description.profile)).wstring();
    std::vector<const wchar_t*> arguments = {description.path, L"-T", profile.c_str(), L"-HV", L"2021", L"-I",
        shaderDirectory.c_str()};

    // RayQuery 2M E3: a compute "library" is one entry point.
    if(description.profile == ShaderLibraryProfile::Compute66)
    {
        arguments.push_back(L"-E");
        arguments.push_back(ComputeLibraryEntryPoint);
    }

    // NVIDIA SHARC stores fp16 data (SharcTypes.h): only its own variant tree compiles with native 16-bit types.
    if(UsesNative16BitTypes(description.path))
    {
        arguments.push_back(L"-enable-16bit-types");
    }

    // M5.1 T-2.1: configuration B's ReSTIRPT/ tree with strict IEEE only on request (UsesStrictFloatingPoint; A1).
    if(UsesStrictFloatingPoint(description.path))
    {
        arguments.push_back(L"-Gis");
    }

    // M5.6 B3a: the ReSTIRPT/ tree's pre-B3a BSDF evaluation on request (UsesUnfusedBsdf).
    if(UsesUnfusedBsdf(description.path))
    {
        arguments.push_back(L"-D");
        arguments.push_back(L"RESTIR_PT_UNFUSED_BSDF=1");
    }

    // Audit T1-c: the ReSTIRPT/ tree's diagnostic counters only on request (UsesDiagnosticCounters).
    if(UsesDiagnosticCounters(description.path))
    {
        arguments.push_back(L"-D");
        arguments.push_back(L"RESTIR_PT_COUNTERS=1");
    }

    if(DisablesSmallLightPath(description.path))
    {
        arguments.push_back(L"-D");
        arguments.push_back(L"RESTIR_PT_NO_SMALL_LIGHTS=1");
    }

    if(UsesListPrototype(description.path))
    {
        arguments.push_back(L"-D");
        arguments.push_back(L"RESTIR_PT_LIST_PROTOTYPE=1");
    }

    std::error_code sameDirectoryError;

    // Root-level libraries keep exactly their original arguments.
    if(description.includeDirectory &&
        !std::filesystem::equivalent(shaderDirectory, description.includeDirectory, sameDirectoryError))
    {
        arguments.push_back(L"-I");
        arguments.push_back(description.includeDirectory);
    }

    // Shader variants (ShaderVariants) compile from their generated text instead of a file.
    const std::optional<VariantSource> variant = FindVariantSource(description.path, description.includeDirectory);

    if(!variant)
    {
        ReportLibraryError(diagnostics, "Shader variant has an invalid base chain: " +
            std::filesystem::path(description.path).string());
        return false;
    }

    DxilLibraryCache cache(description, arguments, variant->text);
    const bool cacheHit = cache.Load(m_utils, output);
    output.cacheReadMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - cacheStart).count();
    output.cacheStatus = cache.Enabled() ? ShaderLibraryCompileResult::CacheStatus::Miss :
        ShaderLibraryCompileResult::CacheStatus::Disabled;

    if(cacheHit)
    {
        output.cacheStatus = ShaderLibraryCompileResult::CacheStatus::Hit;
        return true;
    }

    IDxcBlobEncoding* source = nullptr;

    HRESULT result = LoadMainSource(m_utils, description.path, *variant, &source);

    if (FAILED(result))
    {
        ReportLibraryError(diagnostics, "Cannot read required shader source: " +
            std::filesystem::path(description.path).string() + " (HRESULT " + std::to_string(result) +
            "). Keep the bundled assets/Shaders directory beside Engine.exe.");
        return false;
    }

    DxcBuffer sourceBuffer{};
    sourceBuffer.Ptr = source->GetBufferPointer();

    sourceBuffer.Size = source->GetBufferSize();
    sourceBuffer.Encoding = DXC_CP_UTF8;

    IDxcIncludeHandler* includeHandler = nullptr;

    result = m_utils->CreateDefaultIncludeHandler(&includeHandler);

    if (FAILED(result))
    {
        source->Release();
        return false;
    }

    IDxcResult* compileResult = nullptr;

    // A variant's text is in the cache identity instead of the dependencies: it has no file to check.
    if(!variant->variant)
    {
        RecordDependency(output.dependencies, description.path, source);
    }

    RecordingIncludeHandler recordingIncludeHandler(includeHandler, output.dependencies, m_utils,
        description.includeDirectory);

    const auto compileStart = std::chrono::steady_clock::now();
    output.dxcInvoked = true;
    result =
        m_compiler->Compile(&sourceBuffer, arguments.data(), static_cast<UINT32>(arguments.size()),
                            &recordingIncludeHandler, IID_PPV_ARGS(&compileResult));
    output.dxcMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - compileStart).count();

    includeHandler->Release();

    if (FAILED(result))
    {
        source->Release();
        return false;
    }

    HRESULT compileStatus = S_OK;
    result = compileResult->GetStatus(&compileStatus);

    if (FAILED(result))
    {
        ReportLibraryError(diagnostics,
            "Failed to retrieve DXC shader-library compilation status (GetStatus). HRESULT: " + std::to_string(result));
        compileResult->Release();

        source->Release();
        return false;
    }

    IDxcBlobUtf8* errors = nullptr;

    result = compileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);

    if (SUCCEEDED(result) && errors && errors->GetStringLength() > 0)
    {
        ReportLibraryError(diagnostics, errors->GetStringPointer());
    }

    if (errors)
    {
        errors->Release();
    }

    if (FAILED(compileStatus))
    {
        compileResult->Release();
        source->Release();
        return false;
    }

    IDxcBlob* object = nullptr;

    result = compileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);

    if (FAILED(result))
    {
        compileResult->Release();
        source->Release();
        return false;
    }

    const char* bytecodeBegin = static_cast<const char*>(object->GetBufferPointer());

    const char* bytecodeEnd = bytecodeBegin + object->GetBufferSize();

    output.bytecode.assign(bytecodeBegin, bytecodeEnd);

    object->Release();
    compileResult->Release();

    source->Release();

    const auto writeStart = std::chrono::steady_clock::now();
    output.cacheWritten = cache.Store(output);
    output.cacheWriteMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - writeStart).count();

    return true;
}

const char* ShaderCompiler::GetLibraryTarget(ShaderLibraryProfile profile)
{
    switch(profile)
    {
    case ShaderLibraryProfile::Library69:
        return "lib_6_9";
    case ShaderLibraryProfile::Compute66:
        return "cs_6_6";
    case ShaderLibraryProfile::Library66:
        break;
    }

    return "lib_6_6";
}

ShaderLibraryProfile ShaderCompiler::GetLibraryProfile(const std::filesystem::path& libraryPath)
{
    const std::filesystem::path fileName = libraryPath.filename();

    // RayQuery 2M E3: the ReSTIR DI direct pass's compute shaders.
    if(!libraryPath.empty() && *libraryPath.begin() == "DirectLighting")
    {
        return ShaderLibraryProfile::Compute66;
    }

    if(fileName == "PathTraceReferenceSM69.hlsl" || fileName == "PathTraceReferenceSERSM69.hlsl" ||
        fileName == "PathTraceReferenceRawDiffuseHitDistanceSM69.hlsl" ||
        fileName == "PathTraceReferenceRawDiffuseHitDistanceSERSM69.hlsl" ||
        fileName == "PathTraceReferencePsrSM69.hlsl" || fileName == "PathTraceReferencePsrSERSM69.hlsl" ||
        fileName == "PathTraceReferencePsrGBufferSM69.hlsl" || fileName == "PathTraceReferencePsrGBufferSERSM69.hlsl" ||
        fileName == "PathTraceReferenceRawDiffuseHitDistancePsrGBufferSM69.hlsl" ||
        fileName == "PathTraceReferenceRawDiffuseHitDistancePsrGBufferSERSM69.hlsl")
    {
        return ShaderLibraryProfile::Library69;
    }

    // NVIDIA SHARC's variant tree follows the same naming: SM69 and SERSM69 wrappers are lib_6_9.
    if(UsesNative16BitTypes(libraryPath) && fileName.string().ends_with("SM69.hlsl"))
    {
        return ShaderLibraryProfile::Library69;
    }

    return ShaderLibraryProfile::Library66;
}

ShaderCompiler::ProductionArgumentsScope::ProductionArgumentsScope() :
    m_previous(t_productionArguments)
{
    t_productionArguments = true;
}

ShaderCompiler::ProductionArgumentsScope::~ProductionArgumentsScope()
{
    t_productionArguments = m_previous;
}

bool ShaderCompiler::UsesNative16BitTypes(const std::filesystem::path& shaderPath)
{
    bool restirPtTree = false;

    for(const std::filesystem::path& component : shaderPath)
    {
        if(component == "SharcNv")
        {
            return true;
        }

        if(component == "ReSTIRPT")
        {
            restirPtTree = true;
        }
    }

    // Cooked-game port: ReSTIR PT's SHARC twins (ShaderVariantTable.inl, "ReSTIRPT/...Sharc.hlsl") read and write NVIDIA
    // SHARC's packed fp16 data like its own shaders.
    return restirPtTree && shaderPath.stem().wstring().ends_with(L"Sharc");
}

bool ShaderCompiler::UsesStrictFloatingPoint(const std::filesystem::path& shaderPath)
{
    // Optimization A1 (user-approved 2026-11-23): the ReSTIRPT/ tree compiles without -Gis by default (Demo -4.6 ms,
    // Tri-Test -7.9 ms; gates pass, no visible difference). ENGINE_RESTIR_PT_STRICT_FP=1 restores strict IEEE for
    // diagnostics and comparisons (the M5.1 exact-identity tests need it). The persistent cache keys on the arguments, so
    // the two variants never mix.
    const char* const strict = DevelopmentSwitch("ENGINE_RESTIR_PT_STRICT_FP");

    if(strict == nullptr || std::string(strict) != "1")
    {
        return false;
    }

    for(const std::filesystem::path& component : shaderPath)
    {
        if(component == "ReSTIRPT")
        {
            return true;
        }
    }

    return false;
}

bool ShaderCompiler::UsesDiagnosticCounters(const std::filesystem::path& shaderPath)
{
    // Audit T1-c: the ReSTIRPT/ tree compiles its diagnostic counters only on request (ENGINE_RESTIR_PT_COUNTERS=1, the
    // runtime switch that also turns them on, or ENGINE_RESTIR_PT_DIAGNOSTIC_COUNTERS=1 for harness runs that read them).
    // The persistent cache keys on the arguments.
    const auto requested = [](const char* name)
    {
        const char* value = DevelopmentSwitch(name);
        return value != nullptr && std::string(value) == "1";
    };

    if(!requested("ENGINE_RESTIR_PT_COUNTERS") && !requested("ENGINE_RESTIR_PT_DIAGNOSTIC_COUNTERS"))
    {
        return false;
    }

    for(const std::filesystem::path& component : shaderPath)
    {
        if(component == "ReSTIRPT")
        {
            return true;
        }
    }

    return false;
}

bool ShaderCompiler::DisablesSmallLightPath(const std::filesystem::path& shaderPath)
{
    // Audit T1-b: the small-light NEE path (RestirPtInitialBody.hlsli) is on unless ENGINE_RESTIR_PT_SMALL_LIGHTS=0.
    const char* value = DevelopmentSwitch("ENGINE_RESTIR_PT_SMALL_LIGHTS");

    if(value == nullptr || std::string(value) != "0")
    {
        return false;
    }

    for(const std::filesystem::path& component : shaderPath)
    {
        if(component == "ReSTIRPT")
        {
            return true;
        }
    }

    return false;
}

bool ShaderCompiler::UsesUnfusedBsdf(const std::filesystem::path& shaderPath)
{
    // M5.6 B3a: the ReSTIRPT/ tree compiles with RESTIR_PT_UNFUSED_BSDF (the pre-B3a evaluation: production unchanged)
    // unless ENGINE_RESTIR_PT_FUSED_BSDF=1 selects the fused evaluation (B3a measured no gain beyond process variance;
    // awaiting review). The persistent cache keys on the arguments.
    const char* const fused = DevelopmentSwitch("ENGINE_RESTIR_PT_FUSED_BSDF");

    if(fused != nullptr && std::string(fused) == "1")
    {
        return false;
    }

    for(const std::filesystem::path& component : shaderPath)
    {
        if(component == "ReSTIRPT")
        {
            return true;
        }
    }

    return false;
}

bool ShaderCompiler::DependenciesUnchanged(const std::vector<ShaderSourceDependency>& dependencies)
{
    // Every library and pipeline adoption checks its whole include tree here (a Lighting Mode switch adopts dozens), so
    // each file is read in one block: a character-by-character stream iterator made the switching frame's checks cost
    // several milliseconds per library. The comparison is still the full contents.
    std::string contents;

    for(const ShaderSourceDependency& dependency : dependencies)
    {
        std::ifstream file(dependency.path, std::ios::binary | std::ios::ate);

        if(file.is_open() != dependency.exists)
        {
            return false;
        }

        if(!dependency.exists)
        {
            continue;
        }

        const std::streamoff size = file.tellg();

        if(size < 0 || static_cast<std::uint64_t>(size) != dependency.contents.size())
        {
            return false;
        }

        contents.resize(static_cast<std::size_t>(size));
        file.seekg(0);

        if(!file.read(contents.data(), static_cast<std::streamsize>(size)) || contents != dependency.contents)
        {
            return false;
        }
    }

    return true;
}

const wchar_t* ShaderCompiler::GetShaderProfile(ShaderStage stage) const
{
    switch (stage)
    {
    case ShaderStage::Vertex:
        return L"vs_6_6";
    case ShaderStage::Pixel:
        return L"ps_6_6";
    case ShaderStage::Compute:
        return L"cs_6_6";
    }

    return nullptr;
}
