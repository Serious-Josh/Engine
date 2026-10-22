#include "Core/Log.h"
#include "ShaderCompiler.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

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

        for(const ShaderSourceDependency& existing : dependencies)
        {
            if(existing.path == dependency.path)
            {
                return;
            }
        }

        dependency.exists = source != nullptr;

        if(source)
        {
            const char* begin = static_cast<const char*>(source->GetBufferPointer());
            dependency.contents.assign(begin, begin + source->GetBufferSize());
        }

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
    output.bytecode.clear();

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

    std::filesystem::path shaderPath = description.path;

    std::wstring shaderDirectory = shaderPath.parent_path().wstring();

    const wchar_t* arguments[] = {description.path, L"-E", description.entryPoint, L"-T", profile, L"-HV",
                                  L"2021",          L"-I", shaderDirectory.c_str()};

    IDxcResult* compileResult = nullptr;

    result =
        m_compiler->Compile(&sourceBuffer, arguments, static_cast<UINT32>(sizeof(arguments) / sizeof(arguments[0])),
                            includeHandler, IID_PPV_ARGS(&compileResult));

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

    if (!m_utils || !m_compiler)
    {
        return false;
    }

    if (!description.path)
    {
        return false;
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

    std::filesystem::path shaderPath = description.path;

    std::wstring shaderDirectory = shaderPath.parent_path().wstring();

    const wchar_t* profile = description.profile == ShaderLibraryProfile::Library69 ? L"lib_6_9" : L"lib_6_6";
    const wchar_t* arguments[] = {description.path, L"-T", profile, L"-HV", L"2021", L"-I", shaderDirectory.c_str()};

    IDxcResult* compileResult = nullptr;
    RecordDependency(output.dependencies, description.path, source);
    RecordingIncludeHandler recordingIncludeHandler(includeHandler, output.dependencies);

    result =
        m_compiler->Compile(&sourceBuffer, arguments, static_cast<UINT32>(sizeof(arguments) / sizeof(arguments[0])),
                            &recordingIncludeHandler, IID_PPV_ARGS(&compileResult));

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

    return true;
}

ShaderLibraryProfile ShaderCompiler::GetLibraryProfile(const std::filesystem::path& libraryPath)
{
    const std::filesystem::path fileName = libraryPath.filename();

    if(fileName == "PathTraceReferenceSM69.hlsl" || fileName == "PathTraceReSTIRDISM69.hlsl" ||
        fileName == "PathTraceReferenceSERSM69.hlsl" || fileName == "PathTraceReSTIRDISERSM69.hlsl" ||
        fileName == "PathTraceReSTIRDIExtractSM69.hlsl" || fileName == "PathTraceReSTIRDIExtractSERSM69.hlsl")
    {
        return ShaderLibraryProfile::Library69;
    }

    return ShaderLibraryProfile::Library66;
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
