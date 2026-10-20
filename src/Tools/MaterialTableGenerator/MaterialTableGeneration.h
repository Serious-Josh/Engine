#pragma once

#include "Renderer/MaterialTables/MaterialTableFile.h"

#include <array>
#include <cstdint>

// Offline generation of the RTXPT.1A.1 complement tables (see MaterialTableFile.h for the format).
//
// The integrand is Engine's single-scatter GGX BRDF with F = 1: stable D, height-correlated Smith
// G2 = 1 / (1 + Lambda(l) + Lambda(v)) (algebraically GeometrySmithGGX), roughness clamped to
// MinimumRoughness, alpha = r^2. Directions l are generated on a deterministic midpoint lattice of
// GGX visible normals (Heitz 2018), whose weight G2 / G1(v) is bounded, so grazing views converge. The
// radius coordinate is warped towards the GGX tail (see IntegrateMissingAndBias).
// Each lattice point contributes nonnegative terms only, times the warp Jacobian w:
//   l below the horizon:  M += w
//   otherwise:            M += w Lambda(l) / (1 + Lambda(v) + Lambda(l)),   B += w Fc (1 + Lambda(v)) / (1 + Lambda(v) + Lambda(l))
// Everything is double precision with Kahan summation in a fixed order per texel, so results are
// independent of the thread count.
namespace MaterialTables
{
    // 1: uniform midpoint lattice (superseded during RTXPT.1A.1 validation); 2: tail-warped radius coordinate.
    inline constexpr std::uint32_t GeneratorVersion = 2;
    inline constexpr double MinimumRoughness = 0.045;

    struct GenerationSettings
    {
        std::uint32_t tableSize = 512;
        std::uint32_t latticeSize = 512;
        CosineMapping cosineMapping = CosineMapping::Squared;
        unsigned int threadCount = 0; // 0 = hardware concurrency.
    };

    // Smith GGX Lambda for the clamped alpha, cancellation-free: t / (2 (1 + sqrt(1 + t))), t = a^2 tan^2.
    double SmithLambda(double alphaSquared, double cosine);

    // (M, B) for one (cosine, roughness) with the given lattice size.
    std::array<double, 2> IntegrateMissingAndBias(double cosine, double roughness, std::uint32_t latticeSize);

    // Full generation: directional table, then averages integrated from its bilinear reads.
    MaterialTableSet GenerateMaterialTables(const GenerationSettings& settings);

    // Mavg/Bavg for one roughness: 2 * integral of the bilinear (M, B) * mu dmu, midpoint rule in the
    // table coordinate with 2^20 steps (double).
    std::array<double, 2> IntegrateAverage(const MaterialTableSet& tables, double roughness);
}
