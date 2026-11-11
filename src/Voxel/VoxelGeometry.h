#pragma once

// VX.2: the immutable geometry a voxel bake reads, and the exact predicates it evaluates. See
// Documentation/Voxel/VoxelArchitecture.md, sections 5.1-5.4 and 5.7.
//
// The snapshot is built once, before any region is baked (invariant I2): positions are welded by exact value, triangles are split
// into connected pieces, and each piece records whether it is closed (every edge joins exactly two triangles in opposite
// directions), its bounds and its triangles in a fixed order. Regions never re-derive any of this, so every region evaluates the
// same triangles in the same order.
//
// Input is world space in metres, in Engine's convention: a front face is clockwise seen from outside. A closed piece wound that
// way has winding number +1 inside; an inverted closed piece (a room's inner shell) has -1. Open pieces never classify anything;
// they are surface obstacles only. The algorithm follows Audio::Internal::ProbeSolidClassifier, with two differences required by
// region independence: each closed piece's winding number is rounded to an integer on its own, and open pieces are not summed.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace Voxel
{
using Point = std::array<double, 3>;

// How one object's closed pieces are oriented, overriding the snapshot policy for that object only.
enum class ObjectOrientation : std::uint8_t
{
    // The snapshot's OrientationPolicy decides.
    Automatic,
    // Never flipped: an intentionally inverted piece stays inverted (a cavity, or a deliberate negative solid), at any depth.
    AsAuthored,
    // Every closed piece counts as solid: inverted pieces are flipped whatever their depth.
    Outward
};

// One static object's triangles, already in world space.
struct MeshInput
{
    std::vector<Point> positions;
    std::vector<std::array<std::uint32_t, 3>> triangles;
    ObjectOrientation orientation = ObjectOrientation::Automatic;
};

struct SnapshotTriangle
{
    std::array<Point, 3> corners;
    std::uint32_t piece = 0;
};

// What a closed piece's orientation means, decided once per snapshot from its signed volume and its nesting depth (how many other
// closed pieces enclose it, counted by |winding number| at a point on it). See VoxelArchitecture.md, "Orientation policy".
enum class PieceOrientation
{
    // Not closed: never classifies.
    Open,
    // Positive volume at even depth: an ordinary solid, or an island inside a cavity.
    Outward,
    // Positive volume at odd depth: a solid overlapping or nested in another solid. Its interior counts twice; still Inside.
    NestedOutward,
    // Negative volume at odd depth: the inner boundary of a hollow solid. Kept: it subtracts the cavity.
    Cavity,
    // Negative volume at even depth: it encloses nothing it could hollow out, so it is an accidental inversion. Flipped under
    // FlipUnenclosedInverted (windingSign -1), kept and reported under Preserve.
    UnenclosedInverted,
    // Zero volume, or a test point lying on another closed piece: left as authored and reported.
    Undetermined
};

enum class OrientationPolicy
{
    // Flip UnenclosedInverted pieces; keep cavities. The default.
    FlipUnenclosedInverted,
    // Classify exactly as authored.
    Preserve
};

// How nesting depths are measured. Both apply the same rules; Exact is the quadratic VX.3b reference.
enum class OrientationAnalysis
{
    Accelerated,
    Exact
};

struct SnapshotOptions
{
    OrientationPolicy orientation = OrientationPolicy::FlipUnenclosedInverted;
    OrientationAnalysis analysis = OrientationAnalysis::Accelerated;
};

struct SnapshotPiece
{
    std::uint32_t object = 0;
    bool closed = false;
    PieceOrientation orientation = PieceOrientation::Open;
    // Multiplies the piece's winding number: -1 only for a flipped UnenclosedInverted piece. Triangles are never rewritten.
    std::int8_t windingSign = 1;
    double signedVolume = 0.0;
    // Other closed pieces enclosing the piece; -1 when undetermined.
    int nestingDepth = 0;
    // World-space bounds of the piece's triangles.
    Point minimum{};
    Point maximum{};
    // The piece's triangles are triangles[first, first + count).
    std::uint32_t firstTriangle = 0;
    std::uint32_t triangleCount = 0;
};

struct SnapshotReport
{
    // Triangles collapsed by welding (two corners at the same position): dropped entirely.
    std::size_t collapsedTriangles = 0;
    // Non-collapsed triangles of exactly zero area: kept for piece topology, but never tested for occupancy, distance or winding,
    // where they contribute nothing.
    std::size_t zeroAreaTriangles = 0;
    std::size_t closedPieces = 0;
    std::size_t openPieces = 0;
    // Closed pieces by orientation.
    std::size_t outwardPieces = 0;
    std::size_t nestedOutwardPieces = 0;
    std::size_t cavityPieces = 0;
    std::size_t unenclosedInvertedPieces = 0;
    // Pieces whose sign was flipped: UnenclosedInverted ones under FlipUnenclosedInverted, and inverted pieces of Outward objects.
    std::size_t flippedPieces = 0;
    // Closed pieces of objects with an orientation override (AsAuthored or Outward).
    std::size_t overriddenPieces = 0;
    std::size_t undeterminedPieces = 0;
};

class GeometrySnapshot
{
public:
    // Fails for non-finite positions or out-of-range indices, naming the object.
    static std::expected<GeometrySnapshot, std::string> Build(std::span<const MeshInput> objects, const SnapshotOptions& options = {});

    std::span<const SnapshotTriangle> GetTriangles() const noexcept
    {
        return m_triangles;
    }

    std::span<const SnapshotPiece> GetPieces() const noexcept
    {
        return m_pieces;
    }

    const SnapshotReport& GetReport() const noexcept
    {
        return m_report;
    }

private:
    static std::expected<GeometrySnapshot, std::string> BuildPieces(std::span<const MeshInput> objects);
    void ApplyOrientationPolicy(OrientationPolicy policy, OrientationAnalysis analysis);

    std::vector<SnapshotTriangle> m_triangles;
    std::vector<SnapshotPiece> m_pieces;
    // Per object, its override; indexed by SnapshotPiece::object.
    std::vector<ObjectOrientation> m_objectOrientation;
    SnapshotReport m_report;
};

// Whether a triangle touches the closed axis-aligned cube of half extent `halfExtent` around `centre` (separating axis test,
// Akenine-Moller). Contact at a face, edge or corner counts as touching.
bool TriangleTouchesCube(const Point& centre, double halfExtent, const Point& a, const Point& b, const Point& c) noexcept;

// Ericson, Real-Time Collision Detection, 5.1.5. The triangle must have non-zero area.
double SquaredDistanceToTriangle(const Point& point, const Point& a, const Point& b, const Point& c) noexcept;

// A piece's generalised winding number at `point` (Van Oosterom and Strackee), over its triangles in snapshot order, as authored:
// windingSign is not applied.
double PieceWindingNumber(const GeometrySnapshot& snapshot, const SnapshotPiece& piece, const Point& point) noexcept;

// The exact classification term of one closed piece: its winding number rounded to an integer on its own, times its windingSign.
long long ExactPieceWinding(const GeometrySnapshot& snapshot, const SnapshotPiece& piece, const Point& point) noexcept;
}
