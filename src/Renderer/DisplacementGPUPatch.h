#pragma once

#include <cstddef>
#include <cstdint>

struct DisplacementGPUPatch
{
    std::uint32_t patchX;
    std::uint32_t patchZ;

    std::uint32_t lodLevel;
    std::uint32_t firstIndex;

    std::uint32_t indexCount;
    //for if neighbors are courser
    std::uint32_t stitchFlags;

    std::uint32_t reserved0;
    std::uint32_t reserved1;
};

static_assert(sizeof(DisplacementGPUPatch) == 32);
static_assert(offsetof(DisplacementGPUPatch, firstIndex) == 12);
static_assert(offsetof(DisplacementGPUPatch, stitchFlags) == 20);
