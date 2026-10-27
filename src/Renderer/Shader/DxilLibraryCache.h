#pragma once

#include "ShaderCompiler.h"

#include <span>

// One synchronous cache operation, owned by the calling compiler thread. No background work or locks
// survive CompileLibrary; the existing compile worker's join also covers cache reads and writes.
class DxilLibraryCache
{
public:
    DxilLibraryCache(const ShaderLibraryCompileDescription& description,
        std::span<const wchar_t* const> arguments) noexcept;

    bool Load(IDxcUtils* utils, ShaderLibraryCompileResult& output) const noexcept;
    bool Store(const ShaderLibraryCompileResult& output) const noexcept;
    bool Enabled() const
    {
        return !m_file.empty();
    }

    // Called only by the render-thread consumers; compile workers return metrics without logging.
    static void Report(const std::filesystem::path& path, const ShaderLibraryCompileResult& result);

private:
    std::filesystem::path m_file;
    std::string m_identity;
};
