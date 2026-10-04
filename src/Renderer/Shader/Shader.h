#pragma once

#include <cstddef>
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
        
        bool Initialize(const void* bytecode, size_t size);

        bool Load(const char* path);
        void Unload();

        const void* GetByteCode() const;
        size_t GetBytecodeSize() const;

    private:
        std::vector<char> m_bytecode;
};