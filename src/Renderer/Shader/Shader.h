#pragma once

#include <cstddef>
#include <string>
#include <vector>

enum class ShaderStage
{
    Vertex,
    Pixel,
    Compute
};

struct ShaderDescription
{
    const char* path = nullptr;
    const char* entryPoint = nullptr;
    ShaderStage stage = ShaderStage::Vertex;
};

class Shader
{
    public:
    Shader() = default;

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    bool Initialize(
        const void* bytecode,
        size_t size);

    void Unload();

    const void* GetByteCode() const;
    size_t GetBytecodeSize() const;

    // "path:entryPoint", set by the shader library; names the pipeline states built from it (D3D12 debug names, so
    // GPU-based validation messages identify the draw or dispatch). Empty for shaders created elsewhere.
    void SetName(std::string name)
    {
        m_name = std::move(name);
    }

    const std::string& GetName() const noexcept
    {
        return m_name;
    }

    private:
    std::vector<char> m_bytecode;
    std::string m_name;
};
