#include "Core/Log.h"
#include "ShaderCompiler.h"
#include "DxilLibraryCache.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace
{
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
    class RecordingIncludeHandler final : public IDxcIncludeHandler
    {
        public:
        RecordingIncludeHandler(IDxcIncludeHandler* inner, std::vector<ShaderSourceDependency>& dependencies) :
            m_inner(inner), m_dependencies(dependencies)
        {
        }

        HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR filename, IDxcBlob** includeSource) override
        {
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
        IDxcIncludeHandler* m_inner = nullptr;
        std::vector<ShaderSourceDependency>& m_dependencies;
    };
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

    // M0c.5 Stage 5.4: the persistent cache, for the same target and arguments. A miss compiles exactly as before.
    const auto cacheStart = std::chrono::steady_clock::now();
    const std::string target = std::filesystem::path(profile).string() + ":" + std::filesystem::path(description.entryPoint).string();
    const DxilLibraryCache cache(description.usePersistentCache, description.path, description.shaderRoot, target, arguments);
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

    HRESULT result = m_utils->LoadFile(description.path, nullptr, &source);

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
    // Every file read and include lookup, for the cache entry (as CompileLibrary records them).
    RecordDependency(dependencies, description.path, source);
    RecordingIncludeHandler recordingIncludeHandler(includeHandler, dependencies);

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
    std::error_code sameDirectoryError;

    // Root-level libraries keep exactly their original arguments.
    if(description.includeDirectory &&
        !std::filesystem::equivalent(shaderDirectory, description.includeDirectory, sameDirectoryError))
    {
        arguments.push_back(L"-I");
        arguments.push_back(description.includeDirectory);
    }

    DxilLibraryCache cache(description, arguments);
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

    HRESULT result = m_utils->LoadFile(description.path, nullptr, &source);

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
    RecordDependency(output.dependencies, description.path, source);
    RecordingIncludeHandler recordingIncludeHandler(includeHandler, output.dependencies);

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

    if(fileName == "PathTraceReferencePsrSharcEnergyTrainingSM69.hlsl" ||
        fileName == "PathTraceReferencePsrSharcEnergyTrainingSERSM69.hlsl" ||
        fileName == "PathTraceReferencePsrSharcEnergyReferenceSM69.hlsl" ||
        fileName == "PathTraceReferencePsrSharcEnergyReferenceSERSM69.hlsl" ||
        fileName == "PathTraceReferencePsrSharcRenderSM69.hlsl" ||
        fileName == "PathTraceReferencePsrSharcRenderSERSM69.hlsl" ||
        fileName == "PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSM69.hlsl" ||
        fileName == "PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSERSM69.hlsl" ||
        fileName == "PathTraceReferencePsrSharcTrainingSM69.hlsl" ||
        fileName == "PathTraceReferencePsrSharcTrainingSERSM69.hlsl" ||
        fileName == "PathTraceReferenceSM69.hlsl" || fileName == "PathTraceReferenceSERSM69.hlsl" ||
        fileName == "PathTraceReferenceRadianceCacheDeferredSM69.hlsl" ||
        fileName == "PathTraceReferenceRadianceCacheDeferredSERSM69.hlsl" ||
        fileName == "PathTraceReferenceRadianceCacheDeferredLateSM69.hlsl" ||
        fileName == "PathTraceReferenceRadianceCacheDeferredLateSERSM69.hlsl" ||
        fileName == "PathTraceReferenceRadianceCacheQuerySM69.hlsl" ||
        fileName == "PathTraceReferenceRadianceCacheQuerySERSM69.hlsl" ||
        fileName == "PathTraceReferenceRadianceCacheTrainingSM69.hlsl" ||
        fileName == "PathTraceReferenceRadianceCacheTrainingSERSM69.hlsl" ||
        fileName == "PathTraceReferenceRadianceCacheQueryTerminationSM69.hlsl" ||
        fileName == "PathTraceReferenceRadianceCacheQueryTerminationSERSM69.hlsl" ||
        fileName == "PathTraceReferenceRadianceCacheRenderSM69.hlsl" ||
        fileName == "PathTraceReferenceRadianceCacheRenderSERSM69.hlsl" ||
        fileName == "PathTraceReferenceRawDiffuseHitDistanceSM69.hlsl" ||
        fileName == "PathTraceReferenceRawDiffuseHitDistanceSERSM69.hlsl" ||
        fileName == "PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistanceSM69.hlsl" ||
        fileName == "PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistanceSERSM69.hlsl" ||
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

bool ShaderCompiler::UsesNative16BitTypes(const std::filesystem::path& shaderPath)
{
    for(const std::filesystem::path& component : shaderPath)
    {
        if(component == "SharcNv")
        {
            return true;
        }
    }

    return false;
}

bool ShaderCompiler::DependenciesUnchanged(const std::vector<ShaderSourceDependency>& dependencies)
{
    for(const ShaderSourceDependency& dependency : dependencies)
    {
        std::ifstream file(dependency.path, std::ios::binary);

        if(file.is_open() != dependency.exists)
        {
            return false;
        }

        if(dependency.exists &&
            std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()) != dependency.contents)
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
