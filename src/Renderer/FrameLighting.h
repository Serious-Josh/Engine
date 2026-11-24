#pragma once

#include "Renderer/ShaderData.h"
#include "Scene/DirectionalLight.h"
#include "Scene/PhotometricUnits.h"
#include "Scene/SceneLighting.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

// The renderer's per-frame lighting inputs, derived from a captured snapshot. Values only, in GPU render units:
// every physical light value is converted here, once, for all rendering paths (PhotometricUnits.h).
struct FrameLighting
{
    // Drives the existing single sun path: DirectionalLightConstants (deferred direct light,
    // RTGI and reflection secondary lighting) and the ray-traced shadow direction.
    DirectionalLight sun;

    // The scene light table (many-light M2: uploaded to SceneLightBuffer and read by every scene-light consumer).
    SceneLightTable table{};

    // RTXPT.3: stable scene object IDs, used only by the RTXDI light bridge. tableLightIDs parallels the
    // table.lightCount records; sunObjectID names the shadow owner and is empty for the legacy sun.
    std::vector<std::string> tableLightIDs;
    std::string sunObjectID;
};

// Selects the sun and fills the light table in one step, so a frame never mixes sources.
// - No snapshot (procedural demo scenes): the scene's DirectionalLight (lux), no table, calibrated by
//   proceduralSettings (the default calibration when null).
// - Captured file-scene snapshot: its objects own directional illumination, and its scene settings own the
//   photometric calibration. No owner means a dark sun; no legacy fallback survives deletion.
//   The owner's angular radius feeds the existing shadow, path-trace and RTXDI sun paths.
// Explicit procedural callers may opt into their compatibility source on a manual snapshot.
// Fails, leaving `lighting` untouched, if the table would overflow or a value is not finite.
bool BuildFrameLighting(const DirectionalLight& legacySun, const SceneLightingSnapshot* snapshot,
    FrameLighting& lighting, std::string* error = nullptr, const SceneLightingSettings* proceduralSettings = nullptr);

// Only local lights affect this key. Additional Directionals retain direct-only semantics.
bool SameLocalLightInputs(const SceneLightTable& left, const SceneLightTable& right);

// Discrete lighting changes a radiance cache cannot follow by accumulation (SHARC_ReSTIR_PT_Investigation.md §9.5): its
// cached multi-bounce light feeds back into itself, so after a sudden change it lags for tens of frames. Continuous
// changes (a moving light, a slowly turning sun, a gently animated intensity) stay below the thresholds and are left
// to the cache's accumulation window.
struct LightingDiscontinuities
{
    // A world-space sphere (centre xyz, radius w) whose cached radiance no longer holds.
    using Region = std::array<float, 4>;

    // The sun, an additional Directional or the whole table changed: everything cached is stale.
    bool global = false;
    std::vector<Region> regions;

    bool Empty() const
    {
        return !global && regions.empty();
    }
};

// Compares one frame's lighting with the previous frame's. Local lights are matched by tableLightIDs (by table index when
// the IDs are not unique). A local light is a discontinuity when it appears or disappears, when its emitted radiance
// changes by more than `relativeThreshold` in any channel (switched on or off, an intensity or colour jump), when its
// range or cone changes as much, or when it jumps more than half its range or turns by more than 30 degrees in one
// frame; its region is its influence sphere before and after. The sun or a table Directional changing radiance by more
// than the threshold, or turning by more than `directionalDegrees`, is global, as is a light without a finite range.
// More than maxRegions regions also become global.
LightingDiscontinuities DetectLightingDiscontinuities(const FrameLighting& previous, const FrameLighting& current,
    float relativeThreshold = 0.2f, float directionalDegrees = 5.0f, std::size_t maxRegions = 64);
