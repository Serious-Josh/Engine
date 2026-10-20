#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

// RTXPT.1A.1 GGX single-scatter complement tables (".emt", Engine Material Tables).
//
// Two RG32F tables of Engine's exact single-scatter BRDF with F = 1 (stable GGX D, height-correlated Smith G,
// roughness clamped to MinimumRoughness), generated offline by tools/MaterialTableGenerator:
//
//   directional  width = cosine axis, height = roughness axis, texel (M, B):
//                M(mu, r) = 1 - E(mu, r)   missing single-scatter energy, stored directly (never 1 - E)
//                B(mu, r) = Schlick bias    integral of (1 - v.h)^5 f_ss cos
//                The Schlick scale is S = 1 - M - B, so E_ss(F0) = F0 S + B.
//   average      width = roughness axis, height = 1, texel (Mavg, Bavg) = 2 * integral of (M, B) mu dmu over
//                the bilinear, texel-centre-clamped reads of the directional table.
//
// Axes use texel centres: roughness r = (j + 0.5) / height; cosine coordinate u = (i + 0.5) / width with
// mu = u (Linear) or mu = u * u (Squared, denser near grazing). Reads are bilinear, clamped to texel centres;
// a wrapping sampler must never be used.
//
// File layout (little-endian): MaterialTableFileHeader, then the directional texels row by row (roughness
// rows, cosine columns, RG interleaved), then the average texels. The renderer loads the bundled file at
// initialization (MaterialTableResources); packaged games carry it as a bootstrap archive entry.
namespace MaterialTables
{
    enum class CosineMapping : std::uint32_t
    {
        Linear = 0,
        Squared = 1
    };

    struct MaterialTableFileHeader
    {
        std::array<char, 4> magic{'E', 'M', 'T', 'L'};
        std::uint32_t version = 1;
        std::uint32_t headerBytes = sizeof(MaterialTableFileHeader);
        std::uint32_t channels = 2;

        std::uint32_t cosineMapping = static_cast<std::uint32_t>(CosineMapping::Linear);
        std::uint32_t directionalWidth = 0;
        std::uint32_t directionalHeight = 0;
        std::uint32_t averageWidth = 0;

        float minimumRoughness = 0.045f;
        std::uint32_t latticeSize = 0;
        std::uint32_t generatorVersion = 0;
        std::uint32_t reserved = 0;
    };

    static_assert(sizeof(MaterialTableFileHeader) == 48);

    inline constexpr std::uint32_t FileVersion = 1;
    inline constexpr std::uint32_t MaximumTableSize = 4096;
    inline constexpr std::size_t MaximumFileBytes = 8u * 1024u * 1024u;

    // The one table set the path tracer accepts. Integrity is checked by SHA-256 before parsing; any other
    // content, even a valid regeneration with different settings, is rejected until this pin is updated.
    inline constexpr char BundledTableRelativePath[] = "MaterialTables/GGXSingleScatterComplement.emt";
    inline constexpr char BundledTableSha256[] = "a974ed16f30a25fca93cbf42c02154cab69effd9429dc1451caee4263b2df90e";
    inline constexpr std::uint32_t BundledGeneratorVersion = 2;

    struct MaterialTableSet
    {
        MaterialTableFileHeader header;

        // (M, B) pairs, directionalWidth * directionalHeight * 2 floats.
        std::vector<float> directional;

        // (Mavg, Bavg) pairs, averageWidth * 2 floats.
        std::vector<float> average;
    };

    // Structural and numerical checks shared by writer, reader and tests: dimensions, finite values,
    // M >= 0, B >= 0 and M + B <= 1 + tolerance (S >= 0).
    bool ValidateMaterialTableSet(const MaterialTableSet& tables, std::string* error = nullptr);

    bool WriteMaterialTableFile(const std::filesystem::path& path, const MaterialTableSet& tables,
        std::string* error = nullptr);

    // Parses and validates a complete file image (exact size, no trailing bytes).
    bool ParseMaterialTableBytes(std::span<const std::byte> bytes, MaterialTableSet& tables,
        std::string* error = nullptr);
    bool ReadMaterialTableBytes(const std::filesystem::path& path, std::vector<std::byte>& bytes,
        std::string* error = nullptr);

    // Lower-case hexadecimal SHA-256 (Windows CNG).
    bool ComputeSha256(std::span<const std::byte> bytes, std::string& hexadecimal);

    // Integrity and compatibility gate for the bundled tables: SHA-256 equals BundledTableSha256, the file
    // parses, and its layout is the one the path-tracing shaders read (squared cosine axis, clamp 0.045,
    // generator version 2, square table).
    bool ValidateBundledMaterialTables(std::span<const std::byte> bytes, MaterialTableSet& tables,
        std::string* error = nullptr);
    bool ReadMaterialTableFile(const std::filesystem::path& path, MaterialTableSet& tables,
        std::string* error = nullptr);

    // Cosine at a directional-table column centre, and its inverse texture coordinate.
    double CosineAtColumn(const MaterialTableFileHeader& header, std::uint32_t column);
    double CosineCoordinate(const MaterialTableFileHeader& header, double cosine);

    // CPU reference of the runtime read: bilinear, clamped to texel centres. Returns (M, B) or (Mavg, Bavg).
    std::array<double, 2> SampleDirectional(const MaterialTableSet& tables, double cosine, double roughness);
    std::array<double, 2> SampleAverage(const MaterialTableSet& tables, double roughness);
}
