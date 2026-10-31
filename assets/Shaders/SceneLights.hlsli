#ifndef ENGINE_SCENE_LIGHTS
#define ENGINE_SCENE_LIGHTS

// LIGHT.1 unshadowed Light table; identical layout to GPULight/SceneLightConstants in ShaderData.h.
// direction is the direction light travels; radiance is the light's value in GPU render units (luminous
// intensity for Point/Spot, illuminance for Directional, times the unit-luminance tint and the scene pre-scale;
// FrameLighting.cpp), applied once. |range| is the Point/Spot influence radius; a positive range fades to zero
// approaching it (range attenuation), a negative one keeps the raw inverse-square law up to a hard cutoff.
struct GPULight
{
    float3 position;
    uint type;

    float3 direction;
    float range;

    float3 radiance;
    float cosOuterCone;

    float cosInnerCone;
    float3 padding;
};

static const uint SCENE_LIGHT_CAPACITY = 64u;
static const uint SCENE_LIGHT_POINT = 0u;
static const uint SCENE_LIGHT_SPOT = 1u;
static const uint SCENE_LIGHT_DIRECTIONAL = 2u;

// Inverse-square attenuation is evaluated no closer than 1 cm, so a surface at a Light's
// position receives a large but finite contribution.
static const float SCENE_LIGHT_MINIMUM_DISTANCE = 0.01f;

// Radiance arriving at a surface from one table Light (before BRDF and N.L), and the direction
// towards it. Point and Spot: physical inverse-square falloff, zero at and beyond |range| (every path's
// per-light culling test). With range attenuation (range > 0) it is also windowed smoothly to zero,
// saturate(1 - (d / range)^4)^2; without (range < 0) the inverse-square law holds up to the cutoff.
// Spot: additionally t^2 between the outer and inner cone cosines, the profile its flux is normalized
// over (Photometry::SpotEmissionSolidAngle). Directional: constant illuminance.
float3 EvaluateSceneLightIncidence(GPULight light, float3 surfacePosition, out float3 directionToLight)
{
    if(light.type == SCENE_LIGHT_DIRECTIONAL)
    {
        directionToLight = -light.direction;
        return light.radiance;
    }

    float3 toLight = light.position - surfacePosition;
    float distanceSquared = dot(toLight, toLight);
    float distance = sqrt(distanceSquared);
    directionToLight = distance > 0.0f ? toLight / distance : float3(0.0f, 0.0f, 1.0f);

    float range = abs(light.range);

    if(!(distance < range))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float window = 1.0f;

    if(light.range > 0.0f)
    {
        float rangeRatio = distance / range;
        float rangeRatioSquared = rangeRatio * rangeRatio;
        float fade = saturate(1.0f - rangeRatioSquared * rangeRatioSquared);
        window = fade * fade;
    }

    float clampedDistanceSquared = max(distanceSquared, SCENE_LIGHT_MINIMUM_DISTANCE * SCENE_LIGHT_MINIMUM_DISTANCE);
    float attenuation = window / clampedDistanceSquared;

    if(light.type == SCENE_LIGHT_SPOT)
    {
        float cosAngle = dot(-directionToLight, light.direction);
        float coneWidth = max(light.cosInnerCone - light.cosOuterCone, 1.0e-4f);
        float coneFactor = saturate((cosAngle - light.cosOuterCone) / coneWidth);
        attenuation *= coneFactor * coneFactor;
    }

    return light.radiance * attenuation;
}

#endif
