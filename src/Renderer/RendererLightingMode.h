#pragma once

#include <cstddef>
#include <cstdint>

// RTXPT.1 renderer lighting mode. Selected per frame through RendererOptions; never serialized into
// scenes, projects or cooked archives. Legacy is the default everywhere.
enum class RendererLightingMode
{
    Legacy,
    PathTracedReference,
    // RTXPT.2: progressive path tracing whose first vertex comes from the raster G-buffer. Shares the
    // reference mode's loop, settings, accumulation and resets; renders Legacy when unavailable.
    PathTracedRasterPrimary
};

// How the path tracer forms the first path vertex.
enum class PathTracePrimary
{
    // The reference mode: jittered camera rays.
    Camera,
    // Diagnostic only (never selected by the Editor): unjittered camera rays through pixel centres,
    // the RT counterpart of the raster G-buffer sample for agreement measurements.
    PixelCenter,
    // The raster-primary mode: the G-buffer sample at the pixel centre.
    Raster
};

// Settings of the progressive reference path tracer. Any change restarts accumulation.
struct PathTraceReferenceSettings
{
    static constexpr std::uint32_t MaximumSamplesPerFrame = 64;
    static constexpr std::uint32_t MaximumPathDepth = 64;

    std::uint32_t samplesPerFrame = 1;

    // Surface interactions per path. 1 is direct lighting only (including the HDRI as a distant light).
    std::uint32_t maxPathDepth = 8;

    // First vertex index whose continuation is subject to Russian roulette. Larger than maxPathDepth
    // disables roulette entirely.
    std::uint32_t russianRouletteDepth = 3;

    // Accumulation stops once this many samples per pixel are reached; the image is then held.
    std::uint32_t maxAccumulatedSamples = 1u << 16;

    bool operator==(const PathTraceReferenceSettings&) const = default;
};

inline bool IsValidPathTraceReferenceSettings(const PathTraceReferenceSettings& settings)
{
    return settings.samplesPerFrame >= 1 && settings.samplesPerFrame <= PathTraceReferenceSettings::MaximumSamplesPerFrame &&
        settings.maxPathDepth >= 1 && settings.maxPathDepth <= PathTraceReferenceSettings::MaximumPathDepth &&
        settings.russianRouletteDepth >= 1 && settings.maxAccumulatedSamples >= settings.samplesPerFrame &&
        settings.maxAccumulatedSamples <= (1u << 24);
}

enum class PathTraceReferenceAvailability
{
    // Never requested in this renderer lifetime; no resources exist.
    NotRequested,
    Available,
    // Capability, shader, descriptor or allocation failure. Legacy keeps rendering.
    Unavailable
};

// The G-buffer stores the material index as fp16 (Triangle.hlsl), which represents every integer exactly
// only up to 2048; beyond that, neighbouring indices collapse. The raster-primary mode is unavailable for
// larger material tables (MaterialAdmissionLimit is 4096). PathTracePrimarySurface.hlsli matches this.
inline constexpr std::size_t PathTraceRasterPrimaryMaximumMaterials = 2048;

enum class PathTraceRasterPrimaryAvailability
{
    // Never requested in this renderer lifetime.
    NotRequested,
    // A prerequisite is missing: ray tracing, the G-buffer, a representable material count, anything the
    // reference mode needs, or the raster-primary entry point in the DXR library. Legacy keeps rendering.
    Unavailable,
    Available
};
