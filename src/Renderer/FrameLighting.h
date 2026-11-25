#pragma once

#include "Renderer/ShaderData.h"
#include "Scene/DirectionalLight.h"
#include "Scene/PhotometricUnits.h"
#include "Renderer/SunAtmosphere.h"
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

    // Physical sky S2: with a physical sky and a sun owner that affects the atmosphere, `sun` is the ground-level sun: the owner's
    // top-of-atmosphere light times the atmosphere's transmittance toward it from the camera (SkyAtmosphereModel, float64), so
    // every consumer of `sun` is lit through the atmosphere. sunTransmittance is that transmittance (1 otherwise).
    bool sunThroughAtmosphere = false;
    Vector3 sunTransmittance{1.0f, 1.0f, 1.0f};

    // Physical sky S1: the sun owner's light above the atmosphere, which the physical sky scatters (SkyAtmosphere). Values in render
    // units, from the same owner and calibration as `sun`. Zero when the owner does not affect the atmosphere (S2).
    struct AtmosphereSun
    {
        // The direction the light travels (DirectionalLight's convention).
        Vector3 direction{0.0f, -1.0f, 0.0f};
        // Unit-luminance tint x authored lux x Photometry::RenderScale(referenceEV100).
        Vector3 irradiance{0.0f, 0.0f, 0.0f};
        // The reference exposure the irradiance is scaled for.
        float referenceEV100 = Photometry::DefaultReferenceEV100;
    };

    AtmosphereSun atmosphereSun;

    // Physical sky S5 (§14.1): the moon, a second atmosphere light: the first Directional after the sun owner that affects the
    // atmosphere. Above-atmosphere light in render units (zero without one); its table entry is lit through the atmosphere like the
    // sun, and the sky scatters it once (single scattering, Unreal's rule for the second light).
    AtmosphereSun atmosphereMoon;
    bool moonPresent = false;
    float moonAngularRadius = 0.0f;
    // Its light at the camera (render units, through the air): the moon's disk.
    Vector3 moonGroundIrradiance{0.0f, 0.0f, 0.0f};

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
// atmosphere: the frame's physical sky, or null without one. It changes only the snapshot's sun owner, and only when that owner
// affects the atmosphere (ResolvedSceneLight::affectsAtmosphere).
bool BuildFrameLighting(const DirectionalLight& legacySun, const SceneLightingSnapshot* snapshot,
    FrameLighting& lighting, std::string* error = nullptr, const SceneLightingSettings* proceduralSettings = nullptr,
    const SunAtmosphere* atmosphere = nullptr);

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

    // Physical sky S5B: a jump no temporal history can follow (a time-of-day cut): the sun or a table Directional (the moon among
    // them) changing its illuminance by more than LightingCutRatio either way, measured above the frame's ambient floor, or
    // turning by more than the directional threshold while it outshines that floor. The renderer handles it as a camera cut.
    // Independent of `global` and `regions`, which stay the radiance caches' finer test.
    bool historyCut = false;

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
// cutFloor (render units, illuminance): the ambient level below which a light's change does not count for historyCut, the sky's
// published irradiance-cube mean (never below LightingCutFloor).
LightingDiscontinuities DetectLightingDiscontinuities(const FrameLighting& previous, const FrameLighting& current,
    float relativeThreshold = 0.2f, float directionalDegrees = 5.0f, std::size_t maxRegions = 64,
    double cutFloor = 0.0);

// Physical sky S5B: the history-cut threshold, 2 stops in either direction, shared by the lights (historyCut above) and the sky's
// published irradiance (Renderer).
inline constexpr double LightingCutRatio = 4.0;
// About the smallest normal fp16 value (render units): below it a light or sky is too dim, and too coarsely stored, for a jump to
// matter.
inline constexpr double LightingCutFloor = 6.1e-5;

// Whether a non-negative quantity changed by more than `ratio` either way, measured above `floor`:
// (max + floor) / (min + floor) > ratio, with floor at least LightingCutFloor.
bool LightingJumpExceeds(double before, double after, double floor = LightingCutFloor, double ratio = LightingCutRatio);
