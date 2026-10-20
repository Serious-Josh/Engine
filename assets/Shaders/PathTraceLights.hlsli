#ifndef ENGINE_PATH_TRACE_LIGHTS
#define ENGINE_PATH_TRACE_LIGHTS

// RTXPT.1 light sampling for next-event estimation. Kept independent of TraceRay so a future RTXDI
// bridge can reuse the light definitions and densities; visibility is the caller's responsibility.
//
// Light classes and how each is counted exactly once:
// - Point, Spot and table Directional lights are delta lights: sampled only by NEE (MIS weight 1),
//   never found by BSDF sampling. Incidence is EvaluateSceneLightIncidence, shared with deferred.
// - The frame sun (legacy sun or shadow-owner Directional) is a uniform-radiance disk of half-angle
//   angularRadius whose irradiance at normal incidence equals the authored intensity. NEE samples the
//   cone uniformly; BSDF-sampled misses inside the cone add its radiance. Both use the power heuristic.
//   A zero angular radius degrades to a delta directional light.
// - The HDRI is sampled with its existing marginal/conditional CDF (exact solid-angle density inside
//   each cell) and combined with BSDF-sampled misses by the power heuristic.

#include "Common.hlsli"
#include "SceneLights.hlsli"

struct PathTraceEnvironment
{
    uint textureIndex;
    uint conditionalCDFIndex;
    uint marginalCDFIndex;
    uint width;
    uint height;
    float totalWeight;
};

struct PathTraceSun
{
    float3 directionToSun;
    float cosAngularRadius; // 1 for a delta sun.
    float3 irradiance;      // color * intensity at normal incidence.
};

struct PathTraceLightSample
{
    bool valid;
    float3 direction;  // Towards the light.
    float distance;    // Visibility segment length; a large value for distant lights.
    float3 radiance;   // Incident radiance (area/distant lights) or irradiance (delta lights).
    float pdf;         // Solid-angle density; 0 marks a delta light.
};

static const float PATH_TRACE_DISTANT_LIGHT_DISTANCE = 1.0e27f;

float PathTracePowerHeuristic(float pdfA, float pdfB)
{
    float a = pdfA * pdfA;
    float b = pdfB * pdfB;
    return a + b > 0.0f ? a / (a + b) : 0.0f;
}

bool PathTraceEnvironmentSamplingEnabled(PathTraceEnvironment environment)
{
    return environment.width != 0u && environment.height != 0u && isfinite(environment.totalWeight) &&
        environment.totalWeight > 0.0f && environment.conditionalCDFIndex != 0xFFFFFFFFu &&
        environment.marginalCDFIndex != 0xFFFFFFFFu;
}

float3 PathTraceEnvironmentRadiance(PathTraceEnvironment environment, SamplerState environmentSampler,
    float3 direction)
{
    Texture2D<float4> texture = ResourceDescriptorHeap[environment.textureIndex];
    float2 uv = DirectionToEquirectangularUV(direction);
    float3 radiance = texture.SampleLevel(environmentSampler, uv, 0.0f).rgb;
    return all(isfinite(radiance)) ? max(radiance, 0.0f) : float3(0.0f, 0.0f, 0.0f);
}

uint PathTraceSampleCDF(StructuredBuffer<float> cdf, uint offset, uint count, float randomValue)
{
    uint low = 0u;
    uint high = count - 1u;

    while(low < high)
    {
        uint middle = low + (high - low) / 2u;

        if(randomValue < cdf[offset + middle])
        {
            high = middle;
        }
        else
        {
            low = middle + 1u;
        }
    }

    return low;
}

float PathTraceEnvironmentCellSolidAngle(PathTraceEnvironment environment, uint row)
{
    float upper = cos(PI * float(row) / float(environment.height));
    float lower = cos(PI * float(row + 1u) / float(environment.height));
    return (2.0f * PI / float(environment.width)) * (upper - lower);
}

float PathTraceEnvironmentPdf(PathTraceEnvironment environment, float3 direction)
{
    if(!PathTraceEnvironmentSamplingEnabled(environment))
    {
        return 0.0f;
    }

    float2 uv = DirectionToEquirectangularUV(direction);
    uint column = min(uint(frac(uv.x) * environment.width), environment.width - 1u);
    uint row = min(uint(saturate(uv.y) * environment.height), environment.height - 1u);

    StructuredBuffer<float> rows = ResourceDescriptorHeap[environment.marginalCDFIndex];
    StructuredBuffer<float> columns = ResourceDescriptorHeap[environment.conditionalCDFIndex];
    uint rowOffset = row * environment.width;
    float rowMass = max(rows[row] - (row > 0u ? rows[row - 1u] : 0.0f), 0.0f);
    float columnMass = max(columns[rowOffset + column] - (column > 0u ? columns[rowOffset + column - 1u] : 0.0f), 0.0f);
    float solidAngle = PathTraceEnvironmentCellSolidAngle(environment, row);
    return solidAngle > 0.0f ? rowMass * columnMass / solidAngle : 0.0f;
}

PathTraceLightSample PathTraceSampleEnvironment(PathTraceEnvironment environment, SamplerState environmentSampler,
    float4 random)
{
    PathTraceLightSample result;
    result.valid = false;
    result.direction = float3(0.0f, 1.0f, 0.0f);
    result.distance = PATH_TRACE_DISTANT_LIGHT_DISTANCE;
    result.radiance = float3(0.0f, 0.0f, 0.0f);
    result.pdf = 0.0f;

    if(!PathTraceEnvironmentSamplingEnabled(environment))
    {
        return result;
    }

    StructuredBuffer<float> rows = ResourceDescriptorHeap[environment.marginalCDFIndex];
    StructuredBuffer<float> columns = ResourceDescriptorHeap[environment.conditionalCDFIndex];
    uint row = PathTraceSampleCDF(rows, 0u, environment.height, random.x);
    uint column = PathTraceSampleCDF(columns, row * environment.width, environment.width, random.y);

    // Uniform in longitude and in cos(theta) inside the cell: uniform solid angle within the cell.
    float u = (float(column) + random.z) / float(environment.width);
    float cosTheta = lerp(cos(PI * float(row) / float(environment.height)),
        cos(PI * float(row + 1u) / float(environment.height)), random.w);
    float v = acos(clamp(cosTheta, -1.0f, 1.0f)) / PI;

    result.direction = normalize(EquirectangularUVToDirection(float2(u, v)));
    result.pdf = PathTraceEnvironmentPdf(environment, result.direction);
    result.radiance = PathTraceEnvironmentRadiance(environment, environmentSampler, result.direction);
    result.valid = result.pdf > 0.0f && isfinite(result.pdf);
    return result;
}

bool PathTraceSunIsDelta(PathTraceSun sun)
{
    return !(sun.cosAngularRadius < 1.0f);
}

float PathTraceSunSolidAngle(PathTraceSun sun)
{
    return 2.0f * PI * (1.0f - sun.cosAngularRadius);
}

bool PathTraceSunHasEnergy(PathTraceSun sun)
{
    return any(sun.irradiance > 0.0f) && all(isfinite(sun.irradiance));
}

// Radiance of the sun disk seen along direction (zero outside the cone or for a delta sun).
float3 PathTraceSunRadiance(PathTraceSun sun, float3 direction)
{
    if(PathTraceSunIsDelta(sun) || !PathTraceSunHasEnergy(sun) || dot(direction, sun.directionToSun) < sun.cosAngularRadius)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    return sun.irradiance / PathTraceSunSolidAngle(sun);
}

float PathTraceSunPdf(PathTraceSun sun, float3 direction)
{
    if(PathTraceSunIsDelta(sun) || !PathTraceSunHasEnergy(sun) || dot(direction, sun.directionToSun) < sun.cosAngularRadius)
    {
        return 0.0f;
    }

    return 1.0f / PathTraceSunSolidAngle(sun);
}

PathTraceLightSample PathTraceSampleSun(PathTraceSun sun, float2 random)
{
    PathTraceLightSample result;
    result.valid = PathTraceSunHasEnergy(sun);
    result.direction = sun.directionToSun;
    result.distance = PATH_TRACE_DISTANT_LIGHT_DISTANCE;
    result.radiance = sun.irradiance;
    result.pdf = 0.0f;

    if(!result.valid || PathTraceSunIsDelta(sun))
    {
        return result;
    }

    float3 tangent;
    float3 bitangent;
    float signZ = sun.directionToSun.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (signZ + sun.directionToSun.z);
    float b = sun.directionToSun.x * sun.directionToSun.y * a;
    tangent = float3(1.0f + signZ * sun.directionToSun.x * sun.directionToSun.x * a, signZ * b,
        -signZ * sun.directionToSun.x);
    bitangent = float3(b, signZ + sun.directionToSun.y * sun.directionToSun.y * a, -sun.directionToSun.y);

    // Uniform in cos(theta) on [cosAngularRadius, 1]: uniform solid angle over the cone.
    float cosTheta = lerp(1.0f, sun.cosAngularRadius, random.x);
    float sinTheta = sqrt(max(1.0f - cosTheta * cosTheta, 0.0f));
    float angle = 2.0f * PI * random.y;
    result.direction = normalize(tangent * (sinTheta * cos(angle)) + bitangent * (sinTheta * sin(angle)) +
        sun.directionToSun * cosTheta);

    // Re-evaluate from the rounded direction exactly as a BSDF-sampled miss does, so a sample that
    // rounds outside the cone edge is a zero event for both strategies.
    result.radiance = PathTraceSunRadiance(sun, result.direction);
    result.pdf = PathTraceSunPdf(sun, result.direction);
    result.valid = result.pdf > 0.0f;
    return result;
}

// Table lights are delta lights; radiance carries the incident irradiance before BRDF and cosine.
PathTraceLightSample PathTraceSampleTableLight(GPULight light, float3 position)
{
    PathTraceLightSample result;
    result.pdf = 0.0f;
    result.radiance = EvaluateSceneLightIncidence(light, position, result.direction);
    result.distance = PATH_TRACE_DISTANT_LIGHT_DISTANCE;

    if(light.type != SCENE_LIGHT_DIRECTIONAL)
    {
        result.distance = length(light.position - position);
    }

    result.valid = any(result.radiance > 0.0f) && all(isfinite(result.radiance)) && result.distance > 0.0f;
    return result;
}

#endif
