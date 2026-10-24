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
    PathTracedRasterPrimary,
    // RTXPT.3: the raster-primary mode with RTXDI initial sampling for direct light at the first vertex
    // (secondary vertices keep exhaustive next-event estimation). Renders Legacy when unavailable.
    PathTracedRasterPrimaryReSTIRDI
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
    Raster,
    // RTXPT.3: Raster, with vertex-0 direct light from one RTXDI initial reservoir per path sample.
    RasterReSTIRDI
};

// Settings of the progressive reference path tracer. Any change restarts accumulation.
struct PathTraceReferenceSettings
{
    static constexpr std::uint32_t MaximumSamplesPerFrame = 64;
    static constexpr std::uint32_t MaximumPathDepth = 64;

    std::uint32_t samplesPerFrame = 1;

    // Surface interactions per path. 1 is direct lighting only (including the HDRI as a distant light).
    std::uint32_t maxPathDepth = 3;

    // First vertex index whose continuation is subject to Russian roulette. Larger than maxPathDepth
    // disables roulette entirely.
    std::uint32_t russianRouletteDepth = 3;

    // Accumulation stops once this many samples per pixel are reached; the image is then held.
    std::uint32_t maxAccumulatedSamples = 1u << 16;

    bool operator==(const PathTraceReferenceSettings&) const = default;
};

// SHARC.4E experimental cached-radiance rendering (Path Traced Raster Primary, and with ReSTIR DI since SHARC.5B;
// off by default, not saved).
struct SharcSettings
{
    bool enabled = false;
    // One sparse training path per trainingTile x trainingTile pixels per frame (5 or 8).
    std::uint32_t trainingTile = 5;
    // A cache entry completes a path only with at least this many samples.
    std::uint32_t minimumSamples = 64;
    // No training, insertion or resolve; the existing cache keeps being queried.
    bool freezeTraining = false;
    // Lookup and termination counters for the statistics display (a small per-frame readback).
    bool statistics = false;
    // Incremented to request a cache reset.
    std::uint32_t resetRequest = 0;
    // SHARC.5F: distance-adaptive cell sizes (the default since SHARC.5F.1); false keeps the uniform 0.1 m grid.
    bool adaptiveResolution = true;
    // SHARC.6A (experimental): SHARC-assisted initialization of rejected diffuse denoiser history. Needs the denoiser.
    bool historyPrior = false;

    bool operator==(const SharcSettings&) const = default;
};

// A short GPU-side summary of the cache for the statistics display, two frames old.
struct SharcStatus
{
    bool active = false;
    bool statisticsValid = false;
    std::uint32_t eligibleQueries = 0;
    std::uint32_t lookupHits = 0;
    std::uint32_t substitutions = 0;
    std::uint32_t occupiedEntries = 0;
    std::uint64_t trainedFrames = 0;
    std::uint64_t cacheBytes = 0;
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
