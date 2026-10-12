#include "DisplacementGPUIndexLayout.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace
{
    constexpr std::uint32_t StitchNegativeX = 1u << 0;
    constexpr std::uint32_t StitchPositiveX = 1u << 1;
    constexpr std::uint32_t StitchNegativeZ = 1u << 2;
    constexpr std::uint32_t StitchPositiveZ = 1u << 3;
}

bool DisplacementGPUIndexLayoutBuilder::Build(
    const DisplacementMeshSettings& settings,
    DisplacementGPUIndexLayout& output)
{
    output.patches.clear();
    output.totalIndexCount = 0;

    if (settings.patchesPerAxis == 0 || settings.subdivisionsPerPatch < 2 ||
        (settings.subdivisionsPerPatch & (settings.subdivisionsPerPatch - 1)) != 0 || settings.testPatchLOD > 1)
    {
        return false;
    }

    constexpr std::uint64_t maxUint32 = (std::numeric_limits<std::uint32_t>::max)();

    const std::uint64_t patchesPerAxis = settings.patchesPerAxis;
    const std::uint64_t subdivisionsPerPatch = settings.subdivisionsPerPatch;

    // Both operands are uint32_t, so these products fit in uint64_t.
    const std::uint64_t patchCount = patchesPerAxis * patchesPerAxis;
    const std::uint64_t gridResolution = patchesPerAxis * subdivisionsPerPatch;

    // Bound the raw patch buffer and grid before any further multiplication.

    if (patchCount > maxUint32 / sizeof(DisplacementGPUPatch) || gridResolution >= maxUint32)
    {
        return false;
    }

    const std::uint64_t rowStride = gridResolution + 1;

    if (rowStride > maxUint32 / rowStride)
    {
        return false;
    }

    const std::uint64_t vertexCount = rowStride * rowStride;

    if (vertexCount > maxUint32 / sizeof(Vertex))
    {
        return false;
    }

    if (!settings.patchLODs.empty() &&

        (settings.patchLODs.size() != patchCount ||

         std::any_of(settings.patchLODs.begin(), settings.patchLODs.end(), [](std::uint32_t lod) { return lod > 1; })))
    {
        return false;
    }

    const auto getPatchLOD = [&settings, patchesPerAxis](std::uint32_t x, std::uint32_t z) -> std::uint32_t
    {
        if (!settings.patchLODs.empty())
        {
            const std::uint64_t patchIndex = std::uint64_t{x} * patchesPerAxis + z;
            return settings.patchLODs[static_cast<std::size_t>(patchIndex)];
        }

        // Keep the CPU diagnostic selection, including its uint32_t + 1 behavior.

        const bool primaryPatch = x == settings.testPatchX && z == settings.testPatchZ;
        const bool secondPatch = x == settings.testPatchX + 1u && z == settings.testPatchZ;
        const bool thirdPatch = x == settings.testPatchX && z == settings.testPatchZ + 1u;
        return (primaryPatch || secondPatch || thirdPatch) ? settings.testPatchLOD : 0u;
    };

    // Publish only a complete layout, leaving output empty on validation failure.
    DisplacementGPUIndexLayout layout;
    layout.patches.reserve(static_cast<std::size_t>(patchCount));
    std::uint64_t totalIndexCount = 0;
    constexpr std::uint64_t maxIndexCount = maxUint32 / sizeof(std::uint32_t);

    for (std::uint32_t patchX = 0; patchX < settings.patchesPerAxis; ++patchX)
    {
        for (std::uint32_t patchZ = 0; patchZ < settings.patchesPerAxis; ++patchZ)
        {
            DisplacementGPUPatch patch{};
            patch.patchX = patchX;
            patch.patchZ = patchZ;
            patch.lodLevel = getPatchLOD(patchX, patchZ);

            const bool stitchNegativeX = patchX > 0 && getPatchLOD(patchX - 1, patchZ) > patch.lodLevel;
            const bool stitchPositiveX =
                patchX + 1 < settings.patchesPerAxis && getPatchLOD(patchX + 1, patchZ) > patch.lodLevel;
            const bool stitchNegativeZ = patchZ > 0 && getPatchLOD(patchX, patchZ - 1) > patch.lodLevel;
            const bool stitchPositiveZ =
                patchZ + 1 < settings.patchesPerAxis && getPatchLOD(patchX, patchZ + 1) > patch.lodLevel;

            patch.stitchFlags = (stitchNegativeX ? StitchNegativeX : 0u) | (stitchPositiveX ? StitchPositiveX : 0u) |
                                (stitchNegativeZ ? StitchNegativeZ : 0u) | (stitchPositiveZ ? StitchPositiveZ : 0u);
            patch.reserved0 = 0;
            patch.reserved1 = 0;

            const std::uint32_t subdivisions = settings.subdivisionsPerPatch >> patch.lodLevel;
            std::uint64_t indexCount = 0;

            for (std::uint32_t localX = 0; localX < subdivisions; ++localX)
            {
                for (std::uint32_t localZ = 0; localZ < subdivisions; ++localZ)
                {
                    // Match the CPU branch priority, including overlapping corner blocks
                    // at the smallest subdivision count.
                    const bool cornerNegativeX = stitchNegativeX && localX < 2u;
                    const bool cornerPositiveX = stitchPositiveX && localX >= subdivisions - 2u;
                    const bool cornerNegativeZ = stitchNegativeZ && localZ < 2u;
                    const bool cornerPositiveZ = stitchPositiveZ && localZ >= subdivisions - 2u;

                    if ((cornerNegativeX || cornerPositiveX) && (cornerNegativeZ || cornerPositiveZ))
                    {
                        const std::uint32_t blockX = cornerNegativeX ? 0u : subdivisions - 2u;
                        const std::uint32_t blockZ = cornerNegativeZ ? 0u : subdivisions - 2u;

                        if (localX == blockX && localZ == blockZ)
                        {
                            indexCount += 18;
                        }

                        // Every cell in the reserved block is skipped.
                        continue;
                    }

                    if (stitchPositiveX && localX == subdivisions - 1 && (localZ % 2u) == 0u)
                    {
                        indexCount += 9;

                        continue;
                    }

                    if (stitchPositiveX && localX == subdivisions - 1 && (localZ % 2u) == 1u)
                    {
                        continue;
                    }

                    if (stitchNegativeX && localX == 0u && (localZ % 2u) == 0u)
                    {
                        indexCount += 9;

                        continue;
                    }

                    if (stitchNegativeX && localX == 0u && (localZ % 2u) == 1u)
                    {
                        continue;
                    }

                    if (stitchNegativeZ && localZ == 0u && (localX % 2u) == 0u)
                    {
                        indexCount += 9;

                        continue;
                    }

                    if (stitchNegativeZ && localZ == 0u && (localX % 2u) == 1u)
                    {
                        continue;
                    }

                    if (stitchPositiveZ && localZ == subdivisions - 1 && (localX % 2u) == 0u)
                    {
                        indexCount += 9;

                        continue;
                    }

                    if (stitchPositiveZ && localZ == subdivisions - 1 && (localX % 2u) == 1u)
                    {
                        continue;
                    }

                    indexCount += 6;
                }
            }

            // The vertex-buffer bound above limits the total number of visited
            // cells, so the uint64_t per-patch count cannot overflow.
            // Check addition against the raw uint32 index-buffer byte limit.

            if (indexCount > maxIndexCount - totalIndexCount)
            {
                return false;
            }

            patch.firstIndex = static_cast<std::uint32_t>(totalIndexCount);
            patch.indexCount = static_cast<std::uint32_t>(indexCount);
            totalIndexCount += indexCount;

            layout.patches.push_back(patch);
        }
    }

    layout.totalIndexCount = static_cast<std::uint32_t>(totalIndexCount);
    output = std::move(layout);
    return true;
}
