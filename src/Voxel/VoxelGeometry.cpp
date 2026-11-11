#include "Voxel/VoxelGeometry.h"

#include "Voxel/VoxelAcceleration.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <optional>

namespace Voxel
{
namespace
{
    constexpr double Pi = 3.14159265358979323846;

    Point Subtract(const Point& a, const Point& b) noexcept
    {
        return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    }

    double Dot(const Point& a, const Point& b) noexcept
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    Point Cross(const Point& a, const Point& b) noexcept
    {
        return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    }

    double Length(const Point& a) noexcept
    {
        return std::sqrt(Dot(a, a));
    }

    // Exact position identity; -0.0 and 0.0 are one position.
    std::array<std::uint64_t, 3> PositionKey(const Point& position) noexcept
    {
        std::array<std::uint64_t, 3> key{};

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            key[axis] = std::bit_cast<std::uint64_t>(position[axis] == 0.0 ? 0.0 : position[axis]);
        }

        return key;
    }

    std::uint32_t FindRoot(std::vector<std::uint32_t>& parents, std::uint32_t at) noexcept
    {
        while(parents[at] != at)
        {
            parents[at] = parents[parents[at]];
            at = parents[at];
        }

        return at;
    }

    struct EdgeUse
    {
        std::uint32_t uses = 0;
        std::int32_t direction = 0;
    };

    // Projections of the three corners onto `axis` against the cube's projected radius.
    bool SeparatedOnAxis(const Point& axis, const Point& v0, const Point& v1, const Point& v2, double halfExtent) noexcept
    {
        const double p0 = Dot(axis, v0);
        const double p1 = Dot(axis, v1);
        const double p2 = Dot(axis, v2);
        const double radius = halfExtent * (std::abs(axis[0]) + std::abs(axis[1]) + std::abs(axis[2]));
        return (std::min)({p0, p1, p2}) > radius || (std::max)({p0, p1, p2}) < -radius;
    }
}

namespace
{
    // How far a closed piece's exact winding number may stray from an integer before the test point counts as on its surface.
    constexpr double NestingIntegerTolerance = 0.05;
    // Relative distance (to the probe's magnitude) within which a probe counts as touching another closed piece.
    constexpr double ContactTolerance = 1e-9;

    double SignedVolume(const GeometrySnapshot& snapshot, const SnapshotPiece& piece) noexcept
    {
        // Relative to the piece's minimum corner, for precision far from the origin.
        double sum = 0.0;
        for(const SnapshotTriangle& triangle : snapshot.GetTriangles().subspan(piece.firstTriangle, piece.triangleCount))
        {
            const Point a = Subtract(triangle.corners[0], piece.minimum);
            const Point b = Subtract(triangle.corners[1], piece.minimum);
            const Point c = Subtract(triangle.corners[2], piece.minimum);
            sum += Dot(a, Cross(b, c));
        }
        return sum / 6.0;
    }
}

std::expected<GeometrySnapshot, std::string> GeometrySnapshot::Build(std::span<const MeshInput> objects, const SnapshotOptions& options)
{
    auto snapshot = BuildPieces(objects);

    if(!snapshot)
    {
        return snapshot;
    }

    snapshot->ApplyOrientationPolicy(options.orientation, options.analysis);
    return snapshot;
}

// Orientation from signed volume and nesting depth. The depth of a piece is the sum of |rounded winding number| of every other
// closed piece at the centroid of its first triangle: a point on its own surface, inside exactly the pieces enclosing it. Uses the
// authored orientation of every piece (no sign is applied while depths are measured), so the result does not depend on the order
// pieces are visited. A probe touching another closed piece, or where another's exact sum is not near an integer, is Undetermined.
//
// Exact analysis tests every pair of closed pieces with the solid-angle sum: quadratic. Accelerated analysis finds the enclosing
// candidates with a tree over piece bounds, detects contact with the triangle tree, and counts with the ray index, using the exact
// sum (and its integer check) only where a ray is not certain. Both apply the same rules; tests compare them piece by piece.
void GeometrySnapshot::ApplyOrientationPolicy(OrientationPolicy policy, OrientationAnalysis analysis)
{
    std::vector<std::uint32_t> closed;
    for(std::uint32_t index = 0; index < m_pieces.size(); ++index)
    {
        if(m_pieces[index].closed)
        {
            closed.push_back(index);
        }
        else
        {
            m_pieces[index].orientation = PieceOrientation::Open;
        }
    }

    const auto probeOf = [this](const SnapshotPiece& piece) {
        const SnapshotTriangle& first = m_triangles[piece.firstTriangle];
        return Point{(first.corners[0][0] + first.corners[1][0] + first.corners[2][0]) / 3.0,
            (first.corners[0][1] + first.corners[1][1] + first.corners[2][1]) / 3.0,
            (first.corners[0][2] + first.corners[1][2] + first.corners[2][2]) / 3.0};
    };
    const auto contactOf = [](const Point& probe) {
        return ContactTolerance * (1.0 + (std::max)({std::abs(probe[0]), std::abs(probe[1]), std::abs(probe[2])}));
    };
    const auto contains = [](const SnapshotPiece& piece, const Point& p) {
        return p[0] >= piece.minimum[0] && p[0] <= piece.maximum[0] && p[1] >= piece.minimum[1] && p[1] <= piece.maximum[1] &&
            p[2] >= piece.minimum[2] && p[2] <= piece.maximum[2];
    };
    // The exact term for one other piece: its |rounded sum|, or nullopt when the sum is not near an integer.
    const auto exactDepthTerm = [this](const SnapshotPiece& other, const Point& probe) -> std::optional<int> {
        const double winding = PieceWindingNumber(*this, other, probe);
        const double rounded = std::round(winding);
        if(std::abs(winding - rounded) > NestingIntegerTolerance)
        {
            return std::nullopt;
        }
        return static_cast<int>(std::abs(rounded));
    };

    // Depth of each closed piece, or -1 when undetermined.
    std::vector<int> depths(m_pieces.size(), 0);

    if(analysis == OrientationAnalysis::Exact)
    {
        for(const std::uint32_t index : closed)
        {
            const Point probe = probeOf(m_pieces[index]);
            const double contact = contactOf(probe);
            int depth = 0;

            for(const std::uint32_t otherIndex : closed)
            {
                const SnapshotPiece& other = m_pieces[otherIndex];
                if(otherIndex == index || !contains(other, probe))
                {
                    continue;
                }

                double nearest = std::numeric_limits<double>::infinity();
                for(const SnapshotTriangle& triangle : GetTriangles().subspan(other.firstTriangle, other.triangleCount))
                {
                    nearest = (std::min)(nearest, SquaredDistanceToTriangle(probe, triangle.corners[0], triangle.corners[1], triangle.corners[2]));
                }

                const auto term = exactDepthTerm(other, probe);
                if(!term || nearest <= contact * contact)
                {
                    depth = -1;
                    break;
                }
                depth += *term;
            }

            depths[index] = depth;
        }
    }
    else
    {
        std::vector<Point> minimum;
        std::vector<Point> maximum;
        for(const std::uint32_t index : closed)
        {
            minimum.push_back(m_pieces[index].minimum);
            maximum.push_back(m_pieces[index].maximum);
        }

        const BoxTree pieceTree(std::move(minimum), std::move(maximum));
        const TriangleBvh triangleTree(*this);
        const WindingIndex winding(*this);
        WindingCounters counters;
        std::vector<std::uint32_t> enclosing;
        std::vector<std::uint32_t> touching;

        for(const std::uint32_t index : closed)
        {
            const Point probe = probeOf(m_pieces[index]);
            const double contact = contactOf(probe);
            pieceTree.Query(probe, probe, enclosing);
            triangleTree.Query({probe[0] - contact, probe[1] - contact, probe[2] - contact},
                {probe[0] + contact, probe[1] + contact, probe[2] + contact}, touching);
            int depth = 0;

            for(const std::uint32_t slot : enclosing)
            {
                const std::uint32_t otherIndex = closed[slot];
                const SnapshotPiece& other = m_pieces[otherIndex];
                if(otherIndex == index)
                {
                    continue;
                }

                // Contact: only triangles whose bounds reach the probe's tolerance box can lie within it.
                bool inContact = false;
                for(const std::uint32_t triangleIndex : touching)
                {
                    const SnapshotTriangle& triangle = m_triangles[triangleIndex];
                    if(triangle.piece == otherIndex &&
                        SquaredDistanceToTriangle(probe, triangle.corners[0], triangle.corners[1], triangle.corners[2]) <= contact * contact)
                    {
                        inContact = true;
                        break;
                    }
                }

                if(inContact)
                {
                    depth = -1;
                    break;
                }

                // No sign is applied yet (every windingSign is still 1), so the ray gives the authored winding.
                if(const auto ray = winding.RayWinding(otherIndex, probe, counters))
                {
                    depth += static_cast<int>(std::abs(*ray));
                    continue;
                }

                const auto term = exactDepthTerm(other, probe);
                if(!term)
                {
                    depth = -1;
                    break;
                }
                depth += *term;
            }

            depths[index] = depth;
        }
    }

    for(const std::uint32_t index : closed)
    {
        SnapshotPiece& piece = m_pieces[index];
        piece.signedVolume = SignedVolume(*this, piece);
        const int depth = piece.signedVolume != 0.0 ? depths[index] : -1;
        piece.nestingDepth = depth;
        const ObjectOrientation override = m_objectOrientation[piece.object];
        m_report.overriddenPieces += override != ObjectOrientation::Automatic;

        // Orientation is still classified and reported as measured; an override only decides the sign.
        const auto applyOverride = [&] {
            if(override == ObjectOrientation::AsAuthored)
            {
                piece.windingSign = 1;
            }
            else if(override == ObjectOrientation::Outward && piece.signedVolume < 0.0 && piece.windingSign == 1)
            {
                piece.windingSign = -1;
                ++m_report.flippedPieces;
            }
        };

        if(depth < 0)
        {
            piece.orientation = PieceOrientation::Undetermined;
            ++m_report.undeterminedPieces;
        }
        else if(piece.signedVolume > 0.0)
        {
            piece.orientation = depth % 2 == 0 ? PieceOrientation::Outward : PieceOrientation::NestedOutward;
            ++(depth % 2 == 0 ? m_report.outwardPieces : m_report.nestedOutwardPieces);
        }
        else if(depth % 2 == 1)
        {
            piece.orientation = PieceOrientation::Cavity;
            ++m_report.cavityPieces;
        }
        else
        {
            piece.orientation = PieceOrientation::UnenclosedInverted;
            ++m_report.unenclosedInvertedPieces;

            if(policy == OrientationPolicy::FlipUnenclosedInverted && override != ObjectOrientation::AsAuthored)
            {
                piece.windingSign = -1;
                ++m_report.flippedPieces;
            }
        }

        applyOverride();
    }
}

std::expected<GeometrySnapshot, std::string> GeometrySnapshot::BuildPieces(std::span<const MeshInput> objects)
{
    GeometrySnapshot snapshot;

    for(std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
    {
        const MeshInput& mesh = objects[objectIndex];
        snapshot.m_objectOrientation.push_back(mesh.orientation);
        const std::string name = "Object " + std::to_string(objectIndex);

        for(const Point& position : mesh.positions)
        {
            if(!std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2]))
            {
                return std::unexpected(name + " has a non-finite position.");
            }
        }

        // Weld by exact position. Positions that merely look equal are not welded: a seam with a gap stays a gap and opens the piece.
        std::map<std::array<std::uint64_t, 3>, std::uint32_t> welded;
        std::vector<std::uint32_t> weldedIds(mesh.positions.size());
        std::vector<Point> weldedPositions;

        for(std::size_t index = 0; index < mesh.positions.size(); ++index)
        {
            const auto [found, isNew] = welded.try_emplace(PositionKey(mesh.positions[index]), static_cast<std::uint32_t>(welded.size()));
            if(isNew)
            {
                weldedPositions.push_back(mesh.positions[index]);
            }
            weldedIds[index] = found->second;
        }

        std::vector<std::uint32_t> parents(welded.size());
        std::iota(parents.begin(), parents.end(), 0u);
        std::vector<std::array<std::uint32_t, 3>> triangles;
        triangles.reserve(mesh.triangles.size());

        for(const auto& triangle : mesh.triangles)
        {
            if(triangle[0] >= mesh.positions.size() || triangle[1] >= mesh.positions.size() || triangle[2] >= mesh.positions.size())
            {
                return std::unexpected(name + " has a triangle index out of range.");
            }

            const std::array<std::uint32_t, 3> ids{weldedIds[triangle[0]], weldedIds[triangle[1]], weldedIds[triangle[2]]};

            if(ids[0] == ids[1] || ids[1] == ids[2] || ids[2] == ids[0])
            {
                ++snapshot.m_report.collapsedTriangles;
                continue;
            }

            triangles.push_back(ids);
            parents[FindRoot(parents, ids[1])] = FindRoot(parents, ids[0]);
            parents[FindRoot(parents, ids[2])] = FindRoot(parents, ids[0]);
        }

        std::map<std::pair<std::uint32_t, std::uint32_t>, EdgeUse> edges;

        for(const auto& ids : triangles)
        {
            for(std::size_t corner = 0; corner < 3; ++corner)
            {
                const std::uint32_t from = ids[corner];
                const std::uint32_t to = ids[(corner + 1) % 3];
                EdgeUse& edge = edges[{(std::min)(from, to), (std::max)(from, to)}];
                ++edge.uses;
                edge.direction += from < to ? 1 : -1;
            }
        }

        std::map<std::uint32_t, bool> rootClosed;

        for(const auto& [edge, use] : edges)
        {
            const std::uint32_t root = FindRoot(parents, edge.first);
            auto [entry, isNew] = rootClosed.try_emplace(root, true);
            if(use.uses != 2 || use.direction != 0)
            {
                entry->second = false;
            }
        }

        // Pieces in order of first appearance; each piece's triangles contiguous, in input order.
        std::map<std::uint32_t, std::uint32_t> pieceOfRoot;
        std::vector<std::vector<std::uint32_t>> pieceTriangles;

        for(std::uint32_t index = 0; index < triangles.size(); ++index)
        {
            const auto [found, isNew] = pieceOfRoot.try_emplace(FindRoot(parents, triangles[index][0]),
                static_cast<std::uint32_t>(pieceTriangles.size()));
            if(isNew)
            {
                pieceTriangles.emplace_back();
            }
            pieceTriangles[found->second].push_back(index);
        }

        std::vector<std::uint32_t> rootOfPiece(pieceTriangles.size());
        for(const auto& [root, localPiece] : pieceOfRoot)
        {
            rootOfPiece[localPiece] = root;
        }

        for(std::size_t localPiece = 0; localPiece < pieceTriangles.size(); ++localPiece)
        {
            SnapshotPiece piece;
            piece.object = static_cast<std::uint32_t>(objectIndex);
            piece.closed = rootClosed.at(rootOfPiece[localPiece]);
            piece.firstTriangle = static_cast<std::uint32_t>(snapshot.m_triangles.size());
            piece.minimum.fill((std::numeric_limits<double>::max)());
            piece.maximum.fill(std::numeric_limits<double>::lowest());
            const auto pieceIndex = static_cast<std::uint32_t>(snapshot.m_pieces.size());

            for(const std::uint32_t triangleIndex : pieceTriangles[localPiece])
            {
                const auto& ids = triangles[triangleIndex];
                SnapshotTriangle triangle{{weldedPositions[ids[0]], weldedPositions[ids[1]], weldedPositions[ids[2]]}, pieceIndex};
                const Point normal = Cross(Subtract(triangle.corners[1], triangle.corners[0]), Subtract(triangle.corners[2], triangle.corners[0]));

                if(normal[0] == 0.0 && normal[1] == 0.0 && normal[2] == 0.0)
                {
                    ++snapshot.m_report.zeroAreaTriangles;
                    continue;
                }

                for(const Point& corner : triangle.corners)
                {
                    for(std::size_t axis = 0; axis < 3; ++axis)
                    {
                        piece.minimum[axis] = (std::min)(piece.minimum[axis], corner[axis]);
                        piece.maximum[axis] = (std::max)(piece.maximum[axis], corner[axis]);
                    }
                }

                snapshot.m_triangles.push_back(triangle);
            }

            piece.triangleCount = static_cast<std::uint32_t>(snapshot.m_triangles.size() - piece.firstTriangle);

            // A piece of only zero-area triangles has no surface and encloses nothing.
            if(piece.triangleCount == 0)
            {
                continue;
            }

            ++(piece.closed ? snapshot.m_report.closedPieces : snapshot.m_report.openPieces);
            snapshot.m_pieces.push_back(piece);
        }
    }

    return snapshot;
}

bool TriangleTouchesCube(const Point& centre, double halfExtent, const Point& a, const Point& b, const Point& c) noexcept
{
    const Point v0 = Subtract(a, centre);
    const Point v1 = Subtract(b, centre);
    const Point v2 = Subtract(c, centre);

    // The cube's face normals: the triangle's bounds against the cube.
    for(std::size_t axis = 0; axis < 3; ++axis)
    {
        if((std::min)({v0[axis], v1[axis], v2[axis]}) > halfExtent || (std::max)({v0[axis], v1[axis], v2[axis]}) < -halfExtent)
        {
            return false;
        }
    }

    const std::array<Point, 3> edges{Subtract(v1, v0), Subtract(v2, v1), Subtract(v0, v2)};
    constexpr std::array<Point, 3> cubeAxes{{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}}};

    for(const Point& edge : edges)
    {
        for(const Point& cubeAxis : cubeAxes)
        {
            if(SeparatedOnAxis(Cross(cubeAxis, edge), v0, v1, v2, halfExtent))
            {
                return false;
            }
        }
    }

    // The triangle's plane against the cube.
    const Point normal = Cross(edges[0], edges[1]);
    const double radius = halfExtent * (std::abs(normal[0]) + std::abs(normal[1]) + std::abs(normal[2]));
    return std::abs(Dot(normal, v0)) <= radius;
}

double SquaredDistanceToTriangle(const Point& point, const Point& a, const Point& b, const Point& c) noexcept
{
    const Point ab = Subtract(b, a);
    const Point ac = Subtract(c, a);
    const Point ap = Subtract(point, a);
    const double d1 = Dot(ab, ap);
    const double d2 = Dot(ac, ap);
    const Point bp = Subtract(point, b);
    const double d3 = Dot(ab, bp);
    const double d4 = Dot(ac, bp);
    const Point cp = Subtract(point, c);
    const double d5 = Dot(ab, cp);
    const double d6 = Dot(ac, cp);
    const double vc = d1 * d4 - d3 * d2;
    const double vb = d5 * d2 - d1 * d6;
    const double va = d3 * d6 - d5 * d4;
    double v = 0.0;
    double w = 0.0;

    if(d1 <= 0.0 && d2 <= 0.0)
    {
        // Closest to a.
    }
    else if(d3 >= 0.0 && d4 <= d3)
    {
        v = 1.0;
    }
    else if(d6 >= 0.0 && d5 <= d6)
    {
        w = 1.0;
    }
    else if(vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
    {
        v = d1 / (d1 - d3);
    }
    else if(vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
    {
        w = d2 / (d2 - d6);
    }
    else if(va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
    {
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        v = 1.0 - w;
    }
    else
    {
        const double denominator = 1.0 / (va + vb + vc);
        v = vb * denominator;
        w = vc * denominator;
    }

    const Point offset{ap[0] - ab[0] * v - ac[0] * w, ap[1] - ab[1] * v - ac[1] * w, ap[2] - ab[2] * v - ac[2] * w};
    return Dot(offset, offset);
}

double PieceWindingNumber(const GeometrySnapshot& snapshot, const SnapshotPiece& piece, const Point& point) noexcept
{
    double sum = 0.0;
    const auto triangles = snapshot.GetTriangles().subspan(piece.firstTriangle, piece.triangleCount);

    for(const SnapshotTriangle& triangle : triangles)
    {
        const Point a = Subtract(triangle.corners[0], point);
        const Point b = Subtract(triangle.corners[1], point);
        const Point c = Subtract(triangle.corners[2], point);
        const double lengthA = Length(a);
        const double lengthB = Length(b);
        const double lengthC = Length(c);
        const double numerator = Dot(a, Cross(b, c));
        const double denominator = lengthA * lengthB * lengthC + Dot(a, b) * lengthC + Dot(b, c) * lengthA + Dot(c, a) * lengthB;
        sum += 2.0 * std::atan2(numerator, denominator);
    }

    return sum / (4.0 * Pi);
}

long long ExactPieceWinding(const GeometrySnapshot& snapshot, const SnapshotPiece& piece, const Point& point) noexcept
{
    return std::llround(PieceWindingNumber(snapshot, piece, point)) * piece.windingSign;
}
}
