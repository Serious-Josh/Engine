#pragma once

#include "Shader.h"
#include "ShaderCompiler.h"
#include "ShaderLibraryCompileWorker.h"
#include "Renderer/PackagedShaderInventory.h"

#include <memory>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <span>
#include <map>

struct ShaderKey
{
    std::string path;
    std::string entryPoint;

    ShaderStage stage = ShaderStage::Vertex;

    bool operator==(const ShaderKey& other) const
    {
        return path == other.path && entryPoint == other.entryPoint && stage == other.stage;
    }
};

struct ShaderKeyHash
{
    size_t operator()(const ShaderKey& key) const
    {
        size_t pathHash = std::hash<std::string>()(key.path);
        size_t entryPointHash = std::hash<std::string>()(key.entryPoint);
        size_t stageHash = std::hash<int>()(static_cast<int>(key.stage));

        return pathHash ^ (entryPointHash << 1) ^ (stageHash << 2);
    }
};

// What one CookBootstrap produced.
struct ShaderBootstrapStatistics
{
    std::size_t shaders = 0;
    std::size_t libraries = 0;
    std::size_t optionalLibraries = 0;
    std::size_t libraryCacheHits = 0;
    std::uint64_t bytes = 0;
    std::uint64_t largestRecordBytes = 0;
    double seconds = 0.0;
};

class ShaderLibrary
{
    public:
    ShaderLibrary() = default;
    ShaderLibrary(const ShaderLibrary&) = delete;
    ShaderLibrary& operator=(const ShaderLibrary&) = delete;

    bool Initialize(const std::filesystem::path& shaderRoot);
    void Shutdown();
    bool InitializePackaged(std::span<const std::byte> payload);

    // The shader bootstrap limits InitializePackaged admits.
    static constexpr std::size_t MaximumBootstrapRecords = 256;
    static constexpr std::size_t MaximumBootstrapRecordBytes = 16 * 1024 * 1024;
    static constexpr std::size_t MaximumBootstrapBytes = 64 * 1024 * 1024;

    // Offline authoring only. Compiles the renderer's packaged inventory (PackagedShaderInventory) with the arguments
    // the runtime uses, libraries through the persistent DXIL cache. Throws when any entry fails or a limit is exceeded.
    static std::vector<std::byte> CookBootstrap(
        const std::filesystem::path& root,
        std::span<const PackagedShaderEntry> shaders,
        std::span<const PackagedLibraryEntry> libraries,
        ShaderBootstrapStatistics* statistics = nullptr);

    bool IsPackaged() const
    {
        return m_packaged;
    }

    // Whether the packaged bootstrap holds the library at path (its runtime key). Always false outside packaged games.
    bool HasPackagedLibrary(const std::filesystem::path& path) const
    {
        return m_packaged && m_packagedBytecode.contains(path.generic_string() + ":library");
    }

    // PS.5B.1: the cooked library at path, the packaged counterpart of compiling it. On failure problem says whether it
    // "is not in this game's shaders", "is corrupt (...)" or "is incompatible (...)". Never logs.
    bool AcquirePackagedLibrary(const std::filesystem::path& path, ShaderLibraryCompileResult& output,
        std::string& problem) const;

    // "path:entry:stage" of every graphics or compute shader loaded this session (inventory verification).
    std::vector<std::string> GetLoadedShaderKeys() const;

    Shader* Load(const ShaderDescription& description);

    bool CompileLibrary(
        const std::filesystem::path& path,
        ShaderLibraryCompileResult& output);

    // Resolves a library for the async compile worker and counts it as a DXC library compilation. False
    // for packaged bytecode, which never compiles.
    bool CreateLibraryCompileRequest(
        const std::filesystem::path& path,
        ShaderLibraryCompileRequest& request);

    // True when compiling the library at path again would read identical inputs with the same profile, so
    // its bytecode can be reused. Packaged bytecode is immutable for the session.
    bool IsLibraryCurrent(
        const std::filesystem::path& path,
        const ShaderLibraryCompileResult& library) const;

    // DXC library compilations this session (packaged lookups excluded).
    unsigned int GetLibraryCompileCount() const
    {
        return m_libraryCompileCount;
    }

    private:
    unsigned int m_libraryCompileCount = 0;
    bool m_packaged = false;
    std::map<std::string, std::vector<char>> m_packagedBytecode;
    ShaderCompiler m_compiler;

    std::filesystem::path m_shaderRoot;

    std::unordered_map<ShaderKey, std::unique_ptr<Shader>, ShaderKeyHash> m_shaders;
};
