// R.9I.3 P5D.2A raster identity audit (verification only). For every pixel, compares the G-buffer raster identity
// (GBuffer::IdentityFormat: InstanceID + 1, primitive; 0 background; all ones = no exact mapping) with the
// pixel-centre TLAS ray's committed hit, exactly as CausticPixel.hlsl's CentreRay casts it. Nothing is substituted:
// each pixel lands in exactly one category and the first disagreements are recorded verbatim.
//
// CSCertification (after the estimator publishes) re-classifies every pixel the same way and reads the estimator's
// per-pixel state (diffuse output alpha): word 10 counts certified pixels (states 3, 4, 5), word 11 certified pixels
// whose raster identity does not exactly agree with the TLAS hit (must be zero), word 12 certified agreeing pixels.
// Output (raw UAV): words 0..7 category counts, word 8 record count, then records of 8 words from word 16:
//   pixel, raster.x, raster.y, hit InstanceID, hit InstanceIndex, hit primitive, hit status, category.
// Categories: 0 both background, 1 agree, 2 raster has no exact mapping, 3 both hit but identity differs,
//             4 raster background but TLAS hit, 5 raster identity but TLAS miss.

#include "PathTraceBSDF.hlsli"
#include "SceneLights.hlsli"
#include "PathTracePrimarySurface.hlsli"

RaytracingAccelerationStructure Scene : register(t0);

cbuffer AuditConstants : register(b0)
{
    row_major float4x4 inverseCameraRelativeViewProjection;
    float3 cameraPosition;
    uint identityIndex;
    uint outputIndex;
    uint recordCapacity;
    uint2 resolution;
    uint diffuseIndex;
    uint3 padding;
};

static const uint NO_HIT = 0xFFFFFFFFu;
static const uint INVALID_IDENTITY = 0xFFFFFFFFu;
static const uint RECORD_BASE = 16;

[numthreads(1, 1, 1)]
void CSClear()
{
    RWByteAddressBuffer output = ResourceDescriptorHeap[outputIndex];

    for(uint word = 0; word < RECORD_BASE + recordCapacity * 8; ++word)
    {
        output.Store(word * 4, 0);
    }
}

uint Classify(uint2 pixel, out uint2 raster, out uint instanceID, out uint instanceIndex, out uint primitive,
    out uint status)
{
    PathTracePrimaryCamera camera;
    camera.inverseCameraRelativeViewProjection = inverseCameraRelativeViewProjection;
    camera.cameraPosition = cameraPosition;
    camera.resolution = resolution;

    RayDesc ray;
    ray.Origin = cameraPosition;
    ray.Direction = normalize(PathTraceUnprojectCameraRelative(pixel, 0.5f, camera));
    ray.TMin = 0;
    ray.TMax = 1e30f;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(Scene, RAY_FLAG_NONE, 255, ray);
    q.Proceed();
    status = q.CommittedStatus();
    const bool hit = status == COMMITTED_TRIANGLE_HIT;
    instanceID = hit ? q.CommittedInstanceID() : NO_HIT;
    instanceIndex = hit ? q.CommittedInstanceIndex() : NO_HIT;
    primitive = hit ? q.CommittedPrimitiveIndex() : NO_HIT;

    Texture2D<uint2> identity = ResourceDescriptorHeap[identityIndex];
    raster = identity.Load(int3(pixel, 0));

    if(raster.x == INVALID_IDENTITY)
    {
        return 2;
    }

    if(raster.x == 0)
    {
        return hit ? 4 : 0;
    }

    if(!hit)
    {
        return 5;
    }

    return raster.x - 1 == instanceID && raster.y == primitive ? 1 : 3;
}

[numthreads(8, 8, 1)]
void CSCertification(uint3 id : SV_DispatchThreadID)
{
    if(id.x >= resolution.x || id.y >= resolution.y)
    {
        return;
    }

    uint2 raster;
    uint instanceID, instanceIndex, primitive, status;
    const uint category = Classify(id.xy, raster, instanceID, instanceIndex, primitive, status);
    Texture2D<float4> diffuse = ResourceDescriptorHeap[diffuseIndex];
    const float state = diffuse.Load(int3(id.xy, 0)).w;

    if(state == 3.0f || state == 4.0f || state == 5.0f)
    {
        RWByteAddressBuffer output = ResourceDescriptorHeap[outputIndex];
        output.InterlockedAdd(10 * 4, 1);
        output.InterlockedAdd((category == 1 ? 12 : 11) * 4, 1);
    }
}

[numthreads(8, 8, 1)]
void CSAudit(uint3 id : SV_DispatchThreadID)
{
    if(id.x >= resolution.x || id.y >= resolution.y)
    {
        return;
    }

    PathTracePrimaryCamera camera;
    camera.inverseCameraRelativeViewProjection = inverseCameraRelativeViewProjection;
    camera.cameraPosition = cameraPosition;
    camera.resolution = resolution;

    RayDesc ray;
    ray.Origin = cameraPosition;
    ray.Direction = normalize(PathTraceUnprojectCameraRelative(id.xy, 0.5f, camera));
    ray.TMin = 0;
    ray.TMax = 1e30f;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(Scene, RAY_FLAG_NONE, 255, ray);
    q.Proceed();
    const uint status = q.CommittedStatus();
    const bool hit = status == COMMITTED_TRIANGLE_HIT;
    const uint instanceID = hit ? q.CommittedInstanceID() : NO_HIT;
    const uint instanceIndex = hit ? q.CommittedInstanceIndex() : NO_HIT;
    const uint primitive = hit ? q.CommittedPrimitiveIndex() : NO_HIT;

    Texture2D<uint2> identity = ResourceDescriptorHeap[identityIndex];
    const uint2 raster = identity.Load(int3(id.xy, 0));
    const bool background = raster.x == 0;
    const bool unmapped = raster.x == INVALID_IDENTITY;
    uint category;

    if(unmapped)
    {
        category = 2;
    }
    else if(background)
    {
        category = hit ? 4 : 0;
    }
    else if(!hit)
    {
        category = 5;
    }
    else
    {
        category = raster.x - 1 == instanceID && raster.y == primitive ? 1 : 3;
    }

    RWByteAddressBuffer output = ResourceDescriptorHeap[outputIndex];
    output.InterlockedAdd(category * 4, 1);

    if(category >= 2)
    {
        uint slot;
        output.InterlockedAdd(8 * 4, 1, slot);

        if(slot < recordCapacity)
        {
            const uint base = (RECORD_BASE + slot * 8) * 4;
            output.Store4(base, uint4(id.y * resolution.x + id.x, raster.x, raster.y, instanceID));
            output.Store4(base + 16, uint4(instanceIndex, primitive, status, category));
        }
    }
}
