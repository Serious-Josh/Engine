#pragma once

#include "Shader.h"
#include "ShaderCompiler.h"


#include <memory>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <dxcapi.h>

struct IDxCUtils;
struct IDxcCompiler3;

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

        Shader* Load(const ShaderDescription& description);

    private:
        ShaderCompiler m_compiler;
        std::filesystem::path m_shaderRoot;

        std::unordered_map<ShaderKey, std::unique_ptr<Shader>, ShaderKeyHash> m_shaders;
};