#include "Audio/ProbeSolidClassifier.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>

namespace Audio::Internal
{
namespace
{
    constexpr double Pi = 3.14159265358979323846;
    // A closed surface gives an integer to well within this, unless the point lies on the surface itself.
    constexpr double IntegerTolerance = 0.01;
    // A flat open surface never reaches a half. Beyond it, the piece wraps the point more than an open plane can.
    constexpr double WrappedByOpenPiece = 0.5;
    // Surface hits carry float rounding, so a probe placed at height zero can land a hair under its floor. Metres.
    constexpr double SurfaceTolerance = 0.001;

    using Point = std::array<double, 3>;

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

    // Ericson, Real-Time Collision Detection, 5.1.5.
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
            // Closest to a: v and w stay zero.
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

    // Vertices sharing an exact position are one vertex, as importers split them at hard edges. Positions that merely look equal are
    // not welded: a seam with a gap is a gap, and makes the piece open.
    std::array<std::uint32_t, 3> PositionKey(const std::array<float, 3>& position) noexcept
    {
        std::array<std::uint32_t, 3> key{};

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            key[axis] = std::bit_cast<std::uint32_t>(position[axis] == 0.0f ? 0.0f : position[axis]);
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
}

ProbeSolidClassifier::ProbeSolidClassifier(const std::vector<ProbeGeometryObject>& geometry)
{
    for(const ProbeGeometryObject& object : geometry)
    {
        AddObject(object);
    }
}

void ProbeSolidClassifier::AddObject(const ProbeGeometryObject& object)
{
    if(!object.mesh)
    {
        return;
    }

    const AcousticMeshData& mesh = *object.mesh;
    std::map<std::array<std::uint32_t, 3>, std::uint32_t> welded;
    std::vector<std::uint32_t> weldedIDs(mesh.positions.size());

    for(std::size_t index = 0; index < mesh.positions.size(); ++index)
    {
        weldedIDs[index] = welded.try_emplace(PositionKey(mesh.positions[index]), static_cast<std::uint32_t>(welded.size())).first->second;
    }

    std::vector<std::uint32_t> parents(welded.size());
    std::iota(parents.begin(), parents.end(), 0u);
    std::vector<std::array<std::uint32_t, 3>> triangles;
    triangles.reserve(mesh.triangles.size());

    for(const auto& triangle : mesh.triangles)
    {
        const std::array<std::uint32_t, 3> ids{weldedIDs[triangle[0]], weldedIDs[triangle[1]], weldedIDs[triangle[2]]};

        // Collapsed by welding: encloses nothing and joins no edge.
        if(ids[0] == ids[1] || ids[1] == ids[2] || ids[2] == ids[0])
        {
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

    // In first-appearance order, for determinism.
    std::map<std::uint32_t, std::size_t> pieceOfRoot;
    const std::size_t firstPiece = m_pieces.size();
    std::vector<std::array<double, 3>> worldPositions(welded.size());

    for(std::size_t index = 0; index < mesh.positions.size(); ++index)
    {
        const auto& position = mesh.positions[index];

        for(std::size_t row = 0; row < 3; ++row)
        {
            const auto& matrix = object.world[row];
            worldPositions[weldedIDs[index]][row] = double(matrix[0]) * position[0] + double(matrix[1]) * position[1] + double(matrix[2]) * position[2] + double(matrix[3]);
        }
    }

    // A mirroring transform reverses winding. The author's solid is still a solid.
    const auto& w = object.world;
    const double determinant = double(w[0][0]) * (double(w[1][1]) * w[2][2] - double(w[1][2]) * w[2][1]) -
        double(w[0][1]) * (double(w[1][0]) * w[2][2] - double(w[1][2]) * w[2][0]) + double(w[0][2]) * (double(w[1][0]) * w[2][1] - double(w[1][1]) * w[2][0]);
    const bool mirrored = determinant < 0.0;

    for(const auto& ids : triangles)
    {
        const std::uint32_t root = FindRoot(parents, ids[0]);
        const auto [found, isNew] = pieceOfRoot.try_emplace(root, m_pieces.size());

        if(isNew)
        {
            Piece piece;
            piece.closed = true;
            piece.minimum.fill((std::numeric_limits<double>::max)());
            piece.maximum.fill(std::numeric_limits<double>::lowest());
            m_pieces.push_back(std::move(piece));
        }

        Piece& piece = m_pieces[found->second];
        Triangle triangle{{worldPositions[ids[0]], worldPositions[mirrored ? ids[2] : ids[1]], worldPositions[mirrored ? ids[1] : ids[2]]}};

        for(const Point& corner : triangle.corners)
        {
            for(std::size_t axis = 0; axis < 3; ++axis)
            {
                piece.minimum[axis] = (std::min)(piece.minimum[axis], corner[axis]);
                piece.maximum[axis] = (std::max)(piece.maximum[axis], corner[axis]);
            }
        }

        piece.triangles.push_back(triangle);
    }

    for(const auto& [edge, use] : edges)
    {
        if(use.uses != 2 || use.direction != 0)
        {
            m_pieces[pieceOfRoot.at(FindRoot(parents, edge.first))].closed = false;
        }
    }

    for(std::size_t index = firstPiece; index < m_pieces.size(); ++index)
    {
        ++(m_pieces[index].closed ? m_closedPieces : m_openPieces);
    }
}

// Van Oosterom and Strackee's solid angle of each triangle, summed, over the whole sphere's 4 pi.
double ProbeSolidClassifier::WindingNumber(const Piece& piece, const Point& point) noexcept
{
    double sum = 0.0;

    for(const Triangle& triangle : piece.triangles)
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

ProbeSolidClassifier::Verdict ProbeSolidClassifier::Classify(const std::array<float, 3>& point) const
{
    const Point at{point[0], point[1], point[2]};
    double closed = 0.0;
    double open = 0.0;
    std::vector<const Piece*> closedHere;

    for(const Piece& piece : m_pieces)
    {
        // A closed surface winds zero times around anything outside its bounds, and a flat open one wraps nothing.
        const bool within = at[0] >= piece.minimum[0] && at[0] <= piece.maximum[0] && at[1] >= piece.minimum[1] && at[1] <= piece.maximum[1] &&
            at[2] >= piece.minimum[2] && at[2] <= piece.maximum[2];

        if(within)
        {
            (piece.closed ? closed : open) += WindingNumber(piece, at);

            if(piece.closed)
            {
                closedHere.push_back(&piece);
            }
        }
    }

    const double rounded = std::round(closed);

    if(std::abs(closed - rounded) > IntegerTolerance)
    {
        return Verdict::Ambiguous;
    }

    if(rounded >= 1.0)
    {
        for(const Piece* piece : closedHere)
        {
            for(const Triangle& triangle : piece->triangles)
            {
                if(SquaredDistanceToTriangle(at, triangle.corners[0], triangle.corners[1], triangle.corners[2]) <= SurfaceTolerance * SurfaceTolerance)
                {
                    return Verdict::Ambiguous;
                }
            }
        }

        return Verdict::Inside;
    }

    return open >= WrappedByOpenPiece ? Verdict::Ambiguous : Verdict::Outside;
}
}
