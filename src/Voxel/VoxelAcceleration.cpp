#include "Voxel/VoxelAcceleration.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Voxel
{
namespace
{
    constexpr std::uint32_t LeafTriangles = 8;

    // The ray direction is (1, ShearY, ShearZ): deliberately not aligned with the lattice or with axis-aligned geometry, so rays
    // from voxel centres rarely graze edges.
    constexpr double ShearY = 0.0137194826;
    constexpr double ShearZ = 0.0091733514;

    // Relative tolerances below which an orientation or a hit distance is too close to call.
    constexpr double OrientationTolerance = 1e-12;
    constexpr double HitTolerance = 1e-10;

    bool Overlaps(const Point& minimumA, const Point& maximumA, const Point& minimumB, const Point& maximumB) noexcept
    {
        return maximumA[0] >= minimumB[0] && minimumA[0] <= maximumB[0] && maximumA[1] >= minimumB[1] && minimumA[1] <= maximumB[1] &&
            maximumA[2] >= minimumB[2] && minimumA[2] <= maximumB[2];
    }

    int CellOf(double value, double minimum, double scale, int cells) noexcept
    {
        const double position = std::floor((value - minimum) * scale);
        return static_cast<int>(std::clamp(position, 0.0, static_cast<double>(cells - 1)));
    }
}

// ---------------------------------------------------------------------------------------------------------------------------

namespace
{
    std::vector<Point> TriangleBounds(const GeometrySnapshot& snapshot, bool maximum)
    {
        std::vector<Point> bounds;
        bounds.reserve(snapshot.GetTriangles().size());
        for(const SnapshotTriangle& triangle : snapshot.GetTriangles())
        {
            const auto& c = triangle.corners;
            Point bound;
            for(std::size_t axis = 0; axis < 3; ++axis)
            {
                bound[axis] = maximum ? (std::max)({c[0][axis], c[1][axis], c[2][axis]}) : (std::min)({c[0][axis], c[1][axis], c[2][axis]});
            }
            bounds.push_back(bound);
        }
        return bounds;
    }
}

TriangleBvh::TriangleBvh(const GeometrySnapshot& snapshot) :
    BoxTree(TriangleBounds(snapshot, false), TriangleBounds(snapshot, true))
{
}

BoxTree::BoxTree(std::vector<Point> minimum, std::vector<Point> maximum) :
    m_minimum(std::move(minimum)),
    m_maximum(std::move(maximum))
{
    const auto count = static_cast<std::uint32_t>(m_minimum.size());
    m_order.resize(count);
    std::vector<Point> centres(count);

    for(std::uint32_t index = 0; index < count; ++index)
    {
        m_order[index] = index;
        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            centres[index][axis] = 0.5 * (m_minimum[index][axis] + m_maximum[index][axis]);
        }
    }

    if(count == 0)
    {
        return;
    }

    struct Pending
    {
        std::uint32_t node;
        std::uint32_t first;
        std::uint32_t count;
    };

    m_nodes.push_back({});
    std::vector<Pending> stack{{0, 0, count}};

    while(!stack.empty())
    {
        const Pending pending = stack.back();
        stack.pop_back();

        Point nodeMinimum;
        Point nodeMaximum;
        Point centreMinimum;
        Point centreMaximum;
        nodeMinimum.fill(std::numeric_limits<double>::infinity());
        nodeMaximum.fill(-std::numeric_limits<double>::infinity());
        centreMinimum = nodeMinimum;
        centreMaximum = nodeMaximum;

        for(std::uint32_t at = pending.first; at < pending.first + pending.count; ++at)
        {
            const std::uint32_t box = m_order[at];
            for(std::size_t axis = 0; axis < 3; ++axis)
            {
                nodeMinimum[axis] = (std::min)(nodeMinimum[axis], m_minimum[box][axis]);
                nodeMaximum[axis] = (std::max)(nodeMaximum[axis], m_maximum[box][axis]);
                centreMinimum[axis] = (std::min)(centreMinimum[axis], centres[box][axis]);
                centreMaximum[axis] = (std::max)(centreMaximum[axis], centres[box][axis]);
            }
        }

        m_nodes[pending.node].minimum = nodeMinimum;
        m_nodes[pending.node].maximum = nodeMaximum;

        if(pending.count <= LeafTriangles)
        {
            m_nodes[pending.node].first = pending.first;
            m_nodes[pending.node].count = pending.count;
            continue;
        }

        // Median split on the longest centroid axis; ties broken by index, so the tree is the same on every run.
        std::size_t axis = 0;
        for(std::size_t candidate = 1; candidate < 3; ++candidate)
        {
            if(centreMaximum[candidate] - centreMinimum[candidate] > centreMaximum[axis] - centreMinimum[axis])
            {
                axis = candidate;
            }
        }

        std::sort(m_order.begin() + pending.first, m_order.begin() + pending.first + pending.count,
            [&](std::uint32_t a, std::uint32_t b) { return centres[a][axis] < centres[b][axis] || (centres[a][axis] == centres[b][axis] && a < b); });

        const auto left = static_cast<std::uint32_t>(m_nodes.size());
        m_nodes.push_back({});
        m_nodes.push_back({});
        m_nodes[pending.node].first = left;
        m_nodes[pending.node].count = 0;
        const std::uint32_t half = pending.count / 2;
        stack.push_back({left, pending.first, half});
        stack.push_back({left + 1, pending.first + half, pending.count - half});
    }
}

void BoxTree::Query(const Point& minimum, const Point& maximum, std::vector<std::uint32_t>& result) const
{
    result.clear();

    if(m_nodes.empty())
    {
        return;
    }

    std::vector<std::uint32_t> stack{0};

    while(!stack.empty())
    {
        const Node& node = m_nodes[stack.back()];
        stack.pop_back();

        if(!Overlaps(node.minimum, node.maximum, minimum, maximum))
        {
            continue;
        }

        if(node.count == 0)
        {
            stack.push_back(node.first);
            stack.push_back(node.first + 1);
            continue;
        }

        for(std::uint32_t at = node.first; at < node.first + node.count; ++at)
        {
            const std::uint32_t triangle = m_order[at];
            if(Overlaps(m_minimum[triangle], m_maximum[triangle], minimum, maximum))
            {
                result.push_back(triangle);
            }
        }
    }

    std::sort(result.begin(), result.end());
}

std::size_t BoxTree::GetMemoryBytes() const noexcept
{
    return m_nodes.capacity() * sizeof(Node) + m_order.capacity() * sizeof(std::uint32_t) + (m_minimum.capacity() + m_maximum.capacity()) * sizeof(Point);
}

// ---------------------------------------------------------------------------------------------------------------------------

WindingIndex::WindingIndex(const GeometrySnapshot& snapshot) :
    m_snapshot(snapshot)
{
    const auto triangles = snapshot.GetTriangles();
    const auto pieces = snapshot.GetPieces();
    m_sheared.resize(triangles.size());
    m_grids.resize(pieces.size());

    for(std::size_t pieceIndex = 0; pieceIndex < pieces.size(); ++pieceIndex)
    {
        const SnapshotPiece& piece = pieces[pieceIndex];

        if(!piece.closed)
        {
            continue;
        }

        Grid& grid = m_grids[pieceIndex];
        grid.minimumY = grid.minimumZ = std::numeric_limits<double>::infinity();
        grid.maximumY = grid.maximumZ = -std::numeric_limits<double>::infinity();

        for(std::uint32_t index = piece.firstTriangle; index < piece.firstTriangle + piece.triangleCount; ++index)
        {
            ShearedTriangle& sheared = m_sheared[index];
            sheared.maximumX = -std::numeric_limits<double>::infinity();

            for(std::size_t corner = 0; corner < 3; ++corner)
            {
                const Point& p = triangles[index].corners[corner];
                sheared.corners[corner] = {p[0], p[1] - ShearY * p[0], p[2] - ShearZ * p[0]};
                sheared.maximumX = (std::max)(sheared.maximumX, p[0]);
                grid.minimumY = (std::min)(grid.minimumY, sheared.corners[corner][1]);
                grid.maximumY = (std::max)(grid.maximumY, sheared.corners[corner][1]);
                grid.minimumZ = (std::min)(grid.minimumZ, sheared.corners[corner][2]);
                grid.maximumZ = (std::max)(grid.maximumZ, sheared.corners[corner][2]);
            }
        }

        // About sqrt(n) cells per axis, halved until no more than 16 entries per triangle (large triangles span many cells).
        int cells = std::clamp(static_cast<int>(std::ceil(std::sqrt(static_cast<double>(piece.triangleCount)))), 1, 1024);

        const auto cellRange = [&](const ShearedTriangle& triangle, int& y0, int& y1, int& z0, int& z1) {
            const double lowY = (std::min)({triangle.corners[0][1], triangle.corners[1][1], triangle.corners[2][1]});
            const double highY = (std::max)({triangle.corners[0][1], triangle.corners[1][1], triangle.corners[2][1]});
            const double lowZ = (std::min)({triangle.corners[0][2], triangle.corners[1][2], triangle.corners[2][2]});
            const double highZ = (std::max)({triangle.corners[0][2], triangle.corners[1][2], triangle.corners[2][2]});
            y0 = CellOf(lowY, grid.minimumY, grid.scaleY, grid.cellsY);
            y1 = CellOf(highY, grid.minimumY, grid.scaleY, grid.cellsY);
            z0 = CellOf(lowZ, grid.minimumZ, grid.scaleZ, grid.cellsZ);
            z1 = CellOf(highZ, grid.minimumZ, grid.scaleZ, grid.cellsZ);
        };

        while(true)
        {
            const double extentY = grid.maximumY - grid.minimumY;
            const double extentZ = grid.maximumZ - grid.minimumZ;
            grid.cellsY = extentY > 0.0 ? cells : 1;
            grid.cellsZ = extentZ > 0.0 ? cells : 1;
            grid.scaleY = extentY > 0.0 ? grid.cellsY / extentY : 0.0;
            grid.scaleZ = extentZ > 0.0 ? grid.cellsZ / extentZ : 0.0;

            std::uint64_t entries = 0;
            for(std::uint32_t index = piece.firstTriangle; index < piece.firstTriangle + piece.triangleCount; ++index)
            {
                int y0, y1, z0, z1;
                cellRange(m_sheared[index], y0, y1, z0, z1);
                entries += static_cast<std::uint64_t>(y1 - y0 + 1) * static_cast<std::uint64_t>(z1 - z0 + 1);
            }

            if(cells == 1 || entries <= 16ull * piece.triangleCount + 4096)
            {
                break;
            }

            cells = (std::max)(1, cells / 2);
        }

        // Two passes: count per cell, then fill in triangle order, so each cell lists its triangles ascending.
        const std::size_t cellCount = static_cast<std::size_t>(grid.cellsY) * grid.cellsZ;
        grid.cellStart.assign(cellCount + 1, 0);

        for(std::uint32_t index = piece.firstTriangle; index < piece.firstTriangle + piece.triangleCount; ++index)
        {
            int y0, y1, z0, z1;
            cellRange(m_sheared[index], y0, y1, z0, z1);
            for(int z = z0; z <= z1; ++z)
            {
                for(int y = y0; y <= y1; ++y)
                {
                    ++grid.cellStart[static_cast<std::size_t>(y) + static_cast<std::size_t>(grid.cellsY) * z + 1];
                }
            }
        }

        for(std::size_t cell = 0; cell < cellCount; ++cell)
        {
            grid.cellStart[cell + 1] += grid.cellStart[cell];
        }

        grid.entries.resize(grid.cellStart[cellCount]);
        std::vector<std::uint32_t> fill(grid.cellStart.begin(), grid.cellStart.end() - 1);

        for(std::uint32_t index = piece.firstTriangle; index < piece.firstTriangle + piece.triangleCount; ++index)
        {
            int y0, y1, z0, z1;
            cellRange(m_sheared[index], y0, y1, z0, z1);
            for(int z = z0; z <= z1; ++z)
            {
                for(int y = y0; y <= y1; ++y)
                {
                    grid.entries[fill[static_cast<std::size_t>(y) + static_cast<std::size_t>(grid.cellsY) * z]++] = index;
                }
            }
        }
    }
}

long long WindingIndex::PieceWinding(std::uint32_t pieceIndex, const Point& point, WindingCounters& counters) const
{
    if(const auto ray = RayWinding(pieceIndex, point, counters))
    {
        return *ray;
    }

    const SnapshotPiece& piece = m_snapshot.GetPieces()[pieceIndex];
    ++counters.exactFallbacks;
    counters.exactTriangles += piece.triangleCount;
    return ExactPieceWinding(m_snapshot, piece, point);
}

std::optional<long long> WindingIndex::RayWinding(std::uint32_t pieceIndex, const Point& point, WindingCounters& counters) const
{
    const SnapshotPiece& piece = m_snapshot.GetPieces()[pieceIndex];
    const Grid& grid = m_grids[pieceIndex];
    ++counters.rayQueries;

    const double px = point[0];
    const double py = point[1] - ShearY * point[0];
    const double pz = point[2] - ShearZ * point[0];

    // Outside every triangle's projection: the ray meets nothing, and the point is on no surface.
    if(py < grid.minimumY || py > grid.maximumY || pz < grid.minimumZ || pz > grid.maximumZ)
    {
        return 0;
    }

    const std::size_t cell = static_cast<std::size_t>(CellOf(py, grid.minimumY, grid.scaleY, grid.cellsY)) +
        static_cast<std::size_t>(grid.cellsY) * static_cast<std::size_t>(CellOf(pz, grid.minimumZ, grid.scaleZ, grid.cellsZ));
    long long crossings = 0;
    bool certain = true;

    for(std::uint32_t at = grid.cellStart[cell]; at < grid.cellStart[cell + 1] && certain; ++at)
    {
        const ShearedTriangle& triangle = m_sheared[grid.entries[at]];
        ++counters.triangleTests;

        // Entirely behind the point: the ray cannot reach it, and the point is not on it.
        if(triangle.maximumX < px)
        {
            continue;
        }

        const double ay = triangle.corners[0][1] - py;
        const double az = triangle.corners[0][2] - pz;
        const double by = triangle.corners[1][1] - py;
        const double bz = triangle.corners[1][2] - pz;
        const double cy = triangle.corners[2][1] - py;
        const double cz = triangle.corners[2][2] - pz;

        // The point against each projected edge (ab, bc, ca); positive means to its left.
        const double edges[3][2] = {{ay * bz, az * by}, {by * cz, bz * cy}, {cy * az, cz * ay}};
        double orientation[3];
        int positive = 0;
        int negative = 0;
        int unclear = 0;

        for(int edge = 0; edge < 3; ++edge)
        {
            orientation[edge] = edges[edge][0] - edges[edge][1];
            const double tolerance = OrientationTolerance * (std::abs(edges[edge][0]) + std::abs(edges[edge][1]));

            if(orientation[edge] > tolerance)
            {
                ++positive;
            }
            else if(orientation[edge] < -tolerance)
            {
                ++negative;
            }
            else
            {
                ++unclear;
            }
        }

        // Clearly on opposite sides of two edges: the ray misses this triangle.
        if(positive > 0 && negative > 0)
        {
            continue;
        }

        if(unclear > 0)
        {
            certain = false;
            break;
        }

        // Inside the projection. Barycentric weights: a <- edge bc, b <- edge ca, c <- edge ab.
        const double area = orientation[0] + orientation[1] + orientation[2];
        const double hitX = (orientation[1] * triangle.corners[0][0] + orientation[2] * triangle.corners[1][0] + orientation[0] * triangle.corners[2][0]) / area;
        const double distance = hitX - px;
        const double tolerance = HitTolerance * (std::abs(triangle.corners[0][0] - px) + std::abs(triangle.corners[1][0] - px) + std::abs(triangle.corners[2][0] - px));

        if(std::abs(distance) <= tolerance)
        {
            certain = false;
            break;
        }

        // A hit ahead of the point counts +1 when the triangle faces along the ray (leaving a solid), -1 when it faces back.
        if(distance > 0.0)
        {
            crossings += area > 0.0 ? 1 : -1;
        }
    }

    if(!certain)
    {
        return std::nullopt;
    }

    return crossings * piece.windingSign;
}

std::size_t WindingIndex::GetMemoryBytes() const noexcept
{
    std::size_t bytes = m_sheared.capacity() * sizeof(ShearedTriangle) + m_grids.capacity() * sizeof(Grid);
    for(const Grid& grid : m_grids)
    {
        bytes += (grid.cellStart.capacity() + grid.entries.capacity()) * sizeof(std::uint32_t);
    }
    return bytes;
}
}
