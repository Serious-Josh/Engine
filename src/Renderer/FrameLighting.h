#pragma once

#include "Renderer/ShaderData.h"
#include "Scene/DirectionalLight.h"
#include "Scene/PhotometricUnits.h"
#include "Scene/SceneLighting.h"

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
