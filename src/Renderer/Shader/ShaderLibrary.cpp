#include "ShaderLibrary.h"
#include "DxilLibraryCache.h"
#include "Debug/TransitionTiming.h"
#include "Core/Log.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <cstring>
#include <array>
#include <cstdint>

bool ShaderLibrary::Initialize(const std::filesystem::path& shaderRoot)
{
    m_packaged = false;
    m_shaderRoot = shaderRoot;

    if (!m_compiler.Initialize())
    {
        return false;
    }

    return true;
}

void ShaderLibrary::Shutdown()
{
    for (auto& [key, shader] : m_shaders)
    {
        shader->Unload();
    }

    m_shaders.clear();
    m_packagedBytecode.clear();

    m_compiler.Shutdown();
}

Shader* ShaderLibrary::Load(const ShaderDescription& description)
{
    if (!description.path || !description.entryPoint)
    {
        return nullptr;
    }

    ShaderKey key{};
    key.path = description.path;

    key.entryPoint = description.entryPoint;
    key.stage = description.stage;

    auto existingShader = m_shaders.find(key);

    if (existingShader != m_shaders.end())
    {
        return existingShader->second.get();
    }

    const std::filesystem::path shaderPath = m_shaderRoot / description.path;

    const std::wstring shaderPathString = shaderPath.wstring();
    const std::wstring entryPointString = std::filesystem::path(description.entryPoint).wstring();

    ShaderCompileDescription compileDescription{};
    compileDescription.path = shaderPathString.c_str();
    compileDescription.entryPoint = entryPointString.c_str();
    compileDescription.stage = description.stage;

    TransitionTiming::Scope er1Scope("Main-thread shader Compile " + key.path + ":" + key.entryPoint);
    ShaderCompileResult compileResult{};

    if (m_packaged)
    {
        const auto found = m_packagedBytecode.find(key.path + ":" + key.entryPoint + ":" + std::to_string(static_cast<int>(key.stage)));
        if(found == m_packagedBytecode.end()) { Log::Error("Missing packaged shader: " + key.path + ":" + key.entryPoint); return nullptr; }
        compileResult.bytecode = found->second;
    }
    else if (!m_compiler.Compile(compileDescription, compileResult))
    {
        return nullptr;
    }

    std::unique_ptr<Shader> shader = std::make_unique<Shader>();

    if (!shader->Initialize(compileResult.bytecode.data(), compileResult.bytecode.size()))
    {
        return nullptr;
    }

    Shader* shaderPointer = shader.get();

    m_shaders.emplace(std::move(key), std::move(shader));

    return shaderPointer;
}

bool ShaderLibrary::CompileLibrary(
    const std::filesystem::path& path,
    ShaderLibraryCompileResult& output)
{
    if(m_packaged)
    {
        std::string problem;

        if(!AcquirePackagedLibrary(path, output, problem))
        {
            Log::Error("Packaged DXR library " + path.generic_string() + " " + problem + ".");
            return false;
        }

        return true;
    }
    const std::filesystem::path shaderPath = m_shaderRoot / path;
    const std::wstring shaderPathString = shaderPath.wstring();
    const std::wstring shaderRootString = m_shaderRoot.wstring();

    ShaderLibraryCompileDescription description{};
    description.path = shaderPathString.c_str();
    description.profile = ShaderCompiler::GetLibraryProfile(path);
    description.includeDirectory = shaderRootString.c_str();
    description.usePersistentCache = true;

    ++m_libraryCompileCount;
    const bool succeeded = m_compiler.CompileLibrary(description, output);
    DxilLibraryCache::Report(path, output);
    return succeeded;
}

bool ShaderLibrary::CreateLibraryCompileRequest(
    const std::filesystem::path& path,
    ShaderLibraryCompileRequest& request)
{
    if(m_packaged)
    {
        return false;
    }

    request.libraryPath = path;
    request.sourcePath = m_shaderRoot / path;
    request.includeDirectory = m_shaderRoot;
    ++m_libraryCompileCount;
    return true;
}

bool ShaderLibrary::IsLibraryCurrent(
    const std::filesystem::path& path,
    const ShaderLibraryCompileResult& library) const
{
    if(library.bytecode.empty() || library.profile != ShaderCompiler::GetLibraryProfile(path))
    {
        return false;
    }

    return m_packaged || ShaderCompiler::DependenciesUnchanged(library.dependencies);
}

namespace
{
    void Put(std::vector<std::byte>& out, std::uint32_t value) { for(int i=0;i<4;++i) out.push_back(std::byte(value>>(8*i))); }
}

std::vector<std::byte> ShaderLibrary::CookBootstrap(
    const std::filesystem::path& root,
    std::span<const PackagedShaderEntry> shaders,
    std::span<const PackagedLibraryEntry> libraries,
    ShaderBootstrapStatistics* statistics)
{
    const auto started = std::chrono::steady_clock::now();
    ShaderCompiler compiler;
    if(!compiler.Initialize()) throw std::runtime_error("Cannot initialize offline DXC.");
    struct Cleanup { ShaderCompiler& c; ~Cleanup(){c.Shutdown();} } cleanup{compiler};
    std::map<std::string,std::vector<char>> entries;
    ShaderBootstrapStatistics cooked;

    // The arguments Load uses at runtime: the source under the shader root, its own directory as the include path.
    for(const PackagedShaderEntry& shader : shaders)
    {
        const auto path = (root / shader.path).wstring();
        const auto entryPoint = std::filesystem::path(shader.entryPoint).wstring();
        ShaderCompileResult result;

        if(!compiler.Compile({path.c_str(), entryPoint.c_str(), shader.stage}, result))
        {
            throw std::runtime_error(std::string("Offline shader compilation failed: ") + shader.path + ":" +
                shader.entryPoint + " (" + PackagedShaderFeatureName(shader.feature) + ")");
        }

        const std::string key = std::string(shader.path) + ":" + shader.entryPoint + ":" +
            std::to_string(static_cast<int>(shader.stage));

        if(!entries.emplace(key, std::move(result.bytecode)).second)
        {
            throw std::runtime_error("Duplicate packaged shader inventory entry: " + key);
        }

        ++cooked.shaders;
    }

    // The arguments CompileLibrary uses at runtime: the shader root as the second include directory (variant
    // subdirectories resolve the RTXDI SDK's angled includes through it) and the persistent DXIL cache.
    for(const PackagedLibraryEntry& library : libraries)
    {
        const auto path = (root / library.path).wstring();
        const auto includeDirectory = root.wstring();
        ShaderLibraryCompileDescription description{};
        description.path = path.c_str();
        description.profile = ShaderCompiler::GetLibraryProfile(library.path);
        description.includeDirectory = includeDirectory.c_str();
        description.usePersistentCache = true;
        ShaderLibraryCompileResult result;

        if(description.profile != ShaderLibraryProfile::Library66)
        {
            throw std::runtime_error("Packaged games use SM 6.6 DXR libraries only: " + library.path.generic_string());
        }

        if(!compiler.CompileLibrary(description, result))
        {
            throw std::runtime_error("Offline DXR compilation failed: " + library.path.generic_string() + " (" +
                PackagedShaderFeatureName(library.feature) + ")");
        }

        const std::string key = library.path.generic_string() + ":library";
        cooked.libraryCacheHits += result.cacheStatus == ShaderLibraryCompileResult::CacheStatus::Hit ? 1u : 0u;

        if(!entries.emplace(key, std::move(result.bytecode)).second)
        {
            throw std::runtime_error("Duplicate packaged library inventory entry: " + key);
        }

        ++cooked.libraries;
        cooked.optionalLibraries += library.optional ? 1u : 0u;
    }

    // The limits InitializePackaged admits; a cook that exceeds them fails instead of omitting anything.
    if(entries.size() > MaximumBootstrapRecords)
    {
        throw std::runtime_error("The shader bootstrap needs " + std::to_string(entries.size()) + " records; the limit is " +
            std::to_string(MaximumBootstrapRecords) + ".");
    }

    std::vector<std::byte> out; Put(out,0x31424853); Put(out,1); Put(out,static_cast<std::uint32_t>(entries.size()));
    for(const auto& [key,bytes]:entries)
    {
        if(bytes.size() > MaximumBootstrapRecordBytes)
        {
            throw std::runtime_error("Shader " + key + " is " + std::to_string(bytes.size()) +
                " bytes; a bootstrap record holds at most " + std::to_string(MaximumBootstrapRecordBytes) + ".");
        }

        cooked.largestRecordBytes = (std::max)(cooked.largestRecordBytes, static_cast<std::uint64_t>(bytes.size()));
        Put(out,static_cast<std::uint32_t>(key.size())); Put(out,static_cast<std::uint32_t>(bytes.size()));
        for(auto c:key) out.push_back(std::byte(c));
        for(auto c:bytes) out.push_back(std::byte(c));
    }

    if(out.size() > MaximumBootstrapBytes)
    {
        throw std::runtime_error("The shader bootstrap is " + std::to_string(out.size()) + " bytes; the limit is " +
            std::to_string(MaximumBootstrapBytes) + ".");
    }

    cooked.bytes = out.size();
    cooked.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    if(statistics)
    {
        *statistics = cooked;
    }

    return out;
}
bool ShaderLibrary::InitializePackaged(std::span<const std::byte> payload)
{
    m_packaged = true; m_shaderRoot.clear(); m_packagedBytecode.clear();
    try
    {
        if(payload.size()>MaximumBootstrapBytes) throw std::runtime_error("Shader bundle exceeds 64 MiB.");
        size_t cursor=0;
        const auto get=[&]() { if(payload.size()-cursor<4) throw std::runtime_error("Truncated shader bundle."); std::uint32_t v=0; for(int i=0;i<4;++i) v|=std::uint32_t(payload[cursor++])<<(8*i); return v; };
        if(get()!=0x31424853 || get()!=1) throw std::runtime_error("Unsupported shader bootstrap format.");
        const auto count=get(); if(count==0 || count>MaximumBootstrapRecords) throw std::runtime_error("Invalid shader count.");
        for(std::uint32_t i=0;i<count;++i)
        {
            const auto name=get(), bytes=get();
            if(name==0 || name>256 || bytes<4 || bytes>MaximumBootstrapRecordBytes || std::uint64_t(name)+bytes>payload.size()-cursor) throw std::runtime_error("Invalid shader record.");
            std::string key(reinterpret_cast<const char*>(payload.data()+cursor),name); cursor+=name;
            const auto* begin=reinterpret_cast<const char*>(payload.data()+cursor);
            if(std::memcmp(begin,"DXBC",4)!=0 || !m_packagedBytecode.emplace(key,std::vector<char>(begin,begin+bytes)).second) throw std::runtime_error("Invalid/duplicate DXIL record.");
            cursor+=bytes;
        }
        if(cursor!=payload.size()) throw std::runtime_error("Trailing shader bundle bytes.");
        return true;
    }
    catch(const std::exception& e) { Log::Error(std::string("Packaged shader bootstrap: ")+e.what()); m_packagedBytecode.clear(); return false; }
}

namespace
{
    std::uint32_t ReadWord(const std::vector<char>& bytes, std::size_t offset)
    {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, 4);
        return value;
    }

    // A DXBC container (header, part offsets, parts) whose DXIL program part is a library of at most the profile's
    // shader model. Only structure is checked; the driver validates the bitcode itself at pipeline creation.
    std::string DxilLibraryContainerProblem(const std::vector<char>& bytes, ShaderLibraryProfile profile)
    {
        constexpr std::size_t HeaderBytes = 32;

        if(bytes.size() < HeaderBytes || std::memcmp(bytes.data(), "DXBC", 4) != 0)
        {
            return "is corrupt (not a DXIL container)";
        }

        const std::uint32_t totalBytes = ReadWord(bytes, 24);
        const std::uint32_t partCount = ReadWord(bytes, 28);

        if(totalBytes != bytes.size() || partCount == 0 || partCount > 64 || HeaderBytes + partCount * 4ull > bytes.size())
        {
            return "is corrupt (its container header does not describe its " + std::to_string(bytes.size()) + " bytes)";
        }

        for(std::uint32_t part = 0; part < partCount; ++part)
        {
            const std::uint32_t offset = ReadWord(bytes, HeaderBytes + part * 4ull);

            if(offset < HeaderBytes || offset + 8ull > bytes.size() || offset + 8ull + ReadWord(bytes, offset + 4) > bytes.size())
            {
                return "is corrupt (container part " + std::to_string(part) + " lies outside it)";
            }

            if(std::memcmp(bytes.data() + offset, "DXIL", 4) != 0 || ReadWord(bytes, offset + 4) < 4)
            {
                continue;
            }

            // DxilProgramHeader::ProgramVersion: kind in bits 16-31, major in 4-7, minor in 0-3. Kind 6 is a library.
            const std::uint32_t version = ReadWord(bytes, offset + 8);
            const std::uint32_t kind = version >> 16;
            const std::uint32_t model = ((version >> 4) & 0xF) * 10 + (version & 0xF);
            const std::uint32_t supported = profile == ShaderLibraryProfile::Library69 ? 69u : 66u;

            if(kind != 6 || model > supported)
            {
                return "is incompatible (a shader of kind " + std::to_string(kind) + ", model " +
                    std::to_string(model / 10) + "." + std::to_string(model % 10) + "; this game needs a library of model " +
                    std::to_string(supported / 10) + "." + std::to_string(supported % 10) + ")";
            }

            return {};
        }

        return "is corrupt (it has no DXIL program part)";
    }
}

bool ShaderLibrary::AcquirePackagedLibrary(const std::filesystem::path& path, ShaderLibraryCompileResult& output,
    std::string& problem) const
{
    const auto found = m_packagedBytecode.find(path.generic_string() + ":library");

    if(!m_packaged || found == m_packagedBytecode.end())
    {
        problem = "is not in this game's shaders";
        return false;
    }

    // Cooked with the same mapping (CookBootstrap), so the record's key decides its profile.
    const ShaderLibraryProfile profile = ShaderCompiler::GetLibraryProfile(path);
    problem = DxilLibraryContainerProblem(found->second, profile);

    if(!problem.empty())
    {
        return false;
    }

    output = {};
    output.bytecode = found->second;
    output.profile = profile;
    return true;
}

std::vector<std::string> ShaderLibrary::GetLoadedShaderKeys() const
{
    std::vector<std::string> keys;

    for(const auto& [key, shader] : m_shaders)
    {
        keys.push_back(key.path + ":" + key.entryPoint + ":" + std::to_string(static_cast<int>(key.stage)));
    }

    std::sort(keys.begin(), keys.end());
    return keys;
}
