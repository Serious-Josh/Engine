#ifndef ENGINE_SCENE_LIGHTS
#define ENGINE_SCENE_LIGHTS

// LIGHT.1 unshadowed Light table; identical layout to GPULight/SceneLightConstants in ShaderData.h.
// direction is the direction light travels; radiance is color * intensity, applied once.
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
// towards it. Point and Spot: inverse-square falloff windowed smoothly to zero at range
// (saturate(1 - (d / range)^4)^2). Spot: additionally (smoothstep-like) t^2 between the outer and
// inner cone cosines. Directional: constant, no range.
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

    if(!(distance < light.range))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float rangeRatio = distance / light.range;
    float rangeRatioSquared = rangeRatio * rangeRatio;
    float window = saturate(1.0f - rangeRatioSquared * rangeRatioSquared);
    float clampedDistanceSquared = max(distanceSquared, SCENE_LIGHT_MINIMUM_DISTANCE * SCENE_LIGHT_MINIMUM_DISTANCE);
    float attenuation = (window * window) / clampedDistanceSquared;

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
