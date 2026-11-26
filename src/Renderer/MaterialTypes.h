#pragma once

#include <cstdint>

enum class SurfaceDetailMode : std::uint32_t
{
    NormalOnly = 0,
    ParallaxOcclusion = 1,
    GeometryWithParallaxFallback = 2
};