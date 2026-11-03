#pragma once

#include "Renderer/Shader/Shader.h"

#include <filesystem>
#include <span>
#include <vector>

// PS.5B: the renderer-owned list of what a packaged game's shader bootstrap holds. Build Game cooks exactly this,
// and the packaged runtime looks entries up by the same keys ShaderLibrary uses at runtime, so no other list exists.
// A feature whose shaders are not listed here is unavailable in packaged games and must be kept off there.
enum class PackagedShaderFeature
{
    // Raster, Legacy lighting, exposure, tone mapping and the material preview the bootstrap always held.
    Core,
    Legacy,
    // Reference Path Tracer and Path Tracing (Raster Primary + ReSTIR DI) with their signal extraction.
    PathTracing,
    // The Engine path-tracing denoiser and NRD's Engine-side preparation and resolve passes.
    PathTraceDenoiser,
    // The presentation resolve at a reduced render scale (resizing requires it once a scale is eligible).
    Presentation,
    // PS.5C: Primary Surface Replacement (single and two-bounce) and its denoiser variants.
    PrimarySurfaceReplacement,
    // PS.5D: SHARC, the spatial hash radiance cache (its training and cached-radiance render libraries, and the resolve).
    Sharc,
    // PS.5E: DLSS Super Resolution's Engine-side input preparation (its motion vectors and exposure). NGX itself is
    // a runtime library, not a cooked shader.
    Dlss
};

const char* PackagedShaderFeatureName(PackagedShaderFeature feature);

// One graphics or compute entry point. The shader model follows the stage (vs/ps/cs_6_6, ShaderCompiler); no pass
// compiles with defines (ShaderDescription has none), so path, entry point and stage are the whole compilation.
struct PackagedShaderEntry
{
    const char* path;
    const char* entryPoint;
    ShaderStage stage;
    PackagedShaderFeature feature;
};

// One DXR library (lib_6_6; SM 6.9/SER variants are never cooked). An optional library is a GI.6 or DI.1 RayQuery
// twin: without it the pass keeps the original TraceRay pipeline.
struct PackagedLibraryEntry
{
    std::filesystem::path path;
    PackagedShaderFeature feature;
    bool optional = false;
};

std::span<const PackagedShaderEntry> GetPackagedShaderInventory();
std::vector<PackagedLibraryEntry> GetPackagedLibraryInventory();
