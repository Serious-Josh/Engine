#include "DxilLibraryCache.h"
#include "Platform/Windows/AtomicFileWrite.h"
#include "Debug/TransitionTiming.h"

#include <bcrypt.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace
{
    using Microsoft::WRL::ComPtr;
    using Digest = std::array<unsigned char, 32>;
    // 2 (M0c.5 Stage 5.4): shader-root-relative identity and dependencies, no working directory; any target.
    constexpr std::uint32_t FormatVersion = 2;
    // Marks a path stored relative to the shader root (never the prefix of an absolute path).
    constexpr std::string_view RootMarker = "<shader-root>/";
    constexpr std::size_t MaximumEntryBytes = 128 * 1024 * 1024;
    constexpr std::size_t MaximumBytecodeBytes = 64 * 1024 * 1024;
    constexpr std::size_t MaximumSourceBytes = 16 * 1024 * 1024;
    constexpr std::size_t MaximumIdentityBytes = 256 * 1024;
    constexpr std::size_t MaximumPathBytes = 64 * 1024;
    constexpr std::uint32_t MaximumDependencies = 4096;

    Digest Hash(std::string_view bytes)
    {
        Digest digest{};

        if(bytes.size() > (std::numeric_limits<ULONG>::max)() ||
            BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size())) < 0)
        {
            throw std::runtime_error("SHA-256 unavailable");
        }

        return digest;
    }

    std::string Hex(const Digest& digest)
    {
        constexpr char digits[] = "0123456789abcdef";
        std::string text;

        for(const auto byte : digest)
        {
            text.push_back(digits[byte >> 4]);
            text.push_back(digits[byte & 15]);
        }

        return text;
    }

    std::string ReadFile(const std::filesystem::path& path, std::size_t maximum)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        const auto size = file.tellg();

        if(!file || size < 0 || static_cast<std::uint64_t>(size) > maximum)
        {
            throw std::runtime_error("Unreadable or oversized cache input");
        }

        std::string bytes(static_cast<std::size_t>(size), '\0');
        file.seekg(0);

        if(!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        {
            throw std::runtime_error("Truncated cache input");
        }

        return bytes;
    }


    std::string Utf8(const wchar_t* value)
    {
        const auto text = std::filesystem::path(value).u8string();
        return std::string(text.begin(), text.end());
    }

    std::string Utf8(const std::filesystem::path& path)
    {
        const auto text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    // A path inside the shader root as "<shader-root>/relative/path"; anything else (no root, or outside it) absolute.
    std::string RootRelative(const std::filesystem::path& root, const std::filesystem::path& path)
    {
        const std::filesystem::path absolute = std::filesystem::absolute(path).lexically_normal();

        if(!root.empty())
        {
            const std::filesystem::path relative = absolute.lexically_relative(root);

            if(relative == ".")
            {
                return std::string(RootMarker);
            }

            if(!relative.empty() && !relative.is_absolute() && *relative.begin() != "..")
            {
                return std::string(RootMarker) + Utf8(std::filesystem::path(relative.generic_wstring()));
            }
        }

        return Utf8(absolute);
    }

    std::filesystem::path ResolveStored(const std::filesystem::path& root, std::string_view text)
    {
        if(text.starts_with(RootMarker))
        {
            if(root.empty())
            {
                throw std::runtime_error("Root-relative cache path without a shader root");
            }

            const std::string_view relative = text.substr(RootMarker.size());
            return (root / std::filesystem::path(std::u8string(relative.begin(), relative.end()))).lexically_normal();
        }

        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    // An argument naming a path inside the shader root is written relative to it; other arguments as they are.
    std::string ArgumentIdentity(const std::filesystem::path& root, const wchar_t* argument)
    {
        const std::filesystem::path path(argument);
        return path.is_absolute() ? RootRelative(root, path) : Utf8(path);
    }

    std::wstring Environment(const wchar_t* name)
    {
        const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);

        if(size == 0)
        {
            return {};
        }

        std::wstring value(size, L'\0');
        const DWORD copied = GetEnvironmentVariableW(name, value.data(), size);

        if(copied == 0 || copied >= size)
        {
            throw std::runtime_error("Environment changed during cache configuration");
        }

        value.resize(copied);
        return value;
    }

    // ENGINE_DXIL_CACHE_DIR names the directory ("off" disables the cache); by default %LOCALAPPDATA%\Engine\ShaderCache\DXIL\v2.
    std::filesystem::path CacheDirectory()
    {
        auto directory = Environment(L"ENGINE_DXIL_CACHE_DIR");

        if(_wcsicmp(directory.c_str(), L"off") == 0)
        {
            return {};
        }

        if(directory.empty())
        {
            const auto local = Environment(L"LOCALAPPDATA");

            if(local.empty())
            {
                return {};
            }

            directory = (std::filesystem::path(local) / L"Engine/ShaderCache/DXIL/v2").wstring();
        }

        return std::filesystem::absolute(directory);
    }

    void PutInteger(std::string& bytes, std::uint32_t value)
    {
        for(unsigned shift = 0; shift < 32; shift += 8)
        {
            bytes.push_back(static_cast<char>(value >> shift));
        }
    }

    void PutText(std::string& bytes, std::string_view value)
    {
        PutInteger(bytes, static_cast<std::uint32_t>(value.size()));
        bytes.append(value);
    }

    void PutDigest(std::string& bytes, const Digest& digest)
    {
        bytes.append(reinterpret_cast<const char*>(digest.data()), digest.size());
    }

    class Reader
    {
    public:
        explicit Reader(std::string_view bytes) : m_bytes(bytes)
        {
        }

        std::string_view Take(std::size_t size)
        {
            if(size > m_bytes.size())
            {
                throw std::runtime_error("Truncated cache record");
            }

            const auto value = m_bytes.substr(0, size);
            m_bytes.remove_prefix(size);
            return value;
        }

        std::uint32_t Integer()
        {
            const auto bytes = Take(4);
            std::uint32_t value = 0;

            for(unsigned index = 0; index < 4; ++index)
            {
                value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index])) << (8 * index);
            }

            return value;
        }

        std::string_view Text(std::size_t maximum)
        {
            const auto size = Integer();

            if(size > maximum)
            {
                throw std::runtime_error("Oversized cache field");
            }

            return Take(size);
        }

        bool Matches(const Digest& digest)
        {
            return Take(digest.size()) == std::string_view(
                reinterpret_cast<const char*>(digest.data()), digest.size());
        }

        bool Empty() const
        {
            return m_bytes.empty();
        }

    private:
        std::string_view m_bytes;
    };

    std::filesystem::path ModulePath(HMODULE module)
    {
        std::wstring path(32768, L'\0');
        const DWORD size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));

        if(size == 0 || size >= path.size())
        {
            throw std::runtime_error("Cannot identify loaded DXC module");
        }

        path.resize(size);
        return path;
    }

    // DXC loads its external validator beside dxcompiler.dll. Hold both modules for the process lifetime,
    // and fingerprint the actual loader-resolved files once. If this cannot be established, disable caching.
    // Initialize already verifies the version/commit/release against the pinned build before we get here.
    struct CompilerIdentity
    {
        std::string bytes;
        DxcCreateInstanceProc createValidator = nullptr;

        CompilerIdentity() noexcept
        {
            try
            {
                HMODULE compiler = nullptr;

                if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(&DxcCreateInstance), &compiler))
                {
                    return;
                }

                const auto compilerPath = ModulePath(compiler);
                HMODULE validator = LoadLibraryExW((compilerPath.parent_path() / L"dxil.dll").c_str(),
                    nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

                if(!validator)
                {
                    return;
                }

                createValidator = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(validator, "DxcCreateInstance"));

                if(!createValidator)
                {
                    return;
                }

                PutText(bytes, ENGINE_DXC_VERSION);
                PutInteger(bytes, ENGINE_DXC_VERSION_MAJOR);
                PutInteger(bytes, ENGINE_DXC_VERSION_MINOR);
                PutInteger(bytes, ENGINE_DXC_COMMIT_COUNT);
                PutText(bytes, ENGINE_DXC_COMMIT_HASH);
                // M0c.5 Stage 5.4: the files' contents identify them; their locations (each install's own copy) do not.
                PutDigest(bytes, Hash(ReadFile(compilerPath, MaximumEntryBytes)));
                const auto validatorPath = ModulePath(validator);
                PutDigest(bytes, Hash(ReadFile(validatorPath, MaximumEntryBytes)));
            }
            catch(const std::exception&)
            {
                bytes.clear();
            }
        }
    };

    const CompilerIdentity& Compiler()
    {
        static const CompilerIdentity identity;
        return identity;
    }

    bool ValidateContainer(IDxcUtils* utils, std::string_view bytes)
    {
        if(bytes.size() < 32 || bytes.substr(0, 4) != "DXBC")
        {
            return false;
        }

        // Check the entire container and DXIL program with the deployed external validator. This does
        // not invoke the compiler, mutate the bytecode, or compare driver-assigned shader identifiers.
        ComPtr<IDxcBlobEncoding> blob;
        ComPtr<IDxcValidator> validator;
        ComPtr<IDxcOperationResult> result;
        HRESULT status = E_FAIL;

        return SUCCEEDED(utils->CreateBlob(bytes.data(), static_cast<UINT32>(bytes.size()), DXC_CP_ACP, &blob)) &&
            Compiler().createValidator &&
            SUCCEEDED(Compiler().createValidator(CLSID_DxcValidator, IID_PPV_ARGS(&validator))) &&
            SUCCEEDED(validator->Validate(blob.Get(), DxcValidatorFlags_Default, &result)) &&
            result && SUCCEEDED(result->GetStatus(&status)) && SUCCEEDED(status);
    }

    // A failed open is not sufficient proof of absence: access denied and sharing violations must
    // invalidate a hit. Only missing files/parents can satisfy a recorded negative include lookup.
    bool Absent(const std::filesystem::path& path)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());

        if(attributes != INVALID_FILE_ATTRIBUTES)
        {
            return false;
        }

        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
}

DxilLibraryCache::DxilLibraryCache(const ShaderLibraryCompileDescription& description,
    std::span<const wchar_t* const> arguments) noexcept :
    DxilLibraryCache(description.usePersistentCache, description.path, description.includeDirectory,
        description.profile == ShaderLibraryProfile::Library69 ? "lib_6_9" : "lib_6_6", arguments)
{
}

DxilLibraryCache::DxilLibraryCache(bool enabled, const wchar_t* source, const wchar_t* shaderRoot, std::string_view target,
    std::span<const wchar_t* const> arguments) noexcept
{
    if(!enabled || !source || !shaderRoot || *shaderRoot == L'\0')
    {
        return;
    }

    try
    {
        const std::filesystem::path directory = CacheDirectory();
        const auto& compiler = Compiler();

        if(directory.empty() || compiler.bytes.empty())
        {
            return;
        }

        m_root = std::filesystem::absolute(shaderRoot).lexically_normal();
        PutInteger(m_identity, FormatVersion);
        PutText(m_identity, compiler.bytes);
        PutText(m_identity, target);
        PutInteger(m_identity, GetACP());
        PutText(m_identity, RootRelative(m_root, source));
        PutInteger(m_identity, static_cast<std::uint32_t>(arguments.size()));

        for(const auto argument : arguments)
        {
            PutText(m_identity, ArgumentIdentity(m_root, argument));
        }

        if(m_identity.size() > MaximumIdentityBytes)
        {
            return;
        }

        m_file = directory / (Hex(Hash(m_identity)) + ".edxc");
    }
    catch(const std::exception&)
    {
        m_file.clear();
    }
}

bool DxilLibraryCache::Load(IDxcUtils* utils, std::vector<char>& outputBytecode,
    std::vector<ShaderSourceDependency>& outputDependencies) const noexcept
{
    if(!Enabled())
    {
        return false;
    }

    try
    {
        const auto bytes = ReadFile(m_file, MaximumEntryBytes);

        if(bytes.size() < 32)
        {
            return false;
        }

        const std::string_view payload(bytes.data(), bytes.size() - 32);
        Reader trailer(std::string_view(bytes).substr(payload.size()));

        if(!trailer.Matches(Hash(payload)))
        {
            return false;
        }

        Reader reader(payload);

        if(reader.Take(4) != "EDXC" || reader.Integer() != FormatVersion ||
            reader.Text(MaximumIdentityBytes) != m_identity)
        {
            return false;
        }

        const auto count = reader.Integer();

        if(count == 0 || count > MaximumDependencies)
        {
            return false;
        }

        std::vector<ShaderSourceDependency> dependencies;
        std::size_t sourceBytes = 0;

        for(std::uint32_t index = 0; index < count; ++index)
        {
            const auto pathText = reader.Text(MaximumPathBytes);

            if(pathText.empty() || pathText.find('\0') != std::string_view::npos)
            {
                return false;
            }

            ShaderSourceDependency dependency;
            dependency.path = ResolveStored(m_root, pathText);
            const auto exists = reader.Integer();

            if(!dependency.path.is_absolute() || exists > 1)
            {
                return false;
            }

            dependency.exists = exists != 0;

            if(dependency.exists)
            {
                dependency.contents = ReadFile(dependency.path, MaximumSourceBytes);
                sourceBytes += dependency.contents.size();

                if(sourceBytes > MaximumEntryBytes || !reader.Matches(Hash(dependency.contents)))
                {
                    return false;
                }
            }
            else if(!Absent(dependency.path) || !reader.Matches(Hash({})))
            {
                return false;
            }

            dependencies.push_back(std::move(dependency));
        }

        const auto bytecode = reader.Text(MaximumBytecodeBytes);

        if(!reader.Empty() || !ValidateContainer(utils, bytecode) ||
            !ShaderCompiler::DependenciesUnchanged(dependencies))
        {
            return false;
        }

        // Commit only a complete, validated result. Existing freshness/revision gates receive the same
        // current source contents they would receive from RecordingIncludeHandler after a compilation.
        outputBytecode.assign(bytecode.begin(), bytecode.end());
        outputDependencies = std::move(dependencies);
        return true;
    }
    catch(const std::exception&)
    {
        return false;
    }
}

bool DxilLibraryCache::Store(const std::vector<char>& outputBytecode,
    const std::vector<ShaderSourceDependency>& outputDependencies) const noexcept
{
    if(!Enabled())
    {
        return false;
    }

    try
    {
        if(outputBytecode.empty() || outputBytecode.size() > MaximumBytecodeBytes ||
            outputDependencies.empty() || outputDependencies.size() > MaximumDependencies ||
            !ShaderCompiler::DependenciesUnchanged(outputDependencies))
        {
            return false;
        }

        std::string bytes = "EDXC";
        PutInteger(bytes, FormatVersion);
        PutText(bytes, m_identity);
        PutInteger(bytes, static_cast<std::uint32_t>(outputDependencies.size()));

        for(const auto& dependency : outputDependencies)
        {
            const auto path = RootRelative(m_root, dependency.path);

            if(path.size() > MaximumPathBytes || dependency.contents.size() > MaximumSourceBytes ||
                (!dependency.exists && !Absent(dependency.path)))
            {
                return false;
            }

            PutText(bytes, path);
            PutInteger(bytes, dependency.exists ? 1 : 0);
            PutDigest(bytes, Hash(dependency.contents));
        }

        PutText(bytes, std::string_view(outputBytecode.data(), outputBytecode.size()));
        PutDigest(bytes, Hash(bytes));

        if(bytes.size() > MaximumEntryBytes)
        {
            return false;
        }

        std::filesystem::create_directories(m_file.parent_path());
        std::string error;
        return AtomicFileWrite::Write(m_file, bytes, error);
    }
    catch(const std::exception&)
    {
        return false;
    }
}

void DxilLibraryCache::Report(const std::filesystem::path& path, const ShaderLibraryCompileResult& result)
{
    // ER.1, and the M0c.5 startup timeline: cache hits and DXC times of each library.
    if(!TransitionTiming::Enabled() && !TransitionTiming::StartupEnabled())
    {
        return;
    }

    const char* status = "disabled";

    if(result.cacheStatus == ShaderLibraryCompileResult::CacheStatus::Hit)
    {
        status = "hit";
    }
    else if(result.cacheStatus == ShaderLibraryCompileResult::CacheStatus::Miss)
    {
        status = "miss";
    }

    Log::Info("[ER1] DXIL_CACHE " + path.generic_string() + " " + status +
        " read_ms " + std::to_string(result.cacheReadMilliseconds) +
        " dxc " + std::to_string(result.dxcInvoked) + " dxc_ms " + std::to_string(result.dxcMilliseconds) +
        " written " + std::to_string(result.cacheWritten) +
        " write_ms " + std::to_string(result.cacheWriteMilliseconds));
}
