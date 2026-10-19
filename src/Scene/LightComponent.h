#pragma once

#include "Math/Vector3.h"

#include <cmath>
#include <cstdint>
#include <string>

// Persisted as u32 (cooked) and as a name (loose); keep values stable.
enum class LightType : std::uint32_t
{
    Point = 0,
    Spot = 1,
    Directional = 2
};

inline const char* LightTypeName(LightType type)
{
    switch(type)
    {
    case LightType::Point:
        return "Point";

    case LightType::Spot:
        return "Spot";

    case LightType::Directional:
        return "Directional";

    default:
        return nullptr;
    }
}

// Authored light intent only. Position and direction come from the object's evaluated world
// transform (light travels along local +Z); no GPU resource, shadow map or runtime state is
// stored here. Every field is kept for every type, so switching type never discards values.
//
// Units, in the renderer's existing linear radiometric scale (the scale of the legacy scene
// Directional Light, whose DefaultProject intensity is about 3):
// - color: linear RGB tint, each channel in [0, 1]; magnitude lives in intensity.
// - intensity, Directional: irradiance on a surface facing the light (legacy sun convention).
// - intensity, Point/Spot: intensity at 1 m; irradiance falls off as intensity / distance^2.
// - range (m), Point/Spot: the direct-light influence radius; contribution is windowed to zero.
// - innerConeAngle/outerConeAngle (radians), Spot: half-angles from the +Z axis.
struct LightComponent
{
    static constexpr float MaximumColorChannel = 1.0f;
    static constexpr float MaximumIntensity = 1000000.0f;
    static constexpr float MinimumRange = 0.01f;
    static constexpr float MaximumRange = 10000.0f;
    static constexpr float MinimumOuterConeAngle = 0.01745329252f; // 1 degree.
    static constexpr float MaximumOuterConeAngle = 1.55334303427f; // 89 degrees.

    bool enabled = true;
    LightType type = LightType::Point;
    Vector3 color{1.0f, 1.0f, 1.0f};
    float intensity = 10.0f;
    float range = 10.0f;
    float innerConeAngle = 0.34906585040f; // 20 degrees.
    float outerConeAngle = 0.52359877560f; // 30 degrees.

    bool operator==(const LightComponent& other) const
    {
        return enabled == other.enabled && type == other.type && color.x == other.color.x &&
            color.y == other.color.y && color.z == other.color.z && intensity == other.intensity &&
            range == other.range && innerConeAngle == other.innerConeAngle &&
            outerConeAngle == other.outerConeAngle;
    }

    bool operator!=(const LightComponent& other) const
    {
        return !(*this == other);
    }
};

// Scene-wide bound on Light components, enabled or not. It equals the renderer's per-frame
// light capacity, so a valid scene can never exceed the GPU light table.
inline constexpr std::uint32_t MaximumSceneLights = 64;

// Defaults used by Hierarchy creation for each type. Add Component uses LightComponent{} (Point).
inline LightComponent DefaultLightComponent(LightType type)
{
    LightComponent light;
    light.type = type;

    if(type == LightType::Directional)
    {
        light.intensity = 3.0f;
    }

    return light;
}

// Shared by authoring, both codecs and future checked gameplay APIs. Every field is validated
// regardless of type so a later type switch is always valid. Values are never clamped.
inline bool ValidateLightComponent(const LightComponent& light, std::string* error = nullptr)
{
    if(error)
    {
        error->clear();
    }

    const bool finite = std::isfinite(light.color.x) && std::isfinite(light.color.y) &&
        std::isfinite(light.color.z) && std::isfinite(light.intensity) && std::isfinite(light.range) &&
        std::isfinite(light.innerConeAngle) && std::isfinite(light.outerConeAngle);
    const char* reason = nullptr;

    if(!LightTypeName(light.type))
    {
        reason = "Light type must be Point, Spot or Directional.";
    }
    else if(!finite)
    {
        reason = "Light color, intensity, range and cone angles must be finite.";
    }
    else if(light.color.x < 0.0f || light.color.y < 0.0f || light.color.z < 0.0f ||
        light.color.x > LightComponent::MaximumColorChannel || light.color.y > LightComponent::MaximumColorChannel ||
        light.color.z > LightComponent::MaximumColorChannel)
    {
        reason = "Light color channels must be between 0 and 1 (use intensity for brightness).";
    }
    else if(light.intensity < 0.0f || light.intensity > LightComponent::MaximumIntensity)
    {
        reason = "Light intensity must be between 0 and 1000000.";
    }
    else if(light.range < LightComponent::MinimumRange || light.range > LightComponent::MaximumRange)
    {
        reason = "Light range must be between 0.01 and 10000 meters.";
    }
    else if(light.outerConeAngle < LightComponent::MinimumOuterConeAngle ||
        light.outerConeAngle > LightComponent::MaximumOuterConeAngle)
    {
        reason = "Spot outer cone angle must be between 1 and 89 degrees.";
    }
    else if(light.innerConeAngle < 0.0f || light.innerConeAngle > light.outerConeAngle)
    {
        reason = "Spot inner cone angle must be between 0 degrees and the outer cone angle.";
    }

    if(reason && error)
    {
        *error = reason;
    }

    return reason == nullptr;
}
