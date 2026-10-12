#pragma once

#include "Renderer/Vertex.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Non-owning view of a decompressed, linear R8_UNORM heightmap.
struct DisplacementHeightmap
{
    const std::uint8_t* pixels;
    std::size_t width;
    std::size_t height;
    std::size_t rowPitch;
};

struct DisplacementMeshSettings
{
    std::uint32_t patchesPerAxis = 8;
    std::uint32_t subdivisionsPerPatch = 32;
    std::uint32_t testPatchX = 3;
    std::uint32_t testPatchZ = 3;

    std::uint32_t testPatchLOD = 1;

    float displacementScale = 0.10f;
    float displacementMidpoint = 0.5f;
    // Optional CPU-only LOD map for validation, indexed by patchX * patchesPerAxis + patchZ.
    // An empty map retains the existing three-patch diagnostic selection.
    std::span<const std::uint32_t> patchLODs{};
};

struct DisplacementPatch
{
    std::uint32_t patchX;
    std::uint32_t patchZ;
    std::uint32_t lodLevel;
    std::uint32_t subdivisions;

    std::uint32_t firstIndex;
    std::uint32_t indexCount;
};

struct DisplacementGeometry
{
    std::vector<Vertex> vertices;

    std::vector<std::uint32_t> indices;

    std::vector<DisplacementPatch> patches;
};

class DisplacementMeshGenerator
{
    public:
    // Fixed X/Z bounds [-1,1]. Stitching supports one-LOD neighbors, including
    // adjacent stitched edges using a shared corner transition.
    // Requires valid R8 data and nonzero, compatible subdivision settings.
    static DisplacementGeometry Generate(
        const DisplacementHeightmap& heightmap,
        const DisplacementMeshSettings& settings);
};
