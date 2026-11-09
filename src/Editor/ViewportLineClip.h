#pragma once

#include "Math/Matrix4.h"
#include "Math/Vector3.h"

#include <array>
#include <cmath>
#include <optional>

// M8.3: a world-space line segment on screen, for overlay wireframes. A segment with an end behind the camera is clipped to the
// near side rather than dropped, so a box seen from inside, or with a corner behind the viewer, keeps every visible edge.
// Header-only and free of ImGui, so it can be tested on its own.
namespace ViewportLineClip
{
// Pixels; x right, y down, from the viewport's top-left corner.
using ScreenPoint = std::array<float, 2>;

struct ScreenSegment
{
    ScreenPoint start{};
    ScreenPoint end{};
};

// Clip-space w below this is treated as behind the camera.
inline constexpr float MinimumW = 1.0e-4f;

inline std::array<float, 4> ToClip(const Matrix4& viewProjection, const Vector3& point) noexcept
{
    std::array<float, 4> clip{};

    for(std::size_t row = 0; row < 4; ++row)
    {
        clip[row] = viewProjection.m[row][0] * point.x + viewProjection.m[row][1] * point.y + viewProjection.m[row][2] * point.z +
            viewProjection.m[row][3];
    }

    return clip;
}

inline ScreenPoint ToScreen(const std::array<float, 4>& clip, ScreenPoint origin, ScreenPoint size) noexcept
{
    return {origin[0] + (clip[0] / clip[3] + 1.0f) * 0.5f * size[0], origin[1] + (1.0f - clip[1] / clip[3]) * 0.5f * size[1]};
}

// Null when the whole segment is behind the camera or not finite. Only the near side is clipped: the draw list's clip rectangle
// trims the rest, and a point far off screen still projects to a finite position once w is positive.
inline std::optional<ScreenSegment> Project(const Matrix4& viewProjection, const Vector3& start, const Vector3& end, ScreenPoint origin,
    ScreenPoint size) noexcept
{
    std::array<float, 4> a = ToClip(viewProjection, start);
    std::array<float, 4> b = ToClip(viewProjection, end);

    for(const float value : {a[0], a[1], a[3], b[0], b[1], b[3]})
    {
        if(!std::isfinite(value))
        {
            return std::nullopt;
        }
    }

    const bool aBehind = a[3] < MinimumW;
    const bool bBehind = b[3] < MinimumW;

    if(aBehind && bBehind)
    {
        return std::nullopt;
    }

    // Move the end behind the camera along the segment to where w reaches the minimum.
    if(aBehind || bBehind)
    {
        std::array<float, 4>& behind = aBehind ? a : b;
        const std::array<float, 4>& front = aBehind ? b : a;
        const float t = (front[3] - MinimumW) / (front[3] - behind[3]);

        for(std::size_t index = 0; index < 4; ++index)
        {
            behind[index] = front[index] + (behind[index] - front[index]) * t;
        }
    }

    return ScreenSegment{ToScreen(a, origin, size), ToScreen(b, origin, size)};
}
}
