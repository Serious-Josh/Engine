#pragma once

// VX.3b: acceleration structures built once per geometry snapshot and shared by every region bake. See
// Documentation/Voxel/VoxelArchitecture.md, section 5.4 and "VX.3b acceleration".
//
// TriangleBvh answers "which triangles have bounds overlapping this box", exactly as a full scan of the snapshot would (inclusive
// bounds, results in ascending snapshot order), so replacing the scan changes no bake output.
//
// WindingIndex gives a closed piece's integer winding number by counting signed crossings of one fixed ray. For a closed piece
// that count is the winding number exactly; it is also what per-piece rounding of the solid-angle sum produces wherever that sum
// is well conditioned. A crossing is decided only when every orientation and the hit distance clear a relative tolerance; if any
// candidate triangle is too close to call (the ray grazes an edge or vertex, or the point lies on the surface), the whole piece
// falls back to the exact solid-angle sum. Each result depends only on the point, the piece and the fixed ray, never on the
// region asking, so classification stays region-independent and deterministic.

#include "Voxel/VoxelGeometry.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Voxel
{
// A median-split bounding-volume tree over axis-aligned boxes, built deterministically (ties broken by index). Immutable once built;
// Query is const and keeps its traversal state on the stack, so concurrent queries are safe.
class BoxTree
{
public:
    BoxTree() = default;
    BoxTree(std::vector<Point> minimum, std::vector<Point> maximum);

    // Replaces `result` with the boxes overlapping [minimum, maximum] (inclusive bounds), in ascending index order.
    void Query(const Point& minimum, const Point& maximum, std::vector<std::uint32_t>& result) const;

    std::size_t GetMemoryBytes() const noexcept;

private:
    struct Node
    {
        Point minimum;
        Point maximum;
        // Leaf: triangles m_order[first, first + count). Interior (count 0): children first and first + 1.
        std::uint32_t first = 0;
        std::uint32_t count = 0;
    };

    std::vector<Node> m_nodes;
    std::vector<std::uint32_t> m_order;
    std::vector<Point> m_minimum;
    std::vector<Point> m_maximum;
};

// The snapshot's triangles by their bounds; indices are snapshot triangle indices.
class TriangleBvh : public BoxTree
{
public:
    explicit TriangleBvh(const GeometrySnapshot& snapshot);
};

struct WindingCounters
{
    std::uint64_t rayQueries = 0;
    std::uint64_t triangleTests = 0;
    // Pieces evaluated by the exact solid-angle sum because a ray result was not certain, and the triangles that sum visited.
    std::uint64_t exactFallbacks = 0;
    std::uint64_t exactTriangles = 0;
};

class WindingIndex
{
public:
    explicit WindingIndex(const GeometrySnapshot& snapshot);

    // The piece's classification term at `point` (world space): its integer winding number times its windingSign. Same contract
    // as ExactPieceWinding. The piece must be closed.
    long long PieceWinding(std::uint32_t pieceIndex, const Point& point, WindingCounters& counters) const;

    // The ray count alone, times windingSign; nullopt when the ray is not certain (PieceWinding then uses the exact sum).
    std::optional<long long> RayWinding(std::uint32_t pieceIndex, const Point& point, WindingCounters& counters) const;

    std::size_t GetMemoryBytes() const noexcept;

private:
    // A piece's triangles in sheared coordinates (x, y - ShearY x, z - ShearZ x), where the ray is +x.
    struct Grid
    {
        double minimumY = 0.0;
        double minimumZ = 0.0;
        double maximumY = 0.0;
        double maximumZ = 0.0;
        double scaleY = 0.0;
        double scaleZ = 0.0;
        int cellsY = 1;
        int cellsZ = 1;
        // Cell c holds entries[cellStart[c], cellStart[c + 1]): indices into m_sheared, ascending.
        std::vector<std::uint32_t> cellStart;
        std::vector<std::uint32_t> entries;
    };

    struct ShearedTriangle
    {
        std::array<Point, 3> corners;
        double maximumX = 0.0;
    };

    const GeometrySnapshot& m_snapshot;
    std::vector<Grid> m_grids;
    std::vector<ShearedTriangle> m_sheared;
};

// Everything shared by the region bakes of one snapshot. Immutable once built; safe to share between threads.
class BakeAcceleration
{
public:
    explicit BakeAcceleration(const GeometrySnapshot& snapshot) :
        triangles(snapshot),
        winding(snapshot)
    {
    }

    TriangleBvh triangles;
    WindingIndex winding;
};
}
