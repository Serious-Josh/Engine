#pragma once

#include "Shader.h"
#include "ShaderCompiler.h"

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

class ShaderLibrary
{
    public:
    ShaderLibrary() = default;
    ShaderLibrary(const ShaderLibrary&) = delete;
    ShaderLibrary& operator=(const ShaderLibrary&) = delete;

    bool Initialize(const std::filesystem::path& shaderRoot);
    void Shutdown();
    bool InitializePackaged(std::span<const std::byte> payload);
    // Offline authoring only. Compiles the explicit engine shader manifest.
    static std::vector<std::byte> CookBootstrap(const std::filesystem::path& root);

    Shader* Load(const ShaderDescription& description);

    bool CompileLibrary(
        const std::filesystem::path& path,
        ShaderLibraryCompileResult& output);

    // True when compiling the library again would read identical inputs, so its bytecode can be reused.
    // Packaged bytecode is immutable for the session.
    bool IsLibraryCurrent(const ShaderLibraryCompileResult& library) const;

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
