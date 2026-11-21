#pragma once

#include "ShaderCompiler.h"

#include <span>
#include <string_view>

// The persistent DXIL cache of runtime compilations: DXR libraries and (M0c.5 Stage 5.4) vertex, pixel and compute shaders.
// One synchronous cache operation, owned by the calling compiler thread. No background work or locks survive the compile
// call; the compile workers' joins also cover cache reads and writes. Concurrent writers of one entry each publish a complete
// file atomically (AtomicFileWrite), and every reader verifies an entry completely before using it.
//
// Identity (format 2): the compiler's identity, the target ("lib_6_6", or "cs_6_6:CSMain" for a stage and entry point), the
// ANSI code page, and the source and every argument with paths inside the shader root written relative to it. Neither the
// process's working directory nor the root's absolute location takes part, so any launch directory, and any install with the
// same shaders, reuses entries. Every source file the compilation read, and every include lookup that found nothing, is
// recorded (root-relative inside the root) with its contents' hash, and checked again on every hit: a changed source or
// include, or a file appearing earlier in the include search, is a miss. A hit's bytecode is also checked by the deployed DXIL
// validator. An unreadable, truncated or corrupt entry is a miss, never an error; compilation then reports its own errors.
class DxilLibraryCache
{
public:
    // DXR libraries (ShaderCompiler::CompileLibrary): the shader root is the description's include directory.
    // variantSource: for a shader variant (ShaderVariants), the text it compiles as. A variant has no file to record as a
    // dependency, so its text joins the identity instead; empty for a file, whose identity is unchanged.
    DxilLibraryCache(const ShaderLibraryCompileDescription& description,
        std::span<const wchar_t* const> arguments, std::string_view variantSource = {}) noexcept;

    // Any compilation: enabled only when requested and a shader root is known.
    DxilLibraryCache(bool enabled, const wchar_t* source, const wchar_t* shaderRoot, std::string_view target,
        std::span<const wchar_t* const> arguments, std::string_view variantSource = {}) noexcept;

    bool Load(IDxcUtils* utils, std::vector<char>& bytecode, std::vector<ShaderSourceDependency>& dependencies) const noexcept;
    bool Store(const std::vector<char>& bytecode, const std::vector<ShaderSourceDependency>& dependencies) const noexcept;

    bool Load(IDxcUtils* utils, ShaderLibraryCompileResult& output) const noexcept
    {
        return Load(utils, output.bytecode, output.dependencies);
    }

    bool Store(const ShaderLibraryCompileResult& output) const noexcept
    {
        return Store(output.bytecode, output.dependencies);
    }

    bool Enabled() const
    {
        return !m_file.empty();
    }

    // Called only by the render-thread consumers; compile workers return metrics without logging.
    static void Report(const std::filesystem::path& path, const ShaderLibraryCompileResult& result);

private:
    std::filesystem::path m_file;
    std::filesystem::path m_root;
    std::string m_identity;
};
