#pragma once

#include "Shader.h"

#include <Windows.h>
#include <dxcapi.h>
#include <filesystem>
#include <string>
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

// One source file a compilation read, or an include lookup that found no file. Together with the fixed
// compiler arguments, the dependencies of a compilation are every input that decides its bytecode.
struct ShaderSourceDependency
{
    std::filesystem::path path;
    bool exists = false;
    std::string contents;
};

struct ShaderLibraryCompileResult
{
    std::vector<char> bytecode;
    // Empty for packaged bytecode, which cannot change during a session.
    std::vector<ShaderSourceDependency> dependencies;
};

class ShaderCompiler
{
    public:
    ShaderCompiler() = default;
    ShaderCompiler(const ShaderCompiler&) = delete;
    ShaderCompiler& operator=(const ShaderCompiler&) = delete;

    bool Initialize();
    void Shutdown();

    bool Compile(
        const ShaderCompileDescription& description,
        ShaderCompileResult& output);
    bool CompileLibrary(
        const ShaderLibraryCompileDescription& description,
        ShaderLibraryCompileResult& output);

    // True when every recorded dependency still has the recorded contents (or is still absent).
    static bool DependenciesUnchanged(const std::vector<ShaderSourceDependency>& dependencies);

    private:
    const wchar_t* GetShaderProfile(ShaderStage stage) const;

    IDxcUtils* m_utils = nullptr;

    IDxcCompiler3* m_compiler = nullptr;
};
