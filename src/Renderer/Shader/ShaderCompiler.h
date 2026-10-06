#pragma once

#include "Shader.h"

#include <Windows.h>
#include <dxcapi.h>
#include <vector>

struct ShaderCompileDescription
{
    const wchar_t* path = nullptr;
    const wchar_t* entryPoint = nullptr;
    ShaderStage stage = ShaderStage::Vertex;
};

struct ShaderCompileResult
{
    std::vector<char> bytecode;
};

struct ShaderLibraryCompileDescription
{
    const wchar_t* path = nullptr;
};

struct ShaderLibraryCompileResult
{
    std::vector<char> bytecode;
};

class ShaderCompiler
{
    public:
        ShaderCompiler() = default;
        ShaderCompiler(const ShaderCompiler&) = delete;
        ShaderCompiler& operator=(const ShaderCompiler&) = delete;

        bool Initialize();
        void Shutdown();

        bool Compile(const ShaderCompileDescription& description, ShaderCompileResult& output);
        bool CompileLibrary(const ShaderLibraryCompileDescription& description, ShaderLibraryCompileResult& output);

    private:
        const wchar_t* GetShaderProfile(ShaderStage stage) const;

        IDxcUtils* m_utils = nullptr;
        IDxcCompiler3* m_compiler = nullptr;
};