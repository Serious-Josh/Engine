#ifndef ENGINE_RTXDI_LIGHTS
#define ENGINE_RTXDI_LIGHTS

// RTXPT.3 R3.2: RTXDI light records (RtxdiLightBridge.h) and how each one is sampled from RTXDI's uv in
// [0, 1]^2. Used only by the raster-primary ReSTIR DI variant; every light reuses the path tracer's own
// definitions, so incidence, attenuation and densities are exactly those of PathTraceLights.hlsli.
// - Point, Spot, Directional (table): delta lights, uv ignored (PathTraceSampleTableLight, pdf 0).
// - Sun: uv drives PathTraceSampleSun's two randoms; uniform over the cone, or a delta sun.
// - HDRI: uv drives a continuous inversion of the marginal/conditional CDF. The density is exactly
//   PathTraceEnvironmentPdf, the one PathTraceSampleEnvironment draws from with four randoms.
//
// PT.2B: a record is a GPULight (64 bytes) whose type is the light kind, self-contained so that a previous frame's record
// describes the previous light. Table kinds are the table light itself; the sun keeps its direction to the sun in
// direction, its irradiance in radiance and cos(angular radius) in cosOuterCone; the HDRI keeps only its kind (its
// distribution is the frame's, RtxdiLightBridge.h).

#include "PathTraceLights.hlsli"

// Table kinds equal the GPULight types.
static const uint RTXDI_LIGHT_KIND_POINT = SCENE_LIGHT_POINT;
static const uint RTXDI_LIGHT_KIND_SPOT = SCENE_LIGHT_SPOT;
static const uint RTXDI_LIGHT_KIND_DIRECTIONAL = SCENE_LIGHT_DIRECTIONAL;
static const uint RTXDI_LIGHT_KIND_SUN = 3u;
static const uint RTXDI_LIGHT_KIND_ENVIRONMENT = 4u;
// No light: the record of an index outside its frame's set. Every sample of it is invalid.
static const uint RTXDI_LIGHT_KIND_INVALID = 0xFFFFFFFFu;

static const uint RTXDI_LIGHT_RECORD_BYTES = 64u;

// Largest float below 1, so a remainder never leaves its cell.
static const float RTXDI_ONE_MINUS_EPSILON = 0.99999994f;

GPULight RtxdiInvalidLightRecord()
{
    GPULight record = (GPULight)0;
    record.type = RTXDI_LIGHT_KIND_INVALID;
    return record;
}

// The record at byte `offset` of a light data buffer (RtxdiLightDataLayout).
GPULight RtxdiLoadLightRecord(ByteAddressBuffer lightData, uint offset)
{
    return lightData.Load<GPULight>(offset);
}

// The sun a sun record describes.
PathTraceSun RtxdiSunFromRecord(GPULight record)
{
    PathTraceSun sun;
    sun.directionToSun = record.direction;
    sun.cosAngularRadius = record.cosOuterCone;
    sun.irradiance = record.radiance;
    return sun;
}

// The cell PathTraceSampleCDF selects for randomValue, plus where randomValue lies within that cell's mass,
// rescaled to [0, 1). The selected cell always has positive mass (the search returns the first entry above
// randomValue, so a zero-mass cell is never chosen), except when randomValue reaches the final CDF value of 1;
// then the last cell is returned with remainder clamped, as PathTraceSampleCDF does.
uint RtxdiInvertCDF(StructuredBuffer<float> cdf, uint offset, uint count, float randomValue, out float remainder)
{
    uint index = PathTraceSampleCDF(cdf, offset, count, randomValue);
    float lower = index > 0u ? cdf[offset + index - 1u] : 0.0f;
    float mass = cdf[offset + index] - lower;
    remainder = mass > 0.0f ? clamp((randomValue - lower) / mass, 0.0f, RTXDI_ONE_MINUS_EPSILON) : 0.0f;
    return index;
}

// The HDRI sample at uv: row from uv.y, column from uv.x, then uniform in longitude and in cos(theta) inside
// the cell (uniform solid angle), as PathTraceSampleEnvironment does. Given the cell, each remainder is
// uniform, so the density is rowMass * columnMass / cellSolidAngle. The PDF and radiance are re-evaluated from
// the final direction, so rounding at a cell edge can never make sampling and evaluation disagree.
PathTraceLightSample RtxdiSampleEnvironmentAtUV(PathTraceEnvironment environment, SamplerState environmentSampler,
    float2 uv)
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
    float rowRemainder;
    float columnRemainder;
    uint row = RtxdiInvertCDF(rows, 0u, environment.height, uv.y, rowRemainder);
    uint column = RtxdiInvertCDF(columns, row * environment.width, environment.width, uv.x, columnRemainder);

    float u = (float(column) + columnRemainder) / float(environment.width);
    float cosTheta = lerp(cos(PI * float(row) / float(environment.height)),
        cos(PI * float(row + 1u) / float(environment.height)), rowRemainder);
    float v = acos(clamp(cosTheta, -1.0f, 1.0f)) / PI;

    result.direction = normalize(EquirectangularUVToDirection(float2(u, v)));
    result.pdf = PathTraceEnvironmentPdf(environment, result.direction);
    result.radiance = PathTraceEnvironmentRadiance(environment, environmentSampler, result.direction);
    result.valid = result.pdf > 0.0f && isfinite(result.pdf);
    return result;
}

// PT.2B: the inverse of RtxdiSampleEnvironmentAtUV: the uv that maps to `direction` (up to rounding), found in the cell
// PathTraceEnvironmentPdf evaluates. False, with uv 0, where the HDRI cannot be sampled or the cell has no mass: no uv
// reaches that direction, so the uv-space density there is 0. Elsewhere the uv-space density of uniform uv is 1.
bool RtxdiEnvironmentUVFromDirection(PathTraceEnvironment environment, float3 direction, out float2 uv)
{
    uv = float2(0.0f, 0.0f);

    if(!PathTraceEnvironmentSamplingEnabled(environment))
    {
        return false;
    }

    float3 unit = normalize(direction);
    float2 equirectangular = DirectionToEquirectangularUV(unit);
    float columnPosition = frac(equirectangular.x) * float(environment.width);
    uint column = min(uint(columnPosition), environment.width - 1u);
    uint row = min(uint(saturate(equirectangular.y) * float(environment.height)), environment.height - 1u);

    StructuredBuffer<float> rows = ResourceDescriptorHeap[environment.marginalCDFIndex];
    StructuredBuffer<float> columns = ResourceDescriptorHeap[environment.conditionalCDFIndex];
    uint rowOffset = row * environment.width;
    float rowLower = row > 0u ? rows[row - 1u] : 0.0f;
    float rowMass = rows[row] - rowLower;
    float columnLower = column > 0u ? columns[rowOffset + column - 1u] : 0.0f;
    float columnMass = columns[rowOffset + column] - columnLower;

    if(!(rowMass > 0.0f) || !(columnMass > 0.0f))
    {
        return false;
    }

    // Uniform in cos(theta) inside the row (cos(theta) = y for Engine's equirectangular mapping), uniform in longitude
    // inside the column: the remainders RtxdiSampleEnvironmentAtUV interpolates with.
    float upper = cos(PI * float(row) / float(environment.height));
    float lower = cos(PI * float(row + 1u) / float(environment.height));
    float rowRemainder = upper > lower ? saturate((upper - unit.y) / (upper - lower)) : 0.0f;
    float columnRemainder = saturate(columnPosition - float(column));

    uv.x = min(columnLower + columnRemainder * columnMass, RTXDI_ONE_MINUS_EPSILON);
    uv.y = min(rowLower + rowRemainder * rowMass, RTXDI_ONE_MINUS_EPSILON);
    return true;
}

// The sample of one RTXDI light at uv, in the path tracer's convention: radiance is incident radiance for
// the sun disk and the HDRI and irradiance for delta lights; pdf is the solid-angle density, 0 for delta
// lights. An invalid record gives an invalid sample. Converting to RTXDI's target-PDF convention is the
// application bridge's job (R3.4).
PathTraceLightSample RtxdiSampleLight(GPULight record, PathTraceEnvironment environment, SamplerState environmentSampler,
    float3 position, float2 uv)
{
    if(record.type == RTXDI_LIGHT_KIND_SUN)
    {
        return PathTraceSampleSun(RtxdiSunFromRecord(record), uv);
    }

    if(record.type == RTXDI_LIGHT_KIND_ENVIRONMENT)
    {
        return RtxdiSampleEnvironmentAtUV(environment, environmentSampler, uv);
    }

    if(record.type <= RTXDI_LIGHT_KIND_DIRECTIONAL)
    {
        return PathTraceSampleTableLight(record, position);
    }

    PathTraceLightSample invalid = (PathTraceLightSample)0;
    invalid.valid = false;
    return invalid;
}

#endif
