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

    // Creates the compiler and requires the loaded dxcompiler.dll to be the pinned DXC release. A failure is
    // logged; the second form returns it in diagnostics instead, for a worker thread.
    bool Initialize();
    bool Initialize(std::string& diagnostics);
    void Shutdown();

    bool Compile(
        const ShaderCompileDescription& description,
        ShaderCompileResult& output);
    bool CompileLibrary(
        const ShaderLibraryCompileDescription& description,
        ShaderLibraryCompileResult& output);
    // Identical compilation and bytecode, but every error message is returned in diagnostics instead of
    // logged, so a worker thread can compile without touching Engine logging.
    bool CompileLibrary(
        const ShaderLibraryCompileDescription& description,
        ShaderLibraryCompileResult& output,
        std::string& diagnostics);

    // True when every recorded dependency still has the recorded contents (or is still absent).
    static bool DependenciesUnchanged(const std::vector<ShaderSourceDependency>& dependencies);

    private:
    // Logs through Engine when diagnostics is null; otherwise appends to it.
    bool Initialize(std::string* diagnostics);

    // Logs through Engine when diagnostics is null; otherwise appends to it.
    bool CompileLibrary(
        const ShaderLibraryCompileDescription& description,
        ShaderLibraryCompileResult& output,
        std::string* diagnostics);

    const wchar_t* GetShaderProfile(ShaderStage stage) const;

    IDxcUtils* m_utils = nullptr;

    IDxcCompiler3* m_compiler = nullptr;
};
