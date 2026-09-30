#pragma once

#include "Shader.h"
#include "ShaderCompiler.h"

#include <memory>
#include <vector>
#include <filesystem>

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

        std::vector<std::unique_ptr<Shader>> m_shaders;
};