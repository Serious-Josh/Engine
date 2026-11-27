#include "ShaderLibrary.h"
#include "DxilLibraryCache.h"
#include "Debug/TransitionTiming.h"
#include "Core/Log.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <windows.h>
#include <cstring>
#include <array>
#include <optional>
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

std::size_t ShaderLibrary::AdoptShaders(ShaderLibrary& other)
{
    std::size_t adopted = 0;

    for(auto& [key, shader] : other.m_shaders)
    {
        if(shader && m_shaders.try_emplace(key, std::move(shader)).second)
        {
            ++adopted;
        }
    }

    other.m_shaders.clear();
    return adopted;
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
    const std::wstring shaderRootString = m_shaderRoot.wstring();

    ShaderCompileDescription compileDescription{};
    compileDescription.path = shaderPathString.c_str();
    compileDescription.entryPoint = entryPointString.c_str();
    compileDescription.stage = description.stage;
    // M0c.5 Stage 5.4: every runtime library (the render thread's and the workers' private ones) uses the persistent cache.
    compileDescription.shaderRoot = shaderRootString.c_str();
    compileDescription.usePersistentCache = true;

    // M0c.5 Stage 5.4: the scope is named by what the load did (a cache hit or DXC), and runs on whichever thread loads.
    std::optional<TransitionTiming::Scope> er1Scope;
    er1Scope.emplace("Shader load " + key.path + ":" + key.entryPoint, 0.1, TransitionTiming::TransitionsAndStartup);
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
    else
    {
        if(compileResult.cacheHit)
        {
            ++m_cacheHits;
        }
        else
        {
            ++m_compiles;
        }

        er1Scope->AppendName(compileResult.cacheHit ? " (cache hit)" : " (DXC)");
    }

    std::unique_ptr<Shader> shader = std::make_unique<Shader>();

    if (!shader->Initialize(compileResult.bytecode.data(), compileResult.bytecode.size()))
    {
        return nullptr;
    }

    shader->SetName(key.path + ":" + key.entryPoint);
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

// Startup work (4.4): the cook compiles its inventory on a thread pool, each thread with its own DXC and its own
// production-arguments scope (thread-local), and every entry (graphics and compute shaders too, not only the DXR libraries)
// through the persistent DXIL cache, whose key holds the exact arguments, so an unchanged shader is a cache read. The
// bootstrap is assembled afterwards in key order, as before, so its bytes do not depend on the thread count.
// ENGINE_COOK_THREADS (1-32) overrides the count: half the hardware threads, at most 16, at most one per 1 GiB of free
// physical memory.
std::vector<std::byte> ShaderLibrary::CookBootstrap(
    const std::filesystem::path& root,
    std::span<const PackagedShaderEntry> shaders,
    std::span<const PackagedLibraryEntry> libraries,
    ShaderBootstrapStatistics* statistics)
{
    const auto started = std::chrono::steady_clock::now();
    std::map<std::string,std::vector<char>> entries;
    ShaderBootstrapStatistics cooked;

    // NVIDIA SHARC compiles only with its headers deployed beside the shaders (ENGINE_SHARC_ROOT). Without them its
    // entries are left out (SHARC itself and ReSTIR PT's SHARC twins), and a packaged game reports SHARC unavailable
    // instead of failing to build.
    const bool sharcHeaders = std::filesystem::is_regular_file(root / "Sharc" / "SharcCommon.h");

    if(!sharcHeaders)
    {
        Log::Info("NVIDIA SHARC headers are not deployed; SHARC is left out of the packaged shader bootstrap.");
    }

    // One work item per entry the cook keeps: a shader (index into shaders) or a library (index into libraries).
    struct CookItem
    {
        bool library = false;
        std::size_t index = 0;
        std::string key;
        std::vector<char> bytecode;
        bool cacheHit = false;
    };

    std::vector<CookItem> items;

    for(std::size_t index = 0; index < shaders.size(); ++index)
    {
        const PackagedShaderEntry& shader = shaders[index];
        const bool sharcEntry = shader.feature == PackagedShaderFeature::Sharc ||
            shader.feature == PackagedShaderFeature::ReSTIRPTSharc;

        if(!sharcHeaders && sharcEntry)
        {
            continue;
        }

        items.push_back({false, index, std::string(shader.path) + ":" + shader.entryPoint + ":" +
            std::to_string(static_cast<int>(shader.stage)), {}, false});
    }

    for(std::size_t index = 0; index < libraries.size(); ++index)
    {
        const PackagedLibraryEntry& library = libraries[index];

        if(!sharcHeaders && ShaderCompiler::UsesNative16BitTypes(library.path))
        {
            continue;
        }

        // Packaged games use SM 6.6: DXR libraries, and (RayQuery 2M E3) the cs_6_6 direct-lighting compute shaders.
        if(ShaderCompiler::GetLibraryProfile(library.path) == ShaderLibraryProfile::Library69)
        {
            throw std::runtime_error("Packaged games use SM 6.6 DXR libraries only: " + library.path.generic_string());
        }

        items.push_back({true, index, library.path.generic_string() + ":library", {}, false});
    }

    // The libraries are the long compiles (tens of seconds each when cold); started first so the pool ends sooner.
    std::stable_partition(items.begin(), items.end(), [](const CookItem& item) { return item.library; });

    unsigned int threadCount = std::clamp((std::max)(std::thread::hardware_concurrency(), 1u) / 2u, 1u, 16u);
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);

    if(GlobalMemoryStatusEx(&memory))
    {
        threadCount = static_cast<unsigned int>(std::clamp<std::uint64_t>(memory.ullAvailPhys >> 30, 1u, threadCount));
    }

    if(const char* value = std::getenv("ENGINE_COOK_THREADS"))
    {
        const int requested = std::atoi(value);

        if(requested >= 1 && requested <= 32)
        {
            threadCount = static_cast<unsigned int>(requested);
        }
    }

    threadCount = (std::min)(threadCount, static_cast<unsigned int>((std::max)(items.size(), std::size_t{1})));
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex failureMutex;
    std::string failure;
    const std::wstring shaderRoot = root.wstring();

    const auto cook = [&]()
    {
        try
        {
            ShaderCompiler compiler;

            if(!compiler.Initialize())
            {
                throw std::runtime_error("Cannot initialize offline DXC.");
            }

            struct Cleanup { ShaderCompiler& c; ~Cleanup(){c.Shutdown();} } cleanup{compiler};
            // Cooked-game port: the production arguments, whatever development switches the cooking process was started
            // with (the ReSTIRPT/ tree's ENGINE_RESTIR_PT_* compile switches), so a game's DXIL is the same on every machine.
            const ShaderCompiler::ProductionArgumentsScope productionArguments;

            for(std::size_t slot = next++; slot < items.size() && !failed; slot = next++)
            {
                CookItem& item = items[slot];

                if(!item.library)
                {
                    // The arguments Load uses at runtime: the source under the shader root, its own directory as the include
                    // path; the shader root, so a ShaderVariantTable entry (no file; e.g. PsrInPlaceReplaceK2.hlsl) resolves
                    // to its generated text as at runtime.
                    const PackagedShaderEntry& shader = shaders[item.index];
                    const auto path = (root / shader.path).wstring();
                    const auto entryPoint = std::filesystem::path(shader.entryPoint).wstring();
                    ShaderCompileResult result;

                    if(!compiler.Compile({path.c_str(), entryPoint.c_str(), shader.stage, shaderRoot.c_str(), true}, result))
                    {
                        throw std::runtime_error(std::string("Offline shader compilation failed: ") + shader.path + ":" +
                            shader.entryPoint + " (" + PackagedShaderFeatureName(shader.feature) + ")");
                    }

                    item.bytecode = std::move(result.bytecode);
                    item.cacheHit = result.cacheHit;
                    continue;
                }

                // The arguments CompileLibrary uses at runtime: the shader root as the second include directory (variant
                // subdirectories resolve the RTXDI SDK's angled includes through it) and the persistent DXIL cache.
                const PackagedLibraryEntry& library = libraries[item.index];
                const auto path = (root / library.path).wstring();
                ShaderLibraryCompileDescription description{};
                description.path = path.c_str();
                description.profile = ShaderCompiler::GetLibraryProfile(library.path);
                description.includeDirectory = shaderRoot.c_str();
                description.usePersistentCache = true;
                ShaderLibraryCompileResult result;

                if(!compiler.CompileLibrary(description, result))
                {
                    throw std::runtime_error("Offline DXR compilation failed: " + library.path.generic_string() + " (" +
                        PackagedShaderFeatureName(library.feature) + ")");
                }

                item.bytecode = std::move(result.bytecode);
                item.cacheHit = result.cacheStatus == ShaderLibraryCompileResult::CacheStatus::Hit;
            }
        }
        catch(const std::exception& exception)
        {
            const std::lock_guard lock(failureMutex);

            if(failure.empty())
            {
                failure = exception.what();
            }

            failed = true;
        }
    };

    std::vector<std::thread> threads;

    for(unsigned int thread = 1; thread < threadCount; ++thread)
    {
        try
        {
            threads.emplace_back(cook);
        }
        catch(const std::system_error&)
        {
            break;
        }
    }

    cook();

    for(std::thread& thread : threads)
    {
        thread.join();
    }

    if(failed)
    {
        throw std::runtime_error(failure);
    }

    for(CookItem& item : items)
    {
        if(!entries.emplace(item.key, std::move(item.bytecode)).second)
        {
            throw std::runtime_error(std::string("Duplicate packaged ") + (item.library ? "library" : "shader") +
                " inventory entry: " + item.key);
        }

        if(item.library)
        {
            ++cooked.libraries;
            cooked.libraryCacheHits += item.cacheHit ? 1u : 0u;
            cooked.optionalLibraries += libraries[item.index].optional ? 1u : 0u;
        }
        else
        {
            ++cooked.shaders;
            cooked.shaderCacheHits += item.cacheHit ? 1u : 0u;
        }
    }

    cooked.threads = threadCount;

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
        if(payload.size()>MaximumBootstrapBytes) throw std::runtime_error("Shader bundle exceeds " + std::to_string(MaximumBootstrapBytes / (1024 * 1024)) + " MiB.");
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

    // A DXBC container (header, part offsets, parts) whose DXIL program part is a library (a compute shader for Compute66)
    // of at most the profile's shader model. Only structure is checked; the driver validates the bitcode itself at
    // pipeline creation.
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

            // DxilProgramHeader::ProgramVersion: kind in bits 16-31, major in 4-7, minor in 0-3. Kind 6 is a library,
            // kind 5 a compute shader (the Compute66 profile).
            const std::uint32_t version = ReadWord(bytes, offset + 8);
            const std::uint32_t kind = version >> 16;
            const std::uint32_t model = ((version >> 4) & 0xF) * 10 + (version & 0xF);
            const std::uint32_t supported = profile == ShaderLibraryProfile::Library69 ? 69u : 66u;
            const bool compute = profile == ShaderLibraryProfile::Compute66;
            const std::uint32_t expectedKind = compute ? 5u : 6u;

            if(kind != expectedKind || model > supported)
            {
                return "is incompatible (a shader of kind " + std::to_string(kind) + ", model " +
                    std::to_string(model / 10) + "." + std::to_string(model % 10) + "; this game needs " +
                    (compute ? "a compute shader" : "a library") + " of model " + std::to_string(supported / 10) + "." +
                    std::to_string(supported % 10) + ")";
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
