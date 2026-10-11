#include "DisplacementMeshGenerator.h"

#include "Math/Vector3.h"

#include <algorithm>
#include <cmath>

DisplacementGeometry DisplacementMeshGenerator::Generate(
    const DisplacementHeightmap& heightmap,
    const DisplacementMeshSettings& settings)
{

    if(!heightmap.pixels ||
        heightmap.width == 0 ||
        heightmap.height == 0 ||
        heightmap.rowPitch < heightmap.width ||
        settings.patchesPerAxis == 0 ||
        settings.subdivisionsPerPatch < 2 ||
        (settings.subdivisionsPerPatch & (settings.subdivisionsPerPatch - 1)) != 0 ||
        settings.testPatchLOD > 1)
    {
        return {};
    }

    if(!settings.patchLODs.empty() &&
       (settings.patchLODs.size() != static_cast<std::size_t>(settings.patchesPerAxis) * settings.patchesPerAxis ||
        std::any_of(settings.patchLODs.begin(), settings.patchLODs.end(),
            [](std::uint32_t lod) { return lod > 1; })))
    {
        return {};
    }

    auto sampleHeight = [&](float u, float v) -> float
    {
        const float fx =
            std::clamp(u, 0.0f, 1.0f) *
            static_cast<float>(heightmap.width - 1);

        const float fy =
            std::clamp(v, 0.0f, 1.0f) *
            static_cast<float>(heightmap.height - 1);

        const std::size_t x0 = static_cast<std::size_t>(fx);
        const std::size_t y0 = static_cast<std::size_t>(fy);

        const std::size_t x1 =
            (std::min)(x0 + 1, heightmap.width - 1);

        const std::size_t y1 =
            (std::min)(y0 + 1, heightmap.height - 1);

        const float tx = fx - static_cast<float>(x0);
        const float ty = fy - static_cast<float>(y0);

        auto texel = [&](std::size_t x, std::size_t y) -> float
        {
            return static_cast<float>(
                heightmap.pixels[
                    y * heightmap.rowPitch + x
                ]
            ) / 255.0f;
        };

        const float h00 = texel(x0, y0);
        const float h10 = texel(x1, y0);
        const float h01 = texel(x0, y1);
        const float h11 = texel(x1, y1);

        const float top = std::lerp(h00, h10, tx);
        const float bottom = std::lerp(h01, h11, tx);

        return std::lerp(top, bottom, ty);
    };


    const std::uint32_t patchesPerAxis = settings.patchesPerAxis;
    const std::uint32_t subdivisionsPerPatch = settings.subdivisionsPerPatch;
    const std::uint32_t gridResolution = patchesPerAxis * subdivisionsPerPatch;

    DisplacementGeometry geometry;
    auto& patches = geometry.patches;
    patches.reserve(patchesPerAxis * patchesPerAxis);

    auto& gridVertices = geometry.vertices;
    auto& gridIndices = geometry.indices;

    gridVertices.reserve((gridResolution + 1) * (gridResolution + 1));

    gridIndices.reserve(gridResolution * gridResolution * 6);

    for(std::uint32_t x = 0; x <= gridResolution; ++x)
    {
        for(std::uint32_t z = 0; z <= gridResolution; ++z)
        {
            const float u = static_cast<float>(x) / gridResolution;

            const float v = static_cast<float>(z) / gridResolution;

            Vertex vertex{};

            const float displacementScale = settings.displacementScale;
            const float displacementMidpoint = settings.displacementMidpoint;

            const float height = sampleHeight(u, v);

            vertex.position[0] = u * 2.0f - 1.0f;

            vertex.position[1] = displacementScale * (height - displacementMidpoint);

            vertex.position[2] = v * 2.0f - 1.0f;

            vertex.normal[0] = 0.0f;
            vertex.normal[1] = 1.0f;
            vertex.normal[2] = 0.0f;

            vertex.uv[0] = u;
            vertex.uv[1] = v;

            gridVertices.push_back(vertex);
        }
    }

    const auto getPatchLOD =
        [&settings](std::uint32_t x, std::uint32_t z) -> std::uint32_t
    {
        if(!settings.patchLODs.empty())
        {
            return settings.patchLODs[x * settings.patchesPerAxis + z];
        }
        const bool primaryPatch =
            x == settings.testPatchX &&
            z == settings.testPatchZ;

        const bool secondPatch =
            x == settings.testPatchX + 1 &&
            z == settings.testPatchZ;

        const bool thirdPatch =
            x == settings.testPatchX &&
            z == settings.testPatchZ + 1;

        return (primaryPatch || secondPatch || thirdPatch)
            ? settings.testPatchLOD
            : 0u;
    };

    const auto gridIndex =
        [gridResolution](std::uint32_t x, std::uint32_t z) -> std::uint32_t
    {
        return x * (gridResolution + 1) + z;
    };


    for(std::uint32_t patchX = 0; patchX < patchesPerAxis; ++patchX)
    {
        for(std::uint32_t patchZ = 0; patchZ < patchesPerAxis; ++patchZ)
        {
            DisplacementPatch patch{};
            patch.patchX = patchX;
            patch.patchZ = patchZ;

            patch.lodLevel = getPatchLOD(patchX, patchZ);

            const bool stitchNegativeX =
                patchX > 0 &&
                getPatchLOD(patchX - 1, patchZ) > patch.lodLevel;

            const bool stitchPositiveX =
                patchX + 1 < patchesPerAxis &&
                getPatchLOD(patchX + 1, patchZ) > patch.lodLevel;

            const bool stitchNegativeZ =
                patchZ > 0 &&
                getPatchLOD(patchX, patchZ - 1) > patch.lodLevel;

            const bool stitchPositiveZ =
                patchZ + 1 < patchesPerAxis &&
                getPatchLOD(patchX, patchZ + 1) > patch.lodLevel;

            patch.subdivisions = subdivisionsPerPatch >> patch.lodLevel;

            const std::uint32_t vertexStep = subdivisionsPerPatch / patch.subdivisions;

            patch.firstIndex = static_cast<std::uint32_t>(gridIndices.size());

            for(std::uint32_t localX = 0; localX < patch.subdivisions; ++localX)
            {
                const std::uint32_t x =
                    patchX * subdivisionsPerPatch +
                    localX * vertexStep;

                for(std::uint32_t localZ = 0; localZ < patch.subdivisions; ++localZ)
                {
                    const std::uint32_t z =
                        patchZ * subdivisionsPerPatch +
                        localZ * vertexStep;

                    // Reserve a 2x2-cell block where two transition strips meet.
                    // Both coarse-facing edge midpoints are omitted; the other
                    // boundary vertices remain shared with the ordinary cells.
                    const bool cornerNegativeX = stitchNegativeX && localX < 2u;
                    const bool cornerPositiveX = stitchPositiveX && localX >= patch.subdivisions - 2u;
                    const bool cornerNegativeZ = stitchNegativeZ && localZ < 2u;
                    const bool cornerPositiveZ = stitchPositiveZ && localZ >= patch.subdivisions - 2u;
                    if((cornerNegativeX || cornerPositiveX) &&
                       (cornerNegativeZ || cornerPositiveZ))
                    {
                        const std::uint32_t blockX = cornerNegativeX ? 0u : patch.subdivisions - 2u;
                        const std::uint32_t blockZ = cornerNegativeZ ? 0u : patch.subdivisions - 2u;
                        if(localX == blockX && localZ == blockZ)
                        {
                            const std::uint32_t originX = patchX * subdivisionsPerPatch +
                                (cornerNegativeX ? 0u : patch.subdivisions) * vertexStep;
                            const std::uint32_t originZ = patchZ * subdivisionsPerPatch +
                                (cornerNegativeZ ? 0u : patch.subdivisions) * vertexStep;
                            const auto cornerIndex = [&](std::uint32_t dx, std::uint32_t dz)
                            {
                                return gridIndex(
                                    cornerNegativeX ? originX + dx * vertexStep : originX - dx * vertexStep,
                                    cornerNegativeZ ? originZ + dz * vertexStep : originZ - dz * vertexStep);
                            };
                            const std::uint32_t center = cornerIndex(1u, 1u);
                            const std::uint32_t boundary[] = {
                                cornerIndex(0u, 0u), cornerIndex(0u, 2u),
                                cornerIndex(1u, 2u), cornerIndex(2u, 2u),
                                cornerIndex(2u, 1u), cornerIndex(2u, 0u)};
                            for(std::uint32_t edge = 0; edge < 6u; ++edge)
                            {
                                const std::uint32_t next = (edge + 1u) % 6u;
                                gridIndices.push_back(center);
                                gridIndices.push_back(boundary[cornerNegativeX == cornerNegativeZ ? edge : next]);
                                gridIndices.push_back(boundary[cornerNegativeX == cornerNegativeZ ? next : edge]);
                            }
                        }
                        continue;
                    }

                    // Stitch the positive-X edge against a patch one LOD coarser.
                    if(stitchPositiveX &&
                    localX == patch.subdivisions - 1 &&
                    (localZ % 2u) == 0u)
                    {
                        const std::uint32_t innerX = x;
                        const std::uint32_t outerX = x + vertexStep;

                        const std::uint32_t z0 = z;
                        const std::uint32_t z1 = z + vertexStep;
                        const std::uint32_t z2 = z + 2u * vertexStep;

                        const std::uint32_t i0 = gridIndex(innerX, z0);
                        const std::uint32_t i1 = gridIndex(innerX, z1);
                        const std::uint32_t i2 = gridIndex(innerX, z2);

                        const std::uint32_t o0 = gridIndex(outerX, z0);
                        const std::uint32_t o2 = gridIndex(outerX, z2);

                        // Lower transition triangle.
                        gridIndices.push_back(i0);
                        gridIndices.push_back(i1);
                        gridIndices.push_back(o0);

                        // Central transition triangle.
                        gridIndices.push_back(i1);
                        gridIndices.push_back(o2);
                        gridIndices.push_back(o0);

                        // Upper transition triangle.
                        gridIndices.push_back(i1);
                        gridIndices.push_back(i2);
                        gridIndices.push_back(o2);

                        continue;
                    }

                    if(stitchPositiveX &&
                    localX == patch.subdivisions - 1 &&
                    (localZ % 2u) == 1u)
                    {
                        // The preceding iteration already emitted this pair of cells.
                        continue;
                    }

                    // Stitch the negative-X edge against a patch one LOD coarser.
                    if(stitchNegativeX &&
                    localX == 0u &&
                    (localZ % 2u) == 0u)
                    {
                        const std::uint32_t outerX = x;
                        const std::uint32_t innerX = x + vertexStep;

                        const std::uint32_t z0 = z;
                        const std::uint32_t z1 = z + vertexStep;
                        const std::uint32_t z2 = z + 2u * vertexStep;

                        const std::uint32_t o0 = gridIndex(outerX, z0);
                        const std::uint32_t o2 = gridIndex(outerX, z2);

                        const std::uint32_t i0 = gridIndex(innerX, z0);
                        const std::uint32_t i1 = gridIndex(innerX, z1);
                        const std::uint32_t i2 = gridIndex(innerX, z2);

                        gridIndices.push_back(o0);
                        gridIndices.push_back(o2);
                        gridIndices.push_back(i1);

                        gridIndices.push_back(o0);
                        gridIndices.push_back(i1);
                        gridIndices.push_back(i0);

                        gridIndices.push_back(o2);
                        gridIndices.push_back(i2);
                        gridIndices.push_back(i1);

                        continue;
                    }

                    if(stitchNegativeX &&
                    localX == 0u &&
                    (localZ % 2u) == 1u)
                    {
                        // The preceding iteration already emitted this pair of cells.
                        continue;
                    }

                    // Stitch the negative-Z edge against a patch one LOD coarser.
                    if(stitchNegativeZ &&
                    localZ == 0u &&
                    (localX % 2u) == 0u)
                    {
                        const std::uint32_t zOuter = z;
                        const std::uint32_t zInner = z + vertexStep;

                        const std::uint32_t x0 = x;
                        const std::uint32_t x1 = x + vertexStep;
                        const std::uint32_t x2 = x + 2u * vertexStep;

                        const std::uint32_t o0 = gridIndex(x0, zOuter);
                        const std::uint32_t o2 = gridIndex(x2, zOuter);

                        const std::uint32_t i0 = gridIndex(x0, zInner);
                        const std::uint32_t i1 = gridIndex(x1, zInner);
                        const std::uint32_t i2 = gridIndex(x2, zInner);

                        gridIndices.push_back(o0);
                        gridIndices.push_back(i0);
                        gridIndices.push_back(i1);

                        gridIndices.push_back(o0);
                        gridIndices.push_back(i1);
                        gridIndices.push_back(o2);

                        gridIndices.push_back(o2);
                        gridIndices.push_back(i1);
                        gridIndices.push_back(i2);

                        continue;
                    }

                    if(stitchNegativeZ &&
                    localZ == 0u &&
                    (localX % 2u) == 1u)
                    {
                        // The preceding X iteration already emitted these cells.
                        continue;
                    }

                    // Stitch the positive-Z edge against a patch one LOD coarser.
                    if(stitchPositiveZ &&
                    localZ == patch.subdivisions - 1 &&
                    (localX % 2u) == 0u)
                    {
                        const std::uint32_t zInner = z;
                        const std::uint32_t zOuter = z + vertexStep;

                        const std::uint32_t x0 = x;
                        const std::uint32_t x1 = x + vertexStep;
                        const std::uint32_t x2 = x + 2u * vertexStep;

                        const std::uint32_t i0 = gridIndex(x0, zInner);
                        const std::uint32_t i1 = gridIndex(x1, zInner);
                        const std::uint32_t i2 = gridIndex(x2, zInner);

                        const std::uint32_t o0 = gridIndex(x0, zOuter);
                        const std::uint32_t o2 = gridIndex(x2, zOuter);

                        gridIndices.push_back(i0);
                        gridIndices.push_back(o0);
                        gridIndices.push_back(i1);

                        gridIndices.push_back(i1);
                        gridIndices.push_back(o0);
                        gridIndices.push_back(o2);

                        gridIndices.push_back(i1);
                        gridIndices.push_back(o2);
                        gridIndices.push_back(i2);

                        continue;
                    }

                    if(stitchPositiveZ &&
                    localZ == patch.subdivisions - 1 &&
                    (localX % 2u) == 1u)
                    {
                        // The preceding X iteration already emitted these cells.
                        continue;
                    }

                    const std::uint32_t a =
                        x * (gridResolution + 1) + z;

                    const std::uint32_t b = a + vertexStep;

                    const std::uint32_t c =
                        b + vertexStep * (gridResolution + 1);

                    const std::uint32_t d =
                        a + vertexStep * (gridResolution + 1);

                    gridIndices.push_back(a);
                    gridIndices.push_back(b);
                    gridIndices.push_back(c);

                    gridIndices.push_back(a);
                    gridIndices.push_back(c);
                    gridIndices.push_back(d);
                }
            }

            patch.indexCount =
                static_cast<std::uint32_t>(gridIndices.size()) -
                patch.firstIndex;

            patches.push_back(patch);
        }
    }

    // Reconstruct normals from the displaced grid.
    const std::uint32_t rowStride = gridResolution + 1;

    auto gridPosition = [&](std::uint32_t x, std::uint32_t z)
    {
        const Vertex& vertex =
            gridVertices[x * rowStride + z];

        return Vector3{
            vertex.position[0],
            vertex.position[1],
            vertex.position[2]
        };
    };

    for(std::uint32_t x = 0; x <= gridResolution; ++x)
    {
        for(std::uint32_t z = 0; z <= gridResolution; ++z)
        {
            const std::uint32_t xPrev = x > 0 ? x - 1 : x;
            const std::uint32_t xNext =
                x < gridResolution ? x + 1 : x;

            const std::uint32_t zPrev = z > 0 ? z - 1 : z;
            const std::uint32_t zNext =
                z < gridResolution ? z + 1 : z;

            const Vector3 dx =
                gridPosition(xNext, z) -
                gridPosition(xPrev, z);

            const Vector3 dz =
                gridPosition(x, zNext) -
                gridPosition(x, zPrev);

            const Vector3 normal =
                Vector3::Cross(dz, dx).Normalized();

            Vertex& vertex =
                gridVertices[x * rowStride + z];

            vertex.normal[0] = normal.x;
            vertex.normal[1] = normal.y;
            vertex.normal[2] = normal.z;
        }
    }

    return geometry;
}
