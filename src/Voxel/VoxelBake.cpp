#include "Voxel/VoxelBake.h"

#include "Voxel/VoxelAcceleration.h"
#include "Voxel/VoxelBinary.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <limits>

namespace Voxel
{
namespace
{
    // A candidate triangle in lattice units (world / voxel size, exact).
    struct Candidate
    {
        std::array<Point, 3> corners;
        Point minimum;
        Point maximum;
        // Belongs to a closed piece: can change classification.
        bool closed = false;
        // Plane n . p = offset, for a cheap strict rejection before the full separating-axis test.
        Point normal;
        double offset = 0.0;
        double normalL1 = 0.0;
    };

    using Clock = std::chrono::steady_clock;

    // Adds the scope's wall-clock time to a statistic.
    class StageTimer
    {
    public:
        explicit StageTimer(double& total) :
            m_total(total),
            m_start(Clock::now())
        {
        }

        ~StageTimer()
        {
            m_total += std::chrono::duration<double>(Clock::now() - m_start).count();
        }

    private:
        double& m_total;
        Clock::time_point m_start;
    };

    std::int64_t FloorToInt(double value) noexcept
    {
        return static_cast<std::int64_t>(std::floor(value));
    }

    class RegionBaker
    {
    public:
        RegionBaker(const GeometrySnapshot& snapshot, const BakeAcceleration& acceleration, const RegionKey& key, const BakeSettings& settings,
            int regionBricksLog2) :
            m_snapshot(snapshot),
            m_acceleration(acceleration),
            m_settings(settings),
            m_sizeExponent(settings.baseExponent + key.level),
            m_regionBricks(settings.regionBricks),
            m_band(settings.narrowBand),
            m_clamp(settings.coarseClamp),
            m_distanceRing((settings.narrowBand + 1 + BrickVoxels - 1) / BrickVoxels),
            m_margin((std::max)(static_cast<int>(settings.coarseClamp), m_distanceRing)),
            m_gridSize(m_regionBricks + 2 * m_margin)
        {
            const Coordinate originVoxel = RegionOriginVoxel(key, regionBricksLog2);
            m_originBrick = VoxelToBrick(originVoxel);
            m_gridMinimum = {m_originBrick.x - m_margin, m_originBrick.y - m_margin, m_originBrick.z - m_margin};
            m_result.data.key = key;
        }

        std::expected<RegionBakeResult, std::string> Run()
        {
            StageTimer total(m_result.statistics.totalSeconds);

            {
                StageTimer timer(m_result.statistics.candidateSeconds);
                CollectCandidates();
            }

            ComputeCoarse();
            CollectClosedPieces();

            const std::size_t components = LabelUniformCells();

            // Exact classification is bounded before it starts; accelerated classification is bounded as it runs (its exact
            // fallbacks are rare and not predictable in advance).
            const double estimate = m_settings.classification == ClassificationMode::Exact ? EstimateWindingWork(components) : 0.0;

            if(estimate > m_settings.maximumWindingWork)
            {
                return std::unexpected("Region classification would evaluate about " + std::to_string(static_cast<long long>(estimate)) +
                    " triangle solid angles, above the budget of " + std::to_string(static_cast<long long>(m_settings.maximumWindingWork)) +
                    ". Closed pieces overlapping it hold " + std::to_string(m_closedTriangles) + " triangles.");
            }

            ClassifyUniformComponents(components);

            if(m_allocatedCount == 0)
            {
                PublishWindingCounters();
                // No surface in the region: one surface-free component, so one class.
                m_result.kind = m_componentInside.front() ? RegionBakeKind::UniformInside : RegionBakeKind::UniformOutside;
                m_result.data = {};
                return std::move(m_result);
            }

            BuildBricks();
            PublishWindingCounters();

            if(m_overBudget)
            {
                return std::unexpected("Region classification exceeded the budget of " + std::to_string(static_cast<long long>(m_settings.maximumWindingWork)) +
                    " triangle tests. Closed pieces overlapping it hold " + std::to_string(m_closedTriangles) + " triangles.");
            }

            if(std::string error = ValidateRegion(m_result.data); !error.empty())
            {
                return std::unexpected("Baked region fails VX.1 validation: " + error);
            }

            m_result.kind = RegionBakeKind::Populated;
            return std::move(m_result);
        }

    private:
        void PublishWindingCounters() noexcept
        {
            m_result.statistics.rayQueries = m_winding.rayQueries;
            m_result.statistics.exactFallbacks = m_winding.exactFallbacks;
        }

        int GridLinear(const Coordinate& brick) const noexcept
        {
            const auto x = static_cast<int>(brick.x - m_gridMinimum.x);
            const auto y = static_cast<int>(brick.y - m_gridMinimum.y);
            const auto z = static_cast<int>(brick.z - m_gridMinimum.z);
            return x + m_gridSize * (y + m_gridSize * z);
        }

        bool InGrid(const Coordinate& brick) const noexcept
        {
            return brick.x >= m_gridMinimum.x && brick.y >= m_gridMinimum.y && brick.z >= m_gridMinimum.z &&
                brick.x < m_gridMinimum.x + m_gridSize && brick.y < m_gridMinimum.y + m_gridSize && brick.z < m_gridMinimum.z + m_gridSize;
        }

        int CellLinear(int x, int y, int z) const noexcept
        {
            return x + m_regionBricks * (y + m_regionBricks * z);
        }

        // Every triangle whose bounds reach the brick grid (the region plus its halo), binned by the bricks its padded bounds
        // overlap. Bins are a superset of the triangles that can touch a brick; the exact tests decide.
        void CollectCandidates()
        {
            const Point gridLow{static_cast<double>(m_gridMinimum.x * BrickVoxels), static_cast<double>(m_gridMinimum.y * BrickVoxels),
                static_cast<double>(m_gridMinimum.z * BrickVoxels)};
            const double extent = static_cast<double>(m_gridSize * BrickVoxels);

            m_bins.assign(static_cast<std::size_t>(m_gridSize) * m_gridSize * m_gridSize, {});

            // Every triangle whose world bounds overlap the grid box plus one voxel, in snapshot order: the same set the bounds
            // test below accepts, since scaling by a power of two is exact and monotone. World partitioning hashes this same set.
            const WorldBox bounds = RegionGeometryBounds(m_result.data.key, m_settings);
            std::vector<std::uint32_t> nearby;
            m_acceleration.triangles.Query(bounds.minimum, bounds.maximum, nearby);
            const auto triangles = m_snapshot.GetTriangles();
            const auto pieces = m_snapshot.GetPieces();

            for(const std::uint32_t triangleIndex : nearby)
            {
                const SnapshotTriangle& source = triangles[triangleIndex];
                Candidate candidate;
                candidate.closed = pieces[source.piece].closed;
                candidate.minimum.fill(std::numeric_limits<double>::infinity());
                candidate.maximum.fill(-std::numeric_limits<double>::infinity());

                for(std::size_t corner = 0; corner < 3; ++corner)
                {
                    for(std::size_t axis = 0; axis < 3; ++axis)
                    {
                        const double value = std::ldexp(source.corners[corner][axis], -m_sizeExponent);
                        candidate.corners[corner][axis] = value;
                        candidate.minimum[axis] = (std::min)(candidate.minimum[axis], value);
                        candidate.maximum[axis] = (std::max)(candidate.maximum[axis], value);
                    }
                }

                const auto& c = candidate.corners;
                const Point u{c[1][0] - c[0][0], c[1][1] - c[0][1], c[1][2] - c[0][2]};
                const Point v{c[2][0] - c[0][0], c[2][1] - c[0][1], c[2][2] - c[0][2]};
                candidate.normal = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
                candidate.offset = candidate.normal[0] * c[0][0] + candidate.normal[1] * c[0][1] + candidate.normal[2] * c[0][2];
                candidate.normalL1 = std::abs(candidate.normal[0]) + std::abs(candidate.normal[1]) + std::abs(candidate.normal[2]);

                bool overlaps = true;
                for(std::size_t axis = 0; axis < 3; ++axis)
                {
                    overlaps = overlaps && candidate.maximum[axis] >= gridLow[axis] - 1.0 && candidate.minimum[axis] <= gridLow[axis] + extent + 1.0;
                }

                if(!overlaps)
                {
                    continue;
                }

                const auto index = static_cast<std::uint32_t>(m_candidates.size());
                m_candidates.push_back(candidate);

                // One brick of slack on each side keeps the bins a superset despite rounding at brick faces.
                std::int64_t first[3];
                std::int64_t last[3];
                const std::int64_t gridMinimum[3] = {m_gridMinimum.x, m_gridMinimum.y, m_gridMinimum.z};

                for(std::size_t axis = 0; axis < 3; ++axis)
                {
                    first[axis] = (std::max)(FloorShift(FloorToInt(candidate.minimum[axis]) - 1, BrickVoxelsLog2), gridMinimum[axis]);
                    last[axis] = (std::min)(FloorShift(FloorToInt(candidate.maximum[axis]) + 1, BrickVoxelsLog2), gridMinimum[axis] + m_gridSize - 1);
                }

                for(std::int64_t z = first[2]; z <= last[2]; ++z)
                {
                    for(std::int64_t y = first[1]; y <= last[1]; ++y)
                    {
                        for(std::int64_t x = first[0]; x <= last[0]; ++x)
                        {
                            m_bins[GridLinear({x, y, z})].push_back(index);
                        }
                    }
                }
            }

            m_masks.assign(m_bins.size(), {});
            m_closedMasks.assign(m_bins.size(), {});
            m_masksDone.assign(m_bins.size(), 0);
            m_surfaceFound.assign(m_bins.size(), 0);
            m_visited.assign(m_candidates.size(), 0);
            m_result.statistics.candidateTriangles = m_candidates.size();
        }

        // Surface occupancy of one brick of the grid: a pure function of the brick, whichever region asks.
        const BrickMask& BrickOccupancy(const Coordinate& brick)
        {
            const int index = GridLinear(brick);

            if(!m_masksDone[index])
            {
                ComputeOccupancy(brick, false);
            }

            return m_masks[index];
        }

        // Whether the brick has any surface voxel; stops at the first one when the full mask is not needed (the coarse halo).
        bool HasSurface(const Coordinate& brick)
        {
            const int index = GridLinear(brick);

            if(m_bins[index].empty())
            {
                return false;
            }

            if(m_masksDone[index])
            {
                const BrickMask& mask = m_masks[index];
                return std::any_of(mask.begin(), mask.end(), [](std::uint64_t word) { return word != 0; });
            }

            // A hit records only that the brick has surface; a miss leaves its full (empty) masks computed.
            return m_surfaceFound[index] != 0 || ComputeOccupancy(brick, true);
        }

        // Fills the brick's masks and returns whether any voxel is touched, or with `stopAtFirst` returns true at the first touched
        // voxel without storing masks.
        bool ComputeOccupancy(const Coordinate& brick, bool stopAtFirst)
        {
            const int index = GridLinear(brick);
            StageTimer timer(m_result.statistics.occupancySeconds);
            BrickMask mask{};
            BrickMask closedMask{};
            const std::int64_t base[3] = {brick.x * BrickVoxels, brick.y * BrickVoxels, brick.z * BrickVoxels};
            constexpr double HalfExtent = 0.5 + OccupancyPadding;

            for(const std::uint32_t candidateIndex : m_bins[index])
            {
                const Candidate& candidate = m_candidates[candidateIndex];
                std::int64_t first[3];
                std::int64_t last[3];

                for(std::size_t axis = 0; axis < 3; ++axis)
                {
                    first[axis] = (std::max)(FloorToInt(candidate.minimum[axis]) - 1, base[axis]);
                    last[axis] = (std::min)(FloorToInt(candidate.maximum[axis]) + 1, base[axis] + BrickVoxels - 1);
                }

                for(std::int64_t z = first[2]; z <= last[2]; ++z)
                {
                    for(std::int64_t y = first[1]; y <= last[1]; ++y)
                    {
                        for(std::int64_t x = first[0]; x <= last[0]; ++x)
                        {
                            const int bit = BrickVoxelBit(static_cast<int>(x - base[0]), static_cast<int>(y - base[1]), static_cast<int>(z - base[2]));
                            const std::uint64_t flag = std::uint64_t{1} << (bit & 63);

                            // A voxel already known touched needs no further test, except by a closed triangle when only open ones
                            // touched it so far.
                            if((candidate.closed ? closedMask[bit >> 6] : mask[bit >> 6]) & flag)
                            {
                                continue;
                            }

                            const Point centre{static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5, static_cast<double>(z) + 0.5};

                            // Clearly beyond the triangle's plane by more than the cube's projected radius: the plane axis of the
                            // full test separates it too. The margin keeps this strictly weaker than that test.
                            const double planeDistance = candidate.normal[0] * centre[0] + candidate.normal[1] * centre[1] +
                                candidate.normal[2] * centre[2] - candidate.offset;
                            if(std::abs(planeDistance) >
                                HalfExtent * candidate.normalL1 * (1.0 + 1e-6) + 1e-9 * (std::abs(candidate.offset) + candidate.normalL1))
                            {
                                continue;
                            }

                            ++m_result.statistics.triangleCubeTests;

                            if(TriangleTouchesCube(centre, HalfExtent, candidate.corners[0], candidate.corners[1], candidate.corners[2]))
                            {
                                if(stopAtFirst)
                                {
                                    m_surfaceFound[index] = 1;
                                    return true;
                                }

                                mask[bit >> 6] |= flag;
                                if(candidate.closed)
                                {
                                    closedMask[bit >> 6] |= flag;
                                }
                            }
                        }
                    }
                }
            }

            m_masks[index] = mask;
            m_closedMasks[index] = closedMask;
            m_masksDone[index] = 1;
            return std::any_of(mask.begin(), mask.end(), [](std::uint64_t word) { return word != 0; });
        }

        // Voxels touched by a triangle of a closed piece: the only voxels across which classification can change.
        bool VoxelTouchedByClosed(const Coordinate& voxel)
        {
            const Coordinate brick = VoxelToBrick(voxel);
            BrickOccupancy(brick);
            return TestMask(m_closedMasks[GridLinear(brick)], static_cast<int>(voxel.x & 7), static_cast<int>(voxel.y & 7), static_cast<int>(voxel.z & 7));
        }

        bool VoxelOccupied(const Coordinate& voxel)
        {
            const Coordinate brick = VoxelToBrick(voxel);
            return TestMask(BrickOccupancy(brick), static_cast<int>(voxel.x & 7), static_cast<int>(voxel.y & 7), static_cast<int>(voxel.z & 7));
        }

        // Saturated Chebyshev distance to the nearest surface brick, over the whole grid (the region plus at least the coarse
        // clamp on every side), then read for the region's cells.
        void ComputeCoarse()
        {
            const int saturated = m_clamp + 1;
            std::vector<std::uint8_t> distance(m_bins.size());

            for(int z = 0; z < m_gridSize; ++z)
            {
                for(int y = 0; y < m_gridSize; ++y)
                {
                    for(int x = 0; x < m_gridSize; ++x)
                    {
                        const Coordinate brick{m_gridMinimum.x + x, m_gridMinimum.y + y, m_gridMinimum.z + z};
                        const int index = GridLinear(brick);
                        distance[index] = HasSurface(brick) ? 0 : static_cast<std::uint8_t>(saturated);
                    }
                }
            }

            std::vector<std::uint8_t> window(distance.size());
            std::vector<std::uint8_t> next(distance.size());

            for(int pass = 0; pass < m_clamp; ++pass)
            {
                window = distance;

                for(int axis = 0; axis < 3; ++axis)
                {
                    for(int z = 0; z < m_gridSize; ++z)
                    {
                        for(int y = 0; y < m_gridSize; ++y)
                        {
                            for(int x = 0; x < m_gridSize; ++x)
                            {
                                int position[3] = {x, y, z};
                                const int index = x + m_gridSize * (y + m_gridSize * z);
                                std::uint8_t value = window[index];

                                for(int step = -1; step <= 1; step += 2)
                                {
                                    const int moved = position[axis] + step;
                                    if(moved >= 0 && moved < m_gridSize)
                                    {
                                        int neighbour[3] = {x, y, z};
                                        neighbour[axis] = moved;
                                        value = (std::min)(value, window[neighbour[0] + m_gridSize * (neighbour[1] + m_gridSize * neighbour[2])]);
                                    }
                                }

                                next[index] = value;
                            }
                        }
                    }

                    window.swap(next);
                }

                for(std::size_t index = 0; index < distance.size(); ++index)
                {
                    distance[index] = (std::min)(distance[index], static_cast<std::uint8_t>((std::min)(window[index] + 1, saturated)));
                }
            }

            const std::size_t cells = static_cast<std::size_t>(m_regionBricks) * m_regionBricks * m_regionBricks;
            m_result.data.coarseDistance.assign(cells, 0);
            m_allocated.assign(cells, 0);

            for(int z = 0; z < m_regionBricks; ++z)
            {
                for(int y = 0; y < m_regionBricks; ++y)
                {
                    for(int x = 0; x < m_regionBricks; ++x)
                    {
                        const Coordinate brick{m_originBrick.x + x, m_originBrick.y + y, m_originBrick.z + z};
                        const std::uint8_t value = distance[GridLinear(brick)];
                        m_result.data.coarseDistance[CellLinear(x, y, z)] = value;
                        m_allocated[CellLinear(x, y, z)] = value == 0;
                        m_allocatedCount += value == 0;
                    }
                }
            }
        }

        // Closed pieces whose bounds reach the region or its apron. A closed piece winds zero times around any point outside its
        // bounds, so the others contribute exactly nothing.
        void CollectClosedPieces()
        {
            const WorldBox bounds = RegionClassificationBounds(m_result.data.key, m_settings);

            for(const SnapshotPiece& piece : m_snapshot.GetPieces())
            {
                if(piece.closed && PieceOverlaps(piece, bounds))
                {
                    m_closedPieces.push_back(&piece);
                    m_closedTriangles += piece.triangleCount;
                }
            }

            m_result.statistics.closedPieces = m_closedPieces.size();
        }

        // Upper bound on the solid angles classification evaluates: every closed triangle for each uniform component, and for
        // each allocated brick, at most one evaluation per apron voxel against the closed pieces whose bounds reach that brick.
        double EstimateWindingWork(std::size_t components) const
        {
            double estimate = static_cast<double>(components) * static_cast<double>(m_closedTriangles);

            for(int z = 0; z < m_regionBricks; ++z)
            {
                for(int y = 0; y < m_regionBricks; ++y)
                {
                    for(int x = 0; x < m_regionBricks; ++x)
                    {
                        if(!m_allocated[CellLinear(x, y, z)])
                        {
                            continue;
                        }

                        const Point low{std::ldexp(static_cast<double>((m_originBrick.x + x) * BrickVoxels - 1), m_sizeExponent),
                            std::ldexp(static_cast<double>((m_originBrick.y + y) * BrickVoxels - 1), m_sizeExponent),
                            std::ldexp(static_cast<double>((m_originBrick.z + z) * BrickVoxels - 1), m_sizeExponent)};
                        const double size = std::ldexp(static_cast<double>(ApronBrickVoxels), m_sizeExponent);
                        std::uint64_t triangles = 0;

                        for(const SnapshotPiece* piece : m_closedPieces)
                        {
                            bool overlaps = true;
                            for(std::size_t axis = 0; axis < 3; ++axis)
                            {
                                overlaps = overlaps && piece->maximum[axis] >= low[axis] && piece->minimum[axis] <= low[axis] + size;
                            }
                            triangles += overlaps ? piece->triangleCount : 0;
                        }

                        estimate += static_cast<double>(ApronBrickVoxelCount) * static_cast<double>(triangles);
                    }
                }
            }

            return estimate;
        }

        // Inside when the closed pieces' winding numbers, each rounded on its own, sum to at least one. `point` is in lattice units.
        bool InsideAt(const Point& point)
        {
            const Point world{std::ldexp(point[0], m_sizeExponent), std::ldexp(point[1], m_sizeExponent), std::ldexp(point[2], m_sizeExponent)};
            StageTimer timer(m_result.statistics.classificationSeconds);
            long long total = 0;
            ++m_result.statistics.windingEvaluations;

            const auto pieces = m_snapshot.GetPieces();

            for(const SnapshotPiece* piece : m_closedPieces)
            {
                if(world[0] < piece->minimum[0] || world[0] > piece->maximum[0] || world[1] < piece->minimum[1] || world[1] > piece->maximum[1] ||
                    world[2] < piece->minimum[2] || world[2] > piece->maximum[2])
                {
                    continue;
                }

                if(m_settings.classification == ClassificationMode::Exact)
                {
                    total += ExactPieceWinding(m_snapshot, *piece, world);
                    m_result.statistics.windingWork += piece->triangleCount;
                    continue;
                }

                const std::uint64_t before = m_winding.triangleTests + m_winding.exactTriangles;
                total += m_acceleration.winding.PieceWinding(static_cast<std::uint32_t>(piece - pieces.data()), world, m_winding);
                m_result.statistics.windingWork += m_winding.triangleTests + m_winding.exactTriangles - before;
            }

            if(static_cast<double>(m_result.statistics.windingWork) > m_settings.maximumWindingWork)
            {
                m_overBudget = true;
            }

            return total >= 1;
        }

        // Connected sets of surface-free cells: no surface separates them, so each set has one class.
        std::size_t LabelUniformCells()
        {
            const std::size_t cells = m_allocated.size();
            m_component.assign(cells, -1);
            std::vector<int> stack;
            int components = 0;

            for(int start = 0; start < static_cast<int>(cells); ++start)
            {
                if(m_allocated[start] || m_component[start] >= 0)
                {
                    continue;
                }

                m_componentFirstCell.push_back(start);
                m_component[start] = components;
                stack.push_back(start);

                while(!stack.empty())
                {
                    const int cell = stack.back();
                    stack.pop_back();
                    const int x = cell % m_regionBricks;
                    const int y = (cell / m_regionBricks) % m_regionBricks;
                    const int z = cell / (m_regionBricks * m_regionBricks);
                    const int neighbours[6][3] = {{x - 1, y, z}, {x + 1, y, z}, {x, y - 1, z}, {x, y + 1, z}, {x, y, z - 1}, {x, y, z + 1}};

                    for(const auto& n : neighbours)
                    {
                        if(n[0] < 0 || n[1] < 0 || n[2] < 0 || n[0] >= m_regionBricks || n[1] >= m_regionBricks || n[2] >= m_regionBricks)
                        {
                            continue;
                        }

                        const int next = CellLinear(n[0], n[1], n[2]);
                        if(!m_allocated[next] && m_component[next] < 0)
                        {
                            m_component[next] = components;
                            stack.push_back(next);
                        }
                    }
                }

                ++components;
            }

            return static_cast<std::size_t>(components);
        }

        void ClassifyUniformComponents(std::size_t components)
        {
            m_componentInside.assign(components, false);

            for(std::size_t component = 0; component < components; ++component)
            {
                const int cell = m_componentFirstCell[component];
                const int x = cell % m_regionBricks;
                const int y = (cell / m_regionBricks) % m_regionBricks;
                const int z = cell / (m_regionBricks * m_regionBricks);
                // The brick's centre: four voxels from every face, so far from any surface.
                const Point centre{static_cast<double>((m_originBrick.x + x) * BrickVoxels + 4), static_cast<double>((m_originBrick.y + y) * BrickVoxels + 4),
                    static_cast<double>((m_originBrick.z + z) * BrickVoxels + 4)};
                m_componentInside[component] = InsideAt(centre);
            }
        }

        // The triangles near one brick, binned into a local grid that covers every point within the band of the brick's voxel
        // centres (apron included), so each voxel searches only the cells around it.
        struct DistanceGrid
        {
            Point origin;
            double cellSize = 2.0;
            int cells = 0;
            // Cell c holds entries[cellStart[c], cellStart[c + 1]): candidate indices, ascending. Empty when `ranked` is used.
            std::vector<std::uint32_t> cellStart;
            std::vector<std::uint32_t> entries;
            // With few candidates a linear scan beats walking cells: every candidate with its lower bound, over all voxel centres of
            // the brick and apron, on its squared distance, nearest first (ties by index).
            std::vector<std::pair<double, std::uint32_t>> ranked;
            bool linear = false;
        };

        // Below this many candidates per brick, the ranked linear scan is faster than the cell walk (measured on the VX.2 scenes).
        static constexpr std::size_t LinearDistanceCandidates = 96;

        DistanceGrid BuildDistanceGrid(const Coordinate& brick) const
        {
            std::vector<std::uint32_t> indices;

            for(std::int64_t z = brick.z - m_distanceRing; z <= brick.z + m_distanceRing; ++z)
            {
                for(std::int64_t y = brick.y - m_distanceRing; y <= brick.y + m_distanceRing; ++y)
                {
                    for(std::int64_t x = brick.x - m_distanceRing; x <= brick.x + m_distanceRing; ++x)
                    {
                        const auto& bin = m_bins[GridLinear({x, y, z})];
                        indices.insert(indices.end(), bin.begin(), bin.end());
                    }
                }
            }

            std::sort(indices.begin(), indices.end());
            indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

            DistanceGrid grid;

            if(indices.size() <= LinearDistanceCandidates)
            {
                grid.linear = true;
                const double low[3] = {static_cast<double>(brick.x * BrickVoxels) - 0.5, static_cast<double>(brick.y * BrickVoxels) - 0.5,
                    static_cast<double>(brick.z * BrickVoxels) - 0.5};
                const double band = static_cast<double>(m_band) * m_band;

                for(const std::uint32_t index : indices)
                {
                    const Candidate& candidate = m_candidates[index];
                    double bound = 0.0;
                    for(std::size_t axis = 0; axis < 3; ++axis)
                    {
                        const double gap = (std::max)({candidate.minimum[axis] - (low[axis] + BrickVoxels + 1.0), 0.0, low[axis] - candidate.maximum[axis]});
                        bound += gap * gap;
                    }
                    // Farther than the band from every voxel centre: cannot change any stored value.
                    if(bound < band)
                    {
                        grid.ranked.push_back({bound, index});
                    }
                }

                std::sort(grid.ranked.begin(), grid.ranked.end());
                return grid;
            }

            // Voxel centres span [base - 0.5, base + 8.5]; the grid adds the band on every side. About 12 cells per axis.
            const double span = ApronBrickVoxels - 1 + 2.0 * m_band;
            grid.cellSize = (std::max)(2.0, std::ceil(span / 12.0));
            grid.cells = static_cast<int>(std::ceil(span / grid.cellSize));
            grid.origin = {static_cast<double>(brick.x * BrickVoxels) - 0.5 - m_band, static_cast<double>(brick.y * BrickVoxels) - 0.5 - m_band,
                static_cast<double>(brick.z * BrickVoxels) - 0.5 - m_band};

            const auto cellRange = [&](const Candidate& candidate, int* first, int* last) {
                for(std::size_t axis = 0; axis < 3; ++axis)
                {
                    const double low = std::floor((candidate.minimum[axis] - grid.origin[axis]) / grid.cellSize);
                    const double high = std::floor((candidate.maximum[axis] - grid.origin[axis]) / grid.cellSize);
                    if(high < 0.0 || low > grid.cells - 1)
                    {
                        return false;
                    }
                    first[axis] = static_cast<int>((std::max)(low, 0.0));
                    last[axis] = static_cast<int>((std::min)(high, static_cast<double>(grid.cells - 1)));
                }
                return true;
            };

            const std::size_t cellCount = static_cast<std::size_t>(grid.cells) * grid.cells * grid.cells;
            grid.cellStart.assign(cellCount + 1, 0);

            for(int pass = 0; pass < 2; ++pass)
            {
                std::vector<std::uint32_t> fill;
                if(pass == 1)
                {
                    for(std::size_t cell = 0; cell < cellCount; ++cell)
                    {
                        grid.cellStart[cell + 1] += grid.cellStart[cell];
                    }
                    grid.entries.resize(grid.cellStart[cellCount]);
                    fill.assign(grid.cellStart.begin(), grid.cellStart.end() - 1);
                }

                for(const std::uint32_t index : indices)
                {
                    int first[3];
                    int last[3];
                    if(!cellRange(m_candidates[index], first, last))
                    {
                        continue;
                    }

                    for(int z = first[2]; z <= last[2]; ++z)
                    {
                        for(int y = first[1]; y <= last[1]; ++y)
                        {
                            for(int x = first[0]; x <= last[0]; ++x)
                            {
                                const std::size_t cell = static_cast<std::size_t>(x) + grid.cells * (static_cast<std::size_t>(y) + grid.cells * static_cast<std::size_t>(z));
                                if(pass == 0)
                                {
                                    ++grid.cellStart[cell + 1];
                                }
                                else
                                {
                                    grid.entries[fill[cell]++] = index;
                                }
                            }
                        }
                    }
                }
            }

            return grid;
        }

        // The exact nearest distance, clamped to the band, quantised. Cells are searched in rings of growing Chebyshev radius
        // around the point's cell; ring r lies at least (r - 1) cells away, so the search stops once that bound reaches the best
        // so far. Any triangle nearer than the best has its nearest point inside the grid, in a cell no farther than that point, so
        // the minimum is exact. Starting from the band squared is exact too: everything at or beyond it quantises to 127.
        std::int8_t QuantisedDistance(const Point& point, const DistanceGrid& grid, bool inside)
        {
            double best = static_cast<double>(m_band) * m_band;

            const auto test = [&](std::uint32_t index) {
                const Candidate& candidate = m_candidates[index];
                double boundsDistance = 0.0;

                for(std::size_t axis = 0; axis < 3; ++axis)
                {
                    const double gap = (std::max)({candidate.minimum[axis] - point[axis], 0.0, point[axis] - candidate.maximum[axis]});
                    boundsDistance += gap * gap;
                }

                if(boundsDistance < best)
                {
                    ++m_result.statistics.distanceTests;
                    best = (std::min)(best, SquaredDistanceToTriangle(point, candidate.corners[0], candidate.corners[1], candidate.corners[2]));
                }
            };

            if(grid.linear)
            {
                // Ranked by a bound valid for every voxel of the brick: once it reaches the best, so do all that follow.
                for(const auto& [bound, index] : grid.ranked)
                {
                    if(bound >= best)
                    {
                        break;
                    }
                    test(index);
                }
            }
            else
            {
            ++m_visitStamp;

            int centre[3];
            for(std::size_t axis = 0; axis < 3; ++axis)
            {
                centre[axis] = std::clamp(static_cast<int>(std::floor((point[axis] - grid.origin[axis]) / grid.cellSize)), 0, grid.cells - 1);
            }

            for(int ring = 0; ring < grid.cells; ++ring)
            {
                const double reach = (ring - 1) * grid.cellSize;
                if(ring > 0 && reach > 0.0 && reach * reach >= best)
                {
                    break;
                }

                for(int z = centre[2] - ring; z <= centre[2] + ring; ++z)
                {
                    for(int y = centre[1] - ring; y <= centre[1] + ring; ++y)
                    {
                        for(int x = centre[0] - ring; x <= centre[0] + ring; ++x)
                        {
                            // Only the shell of this ring; inner cells were searched by earlier rings.
                            if((std::max)({std::abs(x - centre[0]), std::abs(y - centre[1]), std::abs(z - centre[2])}) != ring ||
                                x < 0 || y < 0 || z < 0 || x >= grid.cells || y >= grid.cells || z >= grid.cells)
                            {
                                continue;
                            }

                            const std::size_t cell = static_cast<std::size_t>(x) + grid.cells * (static_cast<std::size_t>(y) + grid.cells * static_cast<std::size_t>(z));

                            for(std::uint32_t at = grid.cellStart[cell]; at < grid.cellStart[cell + 1]; ++at)
                            {
                                const std::uint32_t index = grid.entries[at];

                                if(m_visited[index] == m_visitStamp)
                                {
                                    continue;
                                }

                                m_visited[index] = m_visitStamp;
                                test(index);
                            }
                        }
                    }
                }
            }
            }

            const double distance = std::sqrt(best);
            const auto magnitude = static_cast<int>(std::lround((std::min)(distance / m_band, 1.0) * DistanceQuantisationMaximum));
            return static_cast<std::int8_t>(inside ? -magnitude : magnitude);
        }

        void BuildBricks()
        {
            RegionData& data = m_result.data;
            data.baseExponent = m_settings.baseExponent;
            data.regionBricks = m_settings.regionBricks;
            data.narrowBand = m_settings.narrowBand;
            data.coarseClamp = m_settings.coarseClamp;
            data.settingsHash = HashBakeSettings(m_settings);
            // The region input hash belongs to world partitioning (VX.4); a single-region bake records none.
            data.inputHash = 0;
            data.brickIndex.assign(m_allocated.size(), UniformOutside);

            // Classification changes only across voxels touched by closed triangles, so it is evaluated once for each connected set
            // of the other voxels (open-surface voxels included) and once for each closed-touched voxel.
            std::array<bool, ApronBrickVoxelCount> occupied{};
            std::array<bool, ApronBrickVoxelCount> closedTouched{};
            std::array<bool, ApronBrickVoxelCount> inside{};
            std::array<int, ApronBrickVoxelCount> label{};
            std::vector<int> stack;

            for(int z = 0; z < m_regionBricks; ++z)
            {
                for(int y = 0; y < m_regionBricks; ++y)
                {
                    for(int x = 0; x < m_regionBricks; ++x)
                    {
                        const int cell = CellLinear(x, y, z);

                        if(!m_allocated[cell])
                        {
                            data.brickIndex[cell] = m_componentInside[m_component[cell]] ? UniformInside : UniformOutside;
                            continue;
                        }

                        const Coordinate brick{m_originBrick.x + x, m_originBrick.y + y, m_originBrick.z + z};
                        const Coordinate base{brick.x * BrickVoxels, brick.y * BrickVoxels, brick.z * BrickVoxels};

                        for(int az = -1; az <= BrickVoxels; ++az)
                        {
                            for(int ay = -1; ay <= BrickVoxels; ++ay)
                            {
                                for(int ax = -1; ax <= BrickVoxels; ++ax)
                                {
                                    occupied[ApronIndex(ax, ay, az)] = VoxelOccupied({base.x + ax, base.y + ay, base.z + az});
                                    closedTouched[ApronIndex(ax, ay, az)] = VoxelTouchedByClosed({base.x + ax, base.y + ay, base.z + az});
                                }
                            }
                        }

                        // Classes: each occupied voxel on its own, each connected set of unoccupied voxels once.
                        label.fill(-1);

                        for(int start = 0; start < ApronBrickVoxelCount; ++start)
                        {
                            const int ax = start % ApronBrickVoxels - 1;
                            const int ay = (start / ApronBrickVoxels) % ApronBrickVoxels - 1;
                            const int az = start / (ApronBrickVoxels * ApronBrickVoxels) - 1;
                            const Point centre{static_cast<double>(base.x + ax) + 0.5, static_cast<double>(base.y + ay) + 0.5,
                                static_cast<double>(base.z + az) + 0.5};

                            if(closedTouched[start])
                            {
                                inside[start] = InsideAt(centre);
                                continue;
                            }

                            if(label[start] >= 0)
                            {
                                continue;
                            }

                            const bool componentInside = InsideAt(centre);
                            label[start] = start;
                            inside[start] = componentInside;
                            stack.push_back(start);

                            while(!stack.empty())
                            {
                                const int at = stack.back();
                                stack.pop_back();
                                const int px = at % ApronBrickVoxels;
                                const int py = (at / ApronBrickVoxels) % ApronBrickVoxels;
                                const int pz = at / (ApronBrickVoxels * ApronBrickVoxels);
                                const int neighbours[6][3] = {{px - 1, py, pz}, {px + 1, py, pz}, {px, py - 1, pz}, {px, py + 1, pz}, {px, py, pz - 1}, {px, py, pz + 1}};

                                for(const auto& n : neighbours)
                                {
                                    if(n[0] < 0 || n[1] < 0 || n[2] < 0 || n[0] >= ApronBrickVoxels || n[1] >= ApronBrickVoxels || n[2] >= ApronBrickVoxels)
                                    {
                                        continue;
                                    }

                                    const int next = n[0] + ApronBrickVoxels * (n[1] + ApronBrickVoxels * n[2]);
                                    if(!closedTouched[next] && label[next] < 0)
                                    {
                                        label[next] = start;
                                        inside[next] = componentInside;
                                        stack.push_back(next);
                                    }
                                }
                            }
                        }

                        BrickMask occupancy{};
                        BrickMask solid{};

                        for(int vz = 0; vz < BrickVoxels; ++vz)
                        {
                            for(int vy = 0; vy < BrickVoxels; ++vy)
                            {
                                for(int vx = 0; vx < BrickVoxels; ++vx)
                                {
                                    const int bit = BrickVoxelBit(vx, vy, vz);
                                    const std::uint64_t flag = std::uint64_t{1} << (bit & 63);
                                    if(occupied[ApronIndex(vx, vy, vz)])
                                    {
                                        occupancy[bit >> 6] |= flag;
                                    }
                                    if(inside[ApronIndex(vx, vy, vz)])
                                    {
                                        solid[bit >> 6] |= flag;
                                    }
                                }
                            }
                        }

                        StageTimer distanceTimer(m_result.statistics.distanceSeconds);
                        const DistanceGrid distanceGrid = BuildDistanceGrid(brick);
                        const std::size_t first = data.distance.size();
                        data.distance.resize(first + ApronBrickVoxelCount);

                        for(int index = 0; index < ApronBrickVoxelCount; ++index)
                        {
                            const int ax = index % ApronBrickVoxels - 1;
                            const int ay = (index / ApronBrickVoxels) % ApronBrickVoxels - 1;
                            const int az = index / (ApronBrickVoxels * ApronBrickVoxels) - 1;
                            const Point centre{static_cast<double>(base.x + ax) + 0.5, static_cast<double>(base.y + ay) + 0.5,
                                static_cast<double>(base.z + az) + 0.5};
                            data.distance[first + index] = QuantisedDistance(centre, distanceGrid, inside[index]);
                        }

                        data.brickIndex[cell] = data.GetBrickCount();
                        data.brickCells.push_back(PackBrickCell(x, y, z));
                        data.occupancy.push_back(occupancy);
                        data.solid.push_back(solid);
                    }
                }
            }

            m_result.statistics.allocatedBricks = data.GetBrickCount();
        }

        const GeometrySnapshot& m_snapshot;
        const BakeAcceleration& m_acceleration;
        WindingCounters m_winding;
        bool m_overBudget = false;
        const BakeSettings& m_settings;
        int m_sizeExponent;
        int m_regionBricks;
        int m_band;
        int m_clamp;
        // Bricks around a brick whose bins hold every triangle within the narrow band of its apron voxels.
        int m_distanceRing;
        // Bricks of halo on every side of the region: enough for the coarse clamp and the distance ring.
        int m_margin;
        int m_gridSize;
        Coordinate m_originBrick;
        Coordinate m_gridMinimum;

        std::vector<Candidate> m_candidates;
        std::vector<std::vector<std::uint32_t>> m_bins;
        std::vector<BrickMask> m_masks;
        // Voxels touched by triangles of closed pieces; a subset of m_masks.
        std::vector<BrickMask> m_closedMasks;
        std::vector<std::uint8_t> m_masksDone;
        // Bricks known to have surface from an early-exit test, before (or without) their full masks.
        std::vector<std::uint8_t> m_surfaceFound;
        // Per candidate: the distance query that last tested it (a candidate can sit in several cells).
        std::vector<std::uint32_t> m_visited;
        std::uint32_t m_visitStamp = 0;

        std::vector<std::uint8_t> m_allocated;
        std::size_t m_allocatedCount = 0;
        std::vector<int> m_component;
        std::vector<int> m_componentFirstCell;
        std::vector<bool> m_componentInside;

        std::vector<const SnapshotPiece*> m_closedPieces;
        std::uint64_t m_closedTriangles = 0;

        RegionBakeResult m_result;
    };
}

bool PieceOverlaps(const SnapshotPiece& piece, const WorldBox& box) noexcept
{
    for(std::size_t axis = 0; axis < 3; ++axis)
    {
        if(piece.maximum[axis] < box.minimum[axis] || piece.minimum[axis] > box.maximum[axis])
        {
            return false;
        }
    }
    return true;
}

int RegionHaloBricks(const BakeSettings& settings) noexcept
{
    const int distanceRing = (settings.narrowBand + 1 + BrickVoxels - 1) / BrickVoxels;
    return (std::max)(static_cast<int>(settings.coarseClamp), distanceRing);
}

WorldBox RegionGeometryBounds(const RegionKey& key, const BakeSettings& settings) noexcept
{
    const int log2 = *RegionBricksLog2(settings.regionBricks);
    const int sizeExponent = settings.baseExponent + key.level;
    const int margin = RegionHaloBricks(settings);
    const Coordinate originBrick = VoxelToBrick(RegionOriginVoxel(key, log2));
    const double extent = static_cast<double>((settings.regionBricks + 2 * margin) * BrickVoxels);
    const std::int64_t low[3] = {(originBrick.x - margin) * BrickVoxels, (originBrick.y - margin) * BrickVoxels, (originBrick.z - margin) * BrickVoxels};
    WorldBox box;

    for(std::size_t axis = 0; axis < 3; ++axis)
    {
        box.minimum[axis] = std::ldexp(static_cast<double>(low[axis]) - 1.0, sizeExponent);
        box.maximum[axis] = std::ldexp(static_cast<double>(low[axis]) + extent + 1.0, sizeExponent);
    }

    return box;
}

WorldBox RegionClassificationBounds(const RegionKey& key, const BakeSettings& settings) noexcept
{
    const int log2 = *RegionBricksLog2(settings.regionBricks);
    const int sizeExponent = settings.baseExponent + key.level;
    const Coordinate origin = RegionOriginVoxel(key, log2);
    const double regionVoxels = static_cast<double>(settings.regionBricks * BrickVoxels);
    const double low[3] = {static_cast<double>(origin.x) - 1.0, static_cast<double>(origin.y) - 1.0, static_cast<double>(origin.z) - 1.0};
    WorldBox box;

    for(std::size_t axis = 0; axis < 3; ++axis)
    {
        box.minimum[axis] = std::ldexp(low[axis] - OccupancyPadding, sizeExponent);
        box.maximum[axis] = std::ldexp(low[axis] + regionVoxels + 2.0 + OccupancyPadding, sizeExponent);
    }

    return box;
}

std::uint64_t HashBakeSettings(const BakeSettings& settings) noexcept
{
    const std::byte bytes[] = {std::byte{static_cast<std::uint8_t>(RegionFormat::FormatVersion)}, std::byte{static_cast<std::uint8_t>(settings.baseExponent)},
        std::byte{settings.regionBricks}, std::byte{settings.narrowBand}, std::byte{settings.coarseClamp}};
    return Binary::Fnv1a(bytes);
}

std::expected<RegionBakeResult, std::string> BakeRegion(const GeometrySnapshot& snapshot, const RegionKey& key, const BakeSettings& settings)
{
    const BakeAcceleration acceleration(snapshot);
    return BakeRegion(snapshot, acceleration, key, settings);
}

std::expected<RegionBakeResult, std::string> BakeRegion(const GeometrySnapshot& snapshot, const BakeAcceleration& acceleration, const RegionKey& key,
    const BakeSettings& settings)
{
    const auto log2 = RegionBricksLog2(settings.regionBricks);

    if(!log2 || !IsValidBaseExponent(settings.baseExponent) || settings.narrowBand < RegionFormat::MinimumNarrowBand ||
        settings.narrowBand > RegionFormat::MaximumNarrowBand || settings.coarseClamp < RegionFormat::MinimumCoarseClamp ||
        settings.coarseClamp > RegionFormat::MaximumCoarseClamp)
    {
        return std::unexpected("Voxel bake settings are out of range.");
    }

    if(!IsValidRegionKey(key))
    {
        return std::unexpected("Region key is outside the lattice range.");
    }

    RegionBaker baker(snapshot, acceleration, key, settings, *log2);
    auto result = baker.Run();
    return result;
}
}
