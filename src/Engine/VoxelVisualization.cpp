#include "Engine/VoxelVisualization.h"

#include "Voxel/VoxelManifest.h"
#include "Voxel/VoxelRegion.h"
#include "Voxel/VoxelResidency.h"

#include <bit>
#include <exception>
#include <utility>

namespace VoxelVisualization
{
namespace
{
using EditorVoxelOverlay::BoxState;
using EditorVoxelOverlay::CellClass;
using EditorVoxelOverlay::CellScale;

enum class Kind
{
    Outside,
    Surface,
    Interior
};

// Bit order of the face mask; matches EditorVoxel.hlsl.
constexpr int FaceDirections[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};

// Emits one decoded region's cubes.
class RegionBuilder
{
public:
    RegionBuilder(const Voxel::RegionData& region, std::uint32_t slot, std::vector<EditorVoxelOverlay::Cell>& cells, std::size_t maxCells,
        const std::atomic<bool>* cancel) :
        m_region(region),
        m_slot(slot),
        m_cells(cells),
        m_maxCells(maxCells),
        m_cancel(cancel),
        m_bricks(region.regionBricks),
        m_regionVoxels(static_cast<int>(region.regionBricks) * Voxel::BrickVoxels)
    {
    }

    // False when the cell limit was reached or the build was cancelled (checked once per row of bricks).
    bool Emit()
    {
        for(int brickZ = 0; brickZ < m_bricks; ++brickZ)
        {
            for(int brickY = 0; brickY < m_bricks; ++brickY)
            {
                if(m_cancel != nullptr && m_cancel->load())
                {
                    return false;
                }

                for(int brickX = 0; brickX < m_bricks; ++brickX)
                {
                    if(!EmitBrick(brickX, brickY, brickZ))
                    {
                        return false;
                    }
                }
            }
        }

        return true;
    }

private:
    std::uint32_t BrickValue(int brickX, int brickY, int brickZ) const
    {
        return m_region.brickIndex[static_cast<std::size_t>(brickX + m_bricks * (brickY + m_bricks * brickZ))];
    }

    Kind Classify(int x, int y, int z) const
    {
        const std::uint32_t value = BrickValue(x >> Voxel::BrickVoxelsLog2, y >> Voxel::BrickVoxelsLog2, z >> Voxel::BrickVoxelsLog2);

        if(value == Voxel::UniformOutside)
        {
            return Kind::Outside;
        }

        if(value == Voxel::UniformInside)
        {
            return Kind::Interior;
        }

        const int voxelX = x & (Voxel::BrickVoxels - 1);
        const int voxelY = y & (Voxel::BrickVoxels - 1);
        const int voxelZ = z & (Voxel::BrickVoxels - 1);

        if(Voxel::TestMask(m_region.occupancy[value], voxelX, voxelY, voxelZ))
        {
            return Kind::Surface;
        }

        return Voxel::TestMask(m_region.solid[value], voxelX, voxelY, voxelZ) ? Kind::Interior : Kind::Outside;
    }

    // A face of the block [origin, origin + size)^3 is exposed when any voxel across it is empty, or lies outside the region.
    std::uint32_t ExposedFaces(int x, int y, int z, int size) const
    {
        std::uint32_t mask = 0;
        const int origin[3] = {x, y, z};

        for(int face = 0; face < 6; ++face)
        {
            const int axis = face / 2;
            const int first = (axis + 1) % 3;
            const int second = (axis + 2) % 3;
            int neighbour[3] = {x, y, z};
            neighbour[axis] = FaceDirections[face][axis] < 0 ? origin[axis] - 1 : origin[axis] + size;
            bool exposed = neighbour[axis] < 0 || neighbour[axis] >= m_regionVoxels;

            for(int a = 0; a < size && !exposed; ++a)
            {
                for(int b = 0; b < size && !exposed; ++b)
                {
                    neighbour[first] = origin[first] + a;
                    neighbour[second] = origin[second] + b;
                    exposed = Classify(neighbour[0], neighbour[1], neighbour[2]) == Kind::Outside;
                }
            }

            mask |= exposed ? 1u << face : 0u;
        }

        return mask;
    }

    bool Push(std::uint32_t faces, CellClass cellClass, CellScale scale, int x, int y, int z)
    {
        if(faces == 0)
        {
            return true;
        }

        if(m_cells.size() >= m_maxCells)
        {
            return false;
        }

        m_cells.push_back(EditorVoxelOverlay::PackCell(m_slot, faces, cellClass, scale, static_cast<std::uint32_t>(x),
            static_cast<std::uint32_t>(y), static_cast<std::uint32_t>(z)));
        return true;
    }

    bool EmitBrick(int brickX, int brickY, int brickZ)
    {
        const std::uint32_t value = BrickValue(brickX, brickY, brickZ);
        const int originX = brickX * Voxel::BrickVoxels;
        const int originY = brickY * Voxel::BrickVoxels;
        const int originZ = brickZ * Voxel::BrickVoxels;

        if(value == Voxel::UniformOutside)
        {
            return true;
        }

        if(value == Voxel::UniformInside)
        {
            return Push(ExposedFaces(originX, originY, originZ, Voxel::BrickVoxels), CellClass::Interior, CellScale::Brick, originX,
                originY, originZ);
        }

        for(int voxelZ = 0; voxelZ < Voxel::BrickVoxels; ++voxelZ)
        {
            for(int voxelY = 0; voxelY < Voxel::BrickVoxels; ++voxelY)
            {
                for(int voxelX = 0; voxelX < Voxel::BrickVoxels; ++voxelX)
                {
                    const int x = originX + voxelX;
                    const int y = originY + voxelY;
                    const int z = originZ + voxelZ;
                    const Kind kind = Classify(x, y, z);

                    if(kind == Kind::Outside)
                    {
                        continue;
                    }

                    const CellClass cellClass = kind == Kind::Surface ? CellClass::Surface : CellClass::Interior;

                    if(!Push(ExposedFaces(x, y, z, 1), cellClass, CellScale::Voxel, x, y, z))
                    {
                        return false;
                    }
                }
            }
        }

        return true;
    }

    const Voxel::RegionData& m_region;
    std::uint32_t m_slot;
    std::vector<EditorVoxelOverlay::Cell>& m_cells;
    std::size_t m_maxCells;
    const std::atomic<bool>* m_cancel;
    int m_bricks;
    int m_regionVoxels;
};

// Reads one listed region and checks it is the region the manifest says (shared validation, VoxelResidency.h).
std::string LoadRegion(const Voxel::AssetStore& store, const Voxel::ManifestData& manifest, const Voxel::ManifestEntry& entry,
    Voxel::RegionData& data)
{
    const auto bytes = store.Read(entry.reference);

    if(!bytes)
    {
        return "Missing: " + bytes.error();
    }

    return Voxel::Residency::DecodeListedRegion(manifest, entry, *bytes, data);
}

void CountVoxels(const Voxel::RegionData& data, Result& result)
{
    for(std::size_t slot = 0; slot < data.occupancy.size(); ++slot)
    {
        for(std::size_t word = 0; word < data.occupancy[slot].size(); ++word)
        {
            result.surfaceVoxels += static_cast<std::uint64_t>(std::popcount(data.occupancy[slot][word]));
            result.interiorVoxels += static_cast<std::uint64_t>(std::popcount(data.solid[slot][word] & ~data.occupancy[slot][word]));
        }
    }

    for(const std::uint32_t value : data.brickIndex)
    {
        result.interiorVoxels += value == Voxel::UniformInside ? Voxel::BrickVoxelCount : 0;
    }
}
}

Result Build(const Voxel::AssetStore& store, const std::string& manifestReference, const std::optional<Voxel::RegionKey>& isolate,
    const std::atomic<bool>* cancel, std::size_t maxCells)
{
    Result result;
    result.manifestReference = manifestReference;
    result.isolated = isolate;

    // The manifest must read and decode; without it nothing is shown.
    const auto manifestBytes = store.Read(manifestReference);

    if(!manifestBytes)
    {
        result.error = "The manifest is missing: " + manifestBytes.error();
        return result;
    }

    Voxel::ManifestData manifest;

    if(const Voxel::DecodeResult decoded = Voxel::DecodeManifest(*manifestBytes, manifest); !decoded)
    {
        result.error = "The manifest does not decode: " + decoded.message;
        return result;
    }

    if(manifest.entries.size() > EditorVoxelOverlay::MaximumRegions)
    {
        result.error = "The manifest lists more regions than the visualization supports (" +
            std::to_string(EditorVoxelOverlay::MaximumRegions) + ").";
        return result;
    }

    result.baseExponent = manifest.baseExponent;
    result.anchorRegion = manifest.minimumRegion;
    result.data.regionVoxels = static_cast<std::uint32_t>(manifest.regionBricks) * Voxel::BrickVoxels;
    result.regions.reserve(manifest.entries.size());

    for(const Voxel::ManifestEntry& entry : manifest.entries)
    {
        result.regions.push_back({entry.key, entry.kind == Voxel::RegionKind::UniformInside, entry.brickCount});
    }

    // One region at a time: each decoded region is released before the next is read.
    for(std::size_t index = 0; index < manifest.entries.size(); ++index)
    {
        const Voxel::ManifestEntry& entry = manifest.entries[index];

        if(isolate && entry.key != *isolate)
        {
            continue;
        }

        if(cancel != nullptr && cancel->load())
        {
            result.cancelled = true;
            return result;
        }

        const auto slot = static_cast<std::uint32_t>(result.data.regions.size());
        EditorVoxelOverlay::Region region;
        region.offset[0] = entry.key.x - result.anchorRegion[0];
        region.offset[1] = entry.key.y - result.anchorRegion[1];
        region.offset[2] = entry.key.z - result.anchorRegion[2];

        if(entry.kind == Voxel::RegionKind::UniformInside)
        {
            region.state = static_cast<std::uint32_t>(BoxState::UniformInside);
            result.data.regions.push_back(region);
            const std::uint64_t voxels = result.data.regionVoxels;
            result.interiorVoxels += voxels * voxels * voxels;

            if(!result.truncated && result.data.cells.size() >= maxCells)
            {
                result.truncated = true;
                result.firstUndrawnRegion = index;
            }

            if(!result.truncated)
            {
                result.data.cells.push_back(EditorVoxelOverlay::PackCell(slot, EditorVoxelOverlay::AllFaces, CellClass::Interior,
                    CellScale::Region, 0, 0, 0));
            }

            continue;
        }

        Voxel::RegionData data;
        const std::string error = LoadRegion(store, manifest, entry, data);

        if(!error.empty())
        {
            region.state = static_cast<std::uint32_t>(BoxState::Error);
            result.data.regions.push_back(region);
            result.regionErrors.push_back({entry.key, error});
            continue;
        }

        region.state = static_cast<std::uint32_t>(BoxState::Populated);
        result.data.regions.push_back(region);
        CountVoxels(data, result);

        // After truncation, later regions keep their boxes (so they are visibly present) but get no cubes.
        if(!result.truncated && !RegionBuilder(data, slot, result.data.cells, maxCells, cancel).Emit())
        {
            if(cancel != nullptr && cancel->load())
            {
                result.cancelled = true;
                return result;
            }

            result.truncated = true;
            result.firstUndrawnRegion = index;
        }
    }

    return result;
}

Matrix4 WorldFromLattice(const Result& result)
{
    // The anchor's origin is exact in double (VoxelToWorld); only the final translation is rounded to float.
    const std::int64_t regionVoxels = result.data.regionVoxels;
    const double x = Voxel::VoxelToWorld(result.anchorRegion[0] * regionVoxels, result.baseExponent);
    const double y = Voxel::VoxelToWorld(result.anchorRegion[1] * regionVoxels, result.baseExponent);
    const double z = Voxel::VoxelToWorld(result.anchorRegion[2] * regionVoxels, result.baseExponent);
    const float voxelSize = static_cast<float>(Voxel::VoxelToWorld(1, result.baseExponent));
    return Matrix4::Translation(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)) *
        Matrix4::Scale(voxelSize, voxelSize, voxelSize);
}

Job::Job(std::shared_ptr<const Voxel::AssetStore> store, std::string manifestReference, std::optional<Voxel::RegionKey> isolate)
{
    m_thread = std::thread([this, store = std::move(store), manifestReference = std::move(manifestReference), isolate] {
        try
        {
            m_result = Build(*store, manifestReference, isolate, &m_cancel);
        }
        catch(const std::exception& exception)
        {
            m_result = {};
            m_result.manifestReference = manifestReference;
            m_result.error = std::string("Building the voxel visualization failed: ") + exception.what();
        }

        m_finished.store(true);
    });
}

Job::~Job()
{
    m_cancel.store(true);

    if(m_thread.joinable())
    {
        m_thread.join();
    }
}

bool Job::IsFinished() const noexcept
{
    return m_finished.load();
}

Result Job::Take()
{
    if(m_thread.joinable())
    {
        m_thread.join();
    }

    return std::move(m_result);
}
}
