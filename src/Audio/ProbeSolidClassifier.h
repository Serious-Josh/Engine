#pragma once

// Private to the audio module (M7.1): is a point inside solid acoustic geometry? See SteamAudioIntegration.md, section 33.
//
// Rays can't answer this reliably: a floor, a hollow room and a mesh with a gap all look alike from one point. The winding number can
// where a surface is provably closed, because then it is an exact integer. So the classifier splits every mesh into connected pieces,
// welds them by exact position, and trusts only a piece whose every edge joins exactly two triangles in opposite directions:
//
// * Closed pieces add up to an integer: 0 outside, 1 inside. A closed piece facing inwards, such as a room's inner shell, counts -1, so
//   a thick-walled room's interior is 1 - 1 = 0.
// * Everything else can't enclose anything for certain. A point that an open piece mostly wraps is Ambiguous; it is never Inside.
//
// Geometry is in Engine space, where a front face is clockwise seen from outside. A wrongly wound solid reads as a shell, so its
// probes are kept: the failure is the safe one.
//
// No SDK types, and arithmetic is in doubles over a fixed order, so results are deterministic.

#include "Audio/AudioSystem.h"

#include <array>
#include <cstddef>
#include <vector>

namespace Audio::Internal
{
class ProbeSolidClassifier final
{
public:
    enum class Verdict
    {
        Outside,
        Inside,
        // Retained by the caller: on a surface, or mostly wrapped by a surface with gaps.
        Ambiguous
    };

    explicit ProbeSolidClassifier(const std::vector<ProbeGeometryObject>& geometry);

    Verdict Classify(const std::array<float, 3>& point) const;
    std::size_t GetClosedPieces() const noexcept { return m_closedPieces; }
    std::size_t GetOpenPieces() const noexcept { return m_openPieces; }

private:
    struct Triangle
    {
        std::array<std::array<double, 3>, 3> corners;
    };

    struct Piece
    {
        std::vector<Triangle> triangles;
        std::array<double, 3> minimum{};
        std::array<double, 3> maximum{};
        bool closed = false;
    };

    void AddObject(const ProbeGeometryObject& object);
    static double WindingNumber(const Piece& piece, const std::array<double, 3>& point) noexcept;

    std::vector<Piece> m_pieces;
    std::size_t m_closedPieces = 0;
    std::size_t m_openPieces = 0;
};
}
