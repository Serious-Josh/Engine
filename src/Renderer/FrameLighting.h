#pragma once

#include "Renderer/ShaderData.h"
#include "Scene/DirectionalLight.h"
#include "Scene/SceneLighting.h"

#include <array>
#include <string>

static_assert(SceneLightConstants::Capacity == SceneLightingSnapshot::MaximumAdditionalLights,
    "The GPU light table must hold every Light a valid scene can author.");

// The renderer's per-frame lighting inputs, derived from a captured snapshot. Values only.
struct FrameLighting
{
    // Drives the existing single sun path: DirectionalLightConstants (deferred direct light,
    // RTGI and reflection secondary lighting) and the ray-traced shadow direction.
    DirectionalLight sun;

    // Deferred direct lights (b3), also bound unchanged for local secondary-hit RTGI (b2).
    SceneLightConstants table{};

    // RTXPT.3: stable scene object IDs, used only by the RTXDI light bridge. tableLightIDs parallels the
    // first table.lightCount records; sunObjectID names the shadow owner and is empty for the legacy sun.
    std::array<std::string, SceneLightConstants::Capacity> tableLightIDs;
    std::string sunObjectID;
};

// Selects the sun and fills the light table in one step, so a frame never mixes sources.
// - No snapshot (demo scenes, historical callers): the scene's legacy DirectionalLight, no table.
// - Snapshot without Directional Light components: the legacy sun plus Point/Spot Lights.
// - Snapshot with Directional Light components: the legacy sun is replaced by the shadow owner
//   (direction, color, intensity; angular radius stays the scene's legacy value), or by a dark sun
//   (zero intensity) when no owner is enabled and resolved; the remaining Lights fill the table.
// Fails, leaving `lighting` untouched, if the table would overflow or a value is not finite.
bool BuildFrameLighting(const DirectionalLight& legacySun, const SceneLightingSnapshot* snapshot,
    FrameLighting& lighting, std::string* error = nullptr);

// Only local lights affect this key. Additional Directionals retain direct-only semantics.
bool SameLocalLightInputs(const SceneLightConstants& left, const SceneLightConstants& right);
