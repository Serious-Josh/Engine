#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Engine's shader variants (ShaderVariantTable.inl): libraries and shaders that differ from a source file only in
// preprocessor definitions, named by the shader-root-relative path the renderer requests (for example
// "GI6/PathTraceReferenceSERSM69.hlsl", the RayQuery twin of the SER reference library). They have no file. The compiler
// asks ShaderVariants::Source for the text a variant compiles as, which is exactly what its former wrapper file
// preprocessed to: its definitions, in order, then #include of its base and of the files that follow it.
//
// Identity: a variant's name is stable (the cooked libraries, the DXIL cache and every pipeline description use it), and
// its text is a pure function of the table, so it compiles to the same DXIL as the wrapper chain it replaces.
struct ShaderVariant
{
    // Shader-root-relative path, '/' separated.
    std::string_view path;
    // The source file or variant this one includes after its definitions (shader-root-relative).
    std::string_view base;
    // "NAME=VALUE" (or "NAME" for an empty value), space separated, in order.
    std::string_view defines;
    // Shader-root-relative files included after the base, space separated, in order.
    std::string_view after;
};

namespace ShaderVariants
{
    // Every variant, in table order.
    std::span<const ShaderVariant> All();

    // The variant named by a shader-root-relative path, or null.
    const ShaderVariant* Find(const std::filesystem::path& relativePath);

    // The variant at sourcePath (absolute, or relative to the working directory) under shaderRoot, or null when
    // sourcePath is not under shaderRoot or names no variant.
    const ShaderVariant* FindUnder(const std::filesystem::path& shaderRoot, const std::filesystem::path& sourcePath);

    // The text a variant compiles as: the definitions of the variant and of each variant its base chain passes through,
    // outermost first, then the #include of the chain's source file and the `after` files, innermost first. Includes are
    // relative to the variant's own directory, as its wrapper's were. Empty when the chain is invalid.
    std::optional<std::string> Source(const ShaderVariant& variant);

    // Table consistency against a shader root: every base and `after` file resolves (to a variant or a file), no chain
    // repeats a variant, names are unique, and no file shadows a variant. One message per problem; empty when valid.
    std::vector<std::string> Validate(const std::filesystem::path& shaderRoot);

    // Testing only: compile variant paths from files instead (to compare the table with the wrapper files it replaced).
    void SetEnabledForTesting(bool enabled);
    bool Enabled();
}
