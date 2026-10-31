// R.9I.3 dedicated reflective caustics: light-first tracer (additive development mode).
//
// The P5/P6 estimator approved in Documentation/Rendering/R9I3CausticsIntegrationDesign.md
// (addendum) and validated in R9I3P5P6TransportEstimatorValidation.md, unchanged:
//   w0 ~ q (complete sphere/cap proposal), first hit = registered reflector,
//   beta1 = radiance a(d) / q, a(d) = incidence(d) d^2 / radiance (Reference light model),
//   w1 ~ PathTraceSampleBSDF at the reflector (view = -w0), beta2 = beta1 * weight,
//   first hit along w1 = registered receiver, the Reference camera ray through s(x2) must commit
//   the same receiver point, and
//   Z = beta2 f2(-w1, w_c) |n2.w_c| W H J / d_c^2,  J = |P00 P11| / (4 cos^3).
// One deposit record per attempt (pixel or 0xFFFFFFFF, Z bits); every attempt counts in N.

#include "PathTraceBSDF.hlsli"
#include "SceneLights.hlsli"

RaytracingAccelerationStructure SceneTopLevel : register(t0);
StructuredBuffer<GPUMaterial> Materials : register(t1);
ByteAddressBuffer Lights : register(t2);
ByteAddressBuffer InstanceData : register(t3);   // By dense InstanceID, 48-byte stride.
ByteAddressBuffer SlotMaterials : register(t4);
ByteAddressBuffer Vertices : register(t5);       // The admitted static source shared by both rows.
ByteAddressBuffer Indices : register(t6);

cbuffer CausticTraceConstants : register(b0)
{
    row_major float4x4 viewProjection;
    row_major float4x4 inverseViewProjection;
    float3 cameraPosition;
    uint recordsIndex;
    float3 capAxis;
    float cosineHalfAngle;
    float inverseSolidAngle;
    float sphereProbability;
    uint attempts;
    uint seed;
    uint lightOffset;
    uint reflectorDense;
    uint reflectorNative;
    uint receiverDense;
    uint receiverNative;
    uint materialTable;
    uint materialAverageTable;
    uint materialCosineMapping;
    uint2 resolution;
    uint triangleCount;              // The certified primitive range [0, triangleCount) of the source.
    uint padding;
    float3 cameraForward;
    float jacobianScale;
};

uint CausticHash(uint value)
{
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    value ^= value >> 16;
    return value;
}

float CausticUniform(inout uint state)
{
    state = CausticHash(state);
    return (state >> 8) * (1.0f / 16777216.0f);
}

// Verbatim from PathTraceReference.hlsl, which owns registers and cannot be included here.
float3 CausticOffsetRayOrigin(float3 position, float3 normal)
{
    const float originThreshold = 1.0f / 32.0f;
    const float floatScale = 1.0f / 65536.0f;
    const float integerScale = 256.0f;
    int3 integerOffset = int3(integerScale * normal);
    float3 integerOffsetPosition = asfloat(asint(position) + select(position < 0.0f, -integerOffset, integerOffset));
    return select(abs(position) < originThreshold, position + floatScale * normal, integerOffsetPosition);
}

struct CausticHit
{
    bool hit;
    uint dense;
    uint native;
    uint primitive;
    float t;
    float3 position;
    float3 geometricNormal;
    float3 shadingNormal;
    uint material;
};

CausticHit CausticTrace(float3 origin, float3 direction, float maximum)
{
    CausticHit result = (CausticHit)0;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0f;
    ray.TMax = maximum;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(SceneTopLevel, RAY_FLAG_NONE, 0xFF, ray);
    query.Proceed();
    result.hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;

    if(!result.hit)
    {
        return result;
    }

    result.dense = query.CommittedInstanceID();
    result.native = query.CommittedInstanceIndex();
    result.primitive = query.CommittedPrimitiveIndex();
    result.t = query.CommittedRayT();
    result.position = origin + direction * result.t;

    // The TLAS can hit any production mesh. Only these two registered identities
    // share the admitted source bound here; reject all other geometry, and any
    // primitive outside the certified range, before it can address its buffers.
    const bool registered = result.primitive < triangleCount &&
        ((result.dense == reflectorDense && result.native == reflectorNative) ||
         (result.dense == receiverDense && result.native == receiverNative));

    if(!registered)
    {
        return result;
    }

    const uint record = result.dense * 48u;
    result.material = InstanceData.Load(record + 32u) == 0xFFFFFFFFu ?
        SlotMaterials.Load(InstanceData.Load(record + 44u) * 4u) : 0xFFFFFFFFu;

    // Geometry from the admitted source; only used when the identity matches a registered row.
    const uint3 corners = Indices.Load3(result.primitive * 12u);
    const float2 b = query.CommittedTriangleBarycentrics();
    const float3x3 objectToWorld = (float3x3)query.CommittedObjectToWorld3x4();
    const float3 p0 = asfloat(Vertices.Load3(corners.x * 48u));
    const float3 e1 = mul(objectToWorld, asfloat(Vertices.Load3(corners.y * 48u)) - p0);
    const float3 e2 = mul(objectToWorld, asfloat(Vertices.Load3(corners.z * 48u)) - p0);
    result.geometricNormal = normalize(cross(e1, e2));
    result.shadingNormal = normalize(mul(objectToWorld,
        asfloat(Vertices.Load3(corners.x * 48u + 12u)) * (1.0f - b.x - b.y) +
        asfloat(Vertices.Load3(corners.y * 48u + 12u)) * b.x + asfloat(Vertices.Load3(corners.z * 48u + 12u)) * b.y));
    return result;
}

bool CausticIs(CausticHit hit, uint dense, uint native)
{
    return hit.hit && hit.dense == dense && hit.native == native && hit.primitive < triangleCount;
}

// PathTraceBuildSurface for the accepted domain: constant parameters and the admitted 1x1
// fallback base texture, read exactly.
bool CausticBuildSurface(CausticHit hit, float3 view, out PathTraceSurface surface)
{
    surface = (PathTraceSurface)0;
    surface.view = view;
    surface.geometricNormal = hit.geometricNormal;
    surface.normal = hit.shadingNormal;
    surface.materialTableIndex = materialTable;
    surface.materialAverageTableIndex = materialAverageTable;
    surface.materialTableCosineMapping = materialCosineMapping;

    if(dot(surface.geometricNormal, surface.view) < 0.0f)
    {
        surface.geometricNormal = -surface.geometricNormal;
        surface.normal = -surface.normal;
    }

    GPUMaterial material = Materials[hit.material];

    if(material.model > 2u || material.textureIndex == 0xFFFFFFFFu)
    {
        return false;
    }

    Texture2D<float4> baseColorTexture = ResourceDescriptorHeap[material.textureIndex];
    surface.baseColor = max(material.baseColor * baseColorTexture.Load(int3(0, 0, 0)).rgb, 0.0f);
    surface.material = material;
    return PathTraceFinalizeSurface(surface);
}

void CausticInvalidMeasurement(uint attempt)
{
    RWByteAddressBuffer records = ResourceDescriptorHeap[recordsIndex];
    records.Store4(attempt * 16u, uint4(0u, 0x7FC00000u, 0u, 0u));
}

[numthreads(64, 1, 1)]
void CSTrace(uint3 thread : SV_DispatchThreadID)
{
    const uint attempt = thread.x;

    if(attempt >= attempts)
    {
        return;
    }

    RWByteAddressBuffer records = ResourceDescriptorHeap[recordsIndex];
    uint state = CausticHash(attempt * 0x9E3779B9u ^ seed);
    const float branch = CausticUniform(state);
    const float u1 = CausticUniform(state);
    const float u2 = CausticUniform(state);
    const bool sphereBranch = cosineHalfAngle == -1.0f || branch < sphereProbability;
    const float cosine = sphereBranch ? 1.0f - 2.0f * u1 : 1.0f - u1 * (1.0f - cosineHalfAngle);
    const float sine = sqrt(max(0.0f, 1.0f - cosine * cosine));
    const float phi = 2.0f * PI * u2;
    const float3 axis = sphereBranch ? float3(0.0f, 0.0f, 1.0f) : capAxis;
    float3 tangent;
    float3 bitangent;
    PathTraceBuildFrame(axis, tangent, bitangent);
    const float3 emitted = normalize(tangent * (sine * cos(phi)) + bitangent * (sine * sin(phi)) + axis * cosine);
    const float cap = cosineHalfAngle == -1.0f ? inverseSolidAngle :
        (dot(capAxis, emitted) >= cosineHalfAngle ? inverseSolidAngle : 0.0f);
    const float emissionPdf = sphereProbability / (4.0f * PI) + (1.0f - sphereProbability) * cap;

    if(!all(isfinite(emitted)) || !isfinite(emissionPdf) || !(emissionPdf > 0.0f))
    {
        CausticInvalidMeasurement(attempt);
        return;
    }

    GPULight light;
    light.position = asfloat(Lights.Load3(lightOffset));
    light.type = Lights.Load(lightOffset + 12u);
    light.direction = asfloat(Lights.Load3(lightOffset + 16u));
    light.range = asfloat(Lights.Load(lightOffset + 28u));
    light.radiance = asfloat(Lights.Load3(lightOffset + 32u));
    light.cosOuterCone = asfloat(Lights.Load(lightOffset + 44u));
    light.cosInnerCone = asfloat(Lights.Load(lightOffset + 48u));
    light.padding = float3(0.0f, 0.0f, 0.0f);

    uint pixel = 0xFFFFFFFFu;
    float3 contribution = float3(0.0f, 0.0f, 0.0f);
    const CausticHit first = CausticTrace(light.position, emitted, 10000.0f);
    PathTraceSurface reflector;

    if(CausticIs(first, reflectorDense, reflectorNative) && CausticBuildSurface(first, -emitted, reflector))
    {
        float3 toLight;
        const float3 incidence = EvaluateSceneLightIncidence(light, first.position, toLight);
        const float radiance = PathTraceLuminance(light.radiance);
        const float attenuation = radiance > 0.0f ? PathTraceLuminance(incidence) * first.t * first.t / radiance : 0.0f;
        const float3 random = float3(CausticUniform(state), CausticUniform(state), CausticUniform(state));
        const PathTraceBSDFSample sampled = PathTraceSampleBSDF(reflector, random);

        if(!all(isfinite(incidence)) || !isfinite(attenuation) || !isfinite(sampled.pdf) ||
            !all(isfinite(sampled.weight)) || !all(isfinite(sampled.direction)))
        {
            CausticInvalidMeasurement(attempt);
            return;
        }

        if(sampled.valid && !sampled.delta)
        {
            const float3 throughput = light.radiance * attenuation / emissionPdf * sampled.weight;
            if(!all(isfinite(throughput)) || any(throughput < 0.0f))
            {
                CausticInvalidMeasurement(attempt);
                return;
            }

            const float3 offset = dot(reflector.geometricNormal, sampled.direction) > 0.0f ?
                reflector.geometricNormal : -reflector.geometricNormal;
            const CausticHit receiver = CausticTrace(CausticOffsetRayOrigin(first.position, offset), sampled.direction, 10000.0f);

            if(CausticIs(receiver, receiverDense, receiverNative))
            {
                const float4 clip = mul(viewProjection, float4(receiver.position, 1.0f));
                if(!all(isfinite(clip)))
                {
                    CausticInvalidMeasurement(attempt);
                    return;
                }

                const float2 ndc = clip.xy / clip.w;
                const float depth = clip.z / clip.w;
                const float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);

                if(clip.w > 0.0f && all(uv >= 0.0f) && all(uv < 1.0f) && depth >= 0.0f && depth <= 1.0f)
                {
                    // Exact visibility: the Reference camera ray through s(x2) commits the same point.
                    const float2 cameraNdc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
                    const float4 nearPoint = mul(inverseViewProjection, float4(cameraNdc, 0.0f, 1.0f));
                    const float4 farPoint = mul(inverseViewProjection, float4(cameraNdc, 1.0f, 1.0f));
                    const float3 origin = nearPoint.xyz / nearPoint.w;
                    const float3 segment = farPoint.xyz / farPoint.w - origin;
                    const float maximum = length(segment);
                    const float expected = length(receiver.position - origin);
                    PathTraceSurface surface;
                    const float3 toCamera = cameraPosition - receiver.position;
                    const float cameraDistance = length(toCamera);
                    if(!isfinite(cameraDistance) || !(cameraDistance > 0.0f) ||
                        !all(isfinite(origin)) || !all(isfinite(segment)) || !isfinite(maximum) || !(maximum > 0.0f))
                    {
                        CausticInvalidMeasurement(attempt);
                        return;
                    }

                    const float3 cameraDirection = toCamera / cameraDistance;
                    const CausticHit seen = CausticTrace(origin, segment / max(maximum, 1.0e-20f), maximum);


                    if(CausticIs(seen, receiverDense, receiverNative) && abs(seen.t - expected) <= 1.0e-4f * max(expected, 1.0f) &&
                        CausticBuildSurface(receiver, cameraDirection, surface))
                    {
                        const float axial = dot(-cameraDirection, cameraForward);
                        const float jacobian = jacobianScale / (axial * axial * axial);
                        const uint2 coordinate = min(uint2(uv * float2(resolution)), resolution - 1u);
                        pixel = coordinate.y * resolution.x + coordinate.x;
                        contribution = throughput * PathTraceEvaluateBSDF(surface, -sampled.direction) *
                            float(resolution.x) * float(resolution.y) * jacobian * abs(dot(surface.normal, cameraDirection)) /
                            (cameraDistance * cameraDistance);
                    }
                }
            }
        }
    }

    // Nonfinite or negative Z is stored as-is: the accumulator flags it and invalidates the frame.
    records.Store4(attempt * 16u, uint4(pixel, asuint(contribution)));
}
