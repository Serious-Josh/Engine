// R.9I.3 production pixel-centred caustics estimator (PixelCausticPass), Raster Primary + ReSTIR DI.
// The validated B3-T bounded tiled estimator (src/Renderer/tests/CausticPointGPU/TTiled.hlsl, TKernels.hlsli,
// TProposal.hlsli, TCauchyMath.hlsli; R9I3PixelCentredCausticsGPUPerformance.md B3-T) with its raster-receiver
// variant always selected. The proposal, sampling, PDF, visibility, BSDF evaluation and FP64 reduction are
// unchanged. Production differences: no planar/analytic receiver path or raster-tuple kernel; Publish writes two
// RGBA32F lobe textures instead of the 64-byte diagnostic record; probe capture and fault injection exist only in
// the verification variant (CausticPixelProbes.hlsl, PIXEL_CAUSTIC_PROBES).
//
// Output contract (PixelCausticPass.h): diffuse = (receiver diffuse-lobe radiance, state), specular = (receiver
// specular-lobe radiance, fault bits). States: 0 background, 1 uncertified, 3 unscheduled, 4 accepted zero,
// 5 accepted nonzero, 6 fault. A fault publishes both lobes zero. The fault latch (16 bytes) holds the OR of every
// fault, the faulting receiver count, the first faulting pixel and this frame's epoch.
#include "PathTraceBSDF.hlsli"
#include "SceneLights.hlsli"
#include "PathTracePrimarySurface.hlsli"

RaytracingAccelerationStructure Scene : register(t0);
// Accepted source triangle in world space, as uploaded (PixelCausticTriangle, 80 bytes). The reflector
// identity is exact uint32: TLAS InstanceID, native InstanceIndex and material table index.
struct TriangleRecord
{
    float3 a;
    float area;
    float3 b;
    float areaCumulative;
    float3 c;
    uint primitive;
    uint instanceID;
    uint instanceIndex;
    uint material;
    uint reserved[5];
};
// Targeted parameters live in the triangle's own parallelogram coordinates:
// y = a + s (b - a) + t (c - a), the triangle being s, t >= 0, s + t <= 1.
struct MeshTriangle
{
    float3 a;
    float area;
    float3 b;
    float areaCumulative;
    float3 c;
    uint primitive;
    float2 centre;
    float2 scale;
    float weight;
    float weightCumulative;
    uint instanceID;
    uint instanceIndex;
    uint material;
};
StructuredBuffer<TriangleRecord> Geometry : register(t1);
StructuredBuffer<GPUMaterial> Materials : register(t2);
// P5D.2B automatic receivers, indexed by TLAS InstanceID: {eligible, native InstanceIndex, material index, 0}.
StructuredBuffer<uint4> ReceiverTable : register(t3);
cbuffer Constants : register(b0)
{
    uint attempts;
    uint seed;
    uint proposal;
    uint triangleCount;
    uint reflectorMaterial;
    uint receiverMaterial;
    uint energyTable;
    uint averageTable;
    uint mapping;
    uint outputIndex;
    uint reflectorDense;
    uint reflectorNative;
    uint receiverDense;
    uint receiverNative;
    uint epoch;
    uint expectedEpoch;
    float3 receiverPoint;
    float totalArea;
    float3 receiverView;
    // P5D.2B: 1 when receivers are certified by exact raster/TLAS identity against ReceiverTable.
    uint automaticReceivers;
    float broadFraction;
    uint receiverTableCount;
    float2 unused;
    GPULight pointLight;
};

static const uint NO_HIT = 0xFFFFFFFFu;

// Engine's frame camera (this recording's raster-primary camera) and G-buffer views.
cbuffer RasterConstants : register(b1)
{
    row_major float4x4 inverseCameraRelativeViewProjection;
    float3 cameraPosition;
    uint depthIndex;
    uint materialTextureIndex;
    uint surfaceIndex;
    uint geometricNormalIndex;
    uint tupleOutputIndex;
    uint2 resolution;
    uint2 probePixel;
    // Heap SRV of the snapshot's material table: Engine's decode sizes it with GetDimensions, which a root
    // SRV cannot answer.
    uint materialTableIndex;
    // P5D.2A raster identity target (GBuffer::IdentityFormat): (InstanceID + 1, primitive); 0 background; all ones
    // when the draw had no exact TLAS mapping.
    uint identityIndex;
};

static float3 g_receiverPoint;
#define RECEIVER_POINT g_receiverPoint
// Reconstructed (decoded, lifted, finalized) once per thread by Engine's PathTraceReconstructPrimaryVertex.
static PathTraceSurface g_receiverSurface;
#define RECEIVER_SURFACE g_receiverSurface
#define FINALIZE_RECEIVER(s) true

cbuffer TileConstants : register(b2)
{
    uint tileStart;
    uint tileCount;
    uint sampleCount;
    uint targetsIndex;
    uint scratchIndex;
    uint diffuseOutputIndex;
    uint probesIndex;
    uint tupleIndex;
    uint scheduleDivisor;
    uint injectFlags;
    uint captureProbes;
    uint tileCapacity;
    uint4 probePixels0;
    uint4 probePixels1;
    uint faultIndex;
    uint firstPixel;
    uint specularOutputIndex;
    uint probeOutputIndex;
};

static uint g_local;
static float g_broad;
static uint g_lightQueries, g_receiverQueries;
MeshTriangle ReadTriangle(uint k)
{
    TriangleRecord record = Geometry[k];
    MeshTriangle t = (MeshTriangle)0;
    t.a = record.a;
    t.area = record.area;
    t.b = record.b;
    t.areaCumulative = record.areaCumulative;
    t.c = record.c;
    t.primitive = record.primitive;
    t.instanceID = record.instanceID;
    t.instanceIndex = record.instanceIndex;
    t.material = record.material;
    return t;
}

MeshTriangle LoadTriangle(uint k)
{
    MeshTriangle t = ReadTriangle(k);
    RWByteAddressBuffer targets = ResourceDescriptorHeap[targetsIndex];
    uint address = (g_local * triangleCount + k) * 32;
    t.centre = asfloat(targets.Load2(address));
    t.scale = asfloat(targets.Load2(address + 8));
    t.weight = asfloat(targets.Load(address + 16));
    t.weightCumulative = asfloat(targets.Load(address + 20));
    return t;
}

#ifdef PIXEL_CAUSTIC_PROBES
uint ProbeSlot(uint pixel)
{
    for(uint k = 0; k < 8; ++k)
    {
        uint selected = k < 4 ? probePixels0[k] : probePixels1[k - 4];

        if(selected == pixel)
        {
            return k;
        }
    }

    return NO_HIT;
}
#endif

#include "CausticPixelCauchy.hlsli"

uint Hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    return x ^ (x >> 16);
}

float Uniform(inout uint x)
{
    x = Hash(x);
    return (x >> 8) * (1.0f / 16777216.0f);
}

float3 Offset(float3 p, float3 n)
{
    int3 i = int3(256.0f * n);
    float3 shifted = asfloat(asint(p) + select(p < 0, -i, i));
    return select(abs(p) < 1.0f / 32.0f, p + n / 65536.0f, shifted);
}

// A1's acceptance (reflector instance at the expected distance) plus first-hit triangle identity: on a
// closed mesh a back-face sample within the distance tolerance of an edge is otherwise accepted through
// the adjacent front face.
bool VisibleLight(float3 samplePosition, float expected, MeshTriangle tri, out uint primitive)
{
    RayDesc ray;
    ray.Origin = pointLight.position;
    ray.Direction = normalize(samplePosition - ray.Origin);
    ray.TMin = 0;
    ray.TMax = expected + 1e-4f * max(expected, 1.0f);
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(Scene, RAY_FLAG_NONE, 255, ray);
    q.Proceed();
    primitive = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedPrimitiveIndex() : NO_HIT;
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT && q.CommittedInstanceID() == tri.instanceID &&
           q.CommittedInstanceIndex() == tri.instanceIndex && primitive == tri.primitive &&
           abs(q.CommittedRayT() - expected) <= 1e-4f * max(expected, 1.0f);
}

bool VisibleReceiver(float3 samplePosition, float3 normal, out uint instance)
{
    RayDesc ray;
    ray.Origin = Offset(samplePosition, normal);
    float3 segment = RECEIVER_POINT - ray.Origin;
    float distance = length(segment);
    ray.Direction = segment / distance;
    ray.TMin = 0;
    ray.TMax = distance * (1.0f - 1e-5f);
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(Scene, RAY_FLAG_NONE, 255, ray);
    q.Proceed();
    instance = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedInstanceIndex() : NO_HIT;
    return q.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

PathTraceSurface Surface(GPUMaterial material, float3 normal, float3 view)
{
    PathTraceSurface s = (PathTraceSurface)0;
    s.material = material;
    s.baseColor = material.baseColor;
    s.view = view;
    s.normal = normal;
    s.geometricNormal = normal;
    s.materialTableIndex = energyTable;
    s.materialAverageTableIndex = averageTable;
    s.materialTableCosineMapping = mapping;

    if(dot(normal, view) < 0)
    {
        s.normal = -normal;
        s.geometricNormal = -normal;
    }

    return s;
}

// Truncated Cauchy on [0, 1] per axis, centred on the triangle's planar mirror point.
float TargetDensity(MeshTriangle t, float2 st)
{
    return TDensity(t.centre, t.scale, st);
}

uint SelectByArea(float u)
{
    float choose = u * totalArea;
    uint selected = 0;

    for(uint i = 0; i < triangleCount; i++)
    {
        if(choose >= LoadTriangle(i).areaCumulative)
        {
            selected = min(i + 1, triangleCount - 1);
        }
    }

    return selected;
}

uint SelectByWeight(float u)
{
    uint selected = 0;

    for(uint i = 0; i < triangleCount; i++)
    {
        if(u >= LoadTriangle(i).weightCumulative)
        {
            selected = min(i + 1, triangleCount - 1);
        }
    }

    return selected;
}

PathTracePrimaryCamera FrameCamera()
{
    PathTracePrimaryCamera camera;
    camera.inverseCameraRelativeViewProjection = inverseCameraRelativeViewProjection;
    camera.cameraPosition = cameraPosition;
    camera.resolution = resolution;
    return camera;
}

PathTraceGBufferSample LoadFrameGBuffer(uint2 pixel)
{
    Texture2D<float> depth = ResourceDescriptorHeap[depthIndex];
    Texture2D<float4> material = ResourceDescriptorHeap[materialTextureIndex];
    Texture2D<float4> surface = ResourceDescriptorHeap[surfaceIndex];
    Texture2D<float4> geometricNormal = ResourceDescriptorHeap[geometricNormalIndex];
    return PathTraceLoadGBuffer(pixel, depth, material, surface, geometricNormal);
}

// Independent identity source: the pixel-centre ray's committed TLAS hit. surface.w is only a material index.
struct CentreHit
{
    uint status;
    uint instanceID;
    uint instanceIndex;
    uint primitive;
    float t;
    float3 direction;
};

CentreHit CentreRay(uint2 pixel)
{
    CentreHit hit;
    hit.direction = normalize(PathTraceUnprojectCameraRelative(pixel, 0.5f, FrameCamera()));
    RayDesc ray;
    ray.Origin = cameraPosition;
    ray.Direction = hit.direction;
    ray.TMin = 0;
    ray.TMax = 1e30f;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(Scene, RAY_FLAG_NONE, 255, ray);
    q.Proceed();
    hit.status = q.CommittedStatus();
    bool committed = hit.status == COMMITTED_TRIANGLE_HIT;
    hit.instanceID = committed ? q.CommittedInstanceID() : NO_HIT;
    hit.instanceIndex = committed ? q.CommittedInstanceIndex() : NO_HIT;
    hit.primitive = committed ? q.CommittedPrimitiveIndex() : NO_HIT;
    hit.t = committed ? q.CommittedRayT() : 0;
    return hit;
}

#include "CausticPixelProposal.hlsli"
void Attempt(uint pixel, uint attemptID, out float3 diffuseValue, out float3 specularValue,
             out uint attemptFlags, out bool nullAttempt)
{
    uint batchSeed = seed + pixel;
    uint state = Hash(attemptID * 0x9E3779B9u ^ batchSeed);
    float4 random = float4(Uniform(state), Uniform(state), Uniform(state), Uniform(state));
    uint flags = 0, selected = 0, branch = 0;
#ifdef PIXEL_CAUSTIC_PROBES
    uint expectedForAttempt = expectedEpoch + ((pixel == probePixels0.x && (injectFlags & 16u)) ? 1u : 0u);
#else
    uint expectedForAttempt = expectedEpoch;
#endif
    float3 samplePosition = 0;
    float density = 0;
    float2 st = 0;

    if(epoch != expectedForAttempt || triangleCount == 0 || !(totalArea > 0) || !isfinite(totalArea) ||
       !(g_broad > 0) || g_broad > 1)
    {
        flags |= 1u;
    }

    if(flags == 0)
    {
        // U uses all of random.x for the area CDF; M splits random.x between its two branches.
        bool broad = proposal == 0 || random.x < g_broad;

        if(broad)
        {
            selected = SelectByArea(proposal == 0 ? random.x : random.x / g_broad);
            float r = sqrt(random.y);
            st = float2(r * (1 - random.z), r * random.z);
        }
        else
        {
            branch = 1;
            selected = SelectByWeight((random.x - g_broad) / (1 - g_broad));
            MeshTriangle t = LoadTriangle(selected);
            st = TSample(t.centre, t.scale, random.yz);

            if(any(t.scale <= 0) || !all(isfinite(t.scale)) || !(t.weight > 0))
            {
                flags |= 2u;
            }
        }

        MeshTriangle t = LoadTriangle(selected);
        samplePosition = t.a + st.x * (t.b - t.a) + st.y * (t.c - t.a);
        float targeted = t.weight * TargetDensity(t, st) / (2 * t.area);
        density = proposal == 0 ? 1 / totalArea : g_broad / totalArea + (1 - g_broad) * targeted;
        // A targeted point outside the open triangle is a null attempt kept in the denominator, never
        // resampled. The boundary has zero measure; only tan rounding lands on or past it, where a light ray
        // toward a cube edge cannot certify which face it hit.
        if(branch == 1 && (st.x <= 0 || st.y <= 0 || st.x + st.y >= 1))
        {
            branch = 2;
        }
    }

#ifdef PIXEL_CAUSTIC_PROBES
    if(pixel == probePixels0.x && (injectFlags & 8u))
    {
        density = 0;
    }
#endif

    if(!(density > 0) || !isfinite(density) || !all(isfinite(samplePosition)))
    {
        flags |= 2u;
    }

    float3 d = 0, s = 0;
    uint visible = 0, lightPrimitive = NO_HIT, receiverBlocker = NO_HIT;

    if(flags == 0 && branch != 2)
    {
        MeshTriangle tri = LoadTriangle(selected);
        float3 ng = normalize(cross(tri.b - tri.a, tri.c - tri.a));
        float3 toLight = pointLight.position - samplePosition;
        float dl = length(toLight);
        float3 toReceiver = RECEIVER_POINT - samplePosition;
        float dr = length(toReceiver);

        if(!(dl > 0 && dr > 0) || !isfinite(dl) || !isfinite(dr))
        {
            flags |= 4u;
        }
        else
        {
            float3 wl = toLight / dl, wr = toReceiver / dr;
            PathTraceSurface reflector = Surface(Materials[tri.material], ng, wl);
            PathTraceSurface receiver = RECEIVER_SURFACE;

            if(PathTraceFinalizeSurface(reflector) && FINALIZE_RECEIVER(receiver) &&
               PathTraceDirectionAboveSurface(reflector, wr) && PathTraceDirectionAboveSurface(receiver, -wr))
            {
                ++g_lightQueries;
                visible = VisibleLight(samplePosition, dl, tri, lightPrimitive) ? 1u : 0u;

                if(visible != 0)
                {
                    ++g_receiverQueries;

                    if(VisibleReceiver(samplePosition, reflector.geometricNormal, receiverBlocker))
                    {
                        visible |= 2u;
                    }
                }

                if(visible == 3u)
                {
                    float3 lightDirection;
                    float3 incident = EvaluateSceneLightIncidence(pointLight, samplePosition, lightDirection);
                    float3 reflected = PathTraceEvaluateBSDF(reflector, wr) * incident * abs(dot(ng, wl));
                    float3 diffuse;
                    float3 full = PathTraceEvaluateBSDFLobes(receiver, -wr, diffuse);
                    float geometry =
                        abs(dot(ng, wr)) * abs(dot(receiver.geometricNormal, -wr)) / (dr * dr * density);
                    d = diffuse * reflected * geometry;
                    s = (full - diffuse) * reflected * geometry;

                    if(!all(isfinite(d)) || !all(isfinite(s)) || any(d < 0) || any(s < 0))
                    {
                        flags |= 8u;
                    }
                }
            }
        }
    }

#ifdef PIXEL_CAUSTIC_PROBES
    if(pixel == probePixels0.x)
    {
        if(injectFlags & 1u)
        {
            d.x = asfloat(0x7fc00000u);
        }

        if(injectFlags & 2u)
        {
            s.y = -1;
        }

        if(injectFlags & 4u)
        {
            d.z = 3e38f;
        }

        if(injectFlags & 32u)
        {
            float v = attemptID % 4 == 0
                          ? 3e4f
                          : (attemptID % 4 == 1 ? 1e-12f : (attemptID % 4 == 2 ? 1e-4f : 1.0f));
            d = float3(v, v * 0.1f, v * 0.01f);
            s = d * 0.02f;
        }
    }
#endif

    if(!all(isfinite(d)) || !all(isfinite(s)) || any(d < 0) || any(s < 0))
    {
        flags |= 8u;
    }

#ifdef PIXEL_CAUSTIC_PROBES
    if(captureProbes && ProbeSlot(pixel) != NO_HIT)
    {
        RWByteAddressBuffer output = ResourceDescriptorHeap[probesIndex];
        uint offset = (ProbeSlot(pixel) * 16 + attemptID) * 96;
        output.Store4(offset, asuint(float4(samplePosition, density)));
        output.Store4(offset + 16, uint4(asuint(d), lightPrimitive));
        output.Store4(offset + 32, uint4(asuint(s), receiverBlocker));
        output.Store4(offset + 48, uint4(branch, selected, visible, flags));
        output.Store4(offset + 64, asuint(random));
        output.Store4(offset + 80, uint4(epoch, expectedForAttempt, batchSeed, attemptID));
    }
#endif

    diffuseValue = d;
    specularValue = s;
    attemptFlags = flags;
    nullAttempt = (branch == 2 || visible != 3);
}

// All four stages own one distinct image pixel per lane. No full-image proposal table.
[numthreads(64, 1, 1)] void CSClassify(uint3 id : SV_DispatchThreadID)
{
    if(id.x >= tileCount)
    {
        return;
    }

    if(id.x == 0 && tileStart == firstPixel)
    {
        RWByteAddressBuffer faults = ResourceDescriptorHeap[faultIndex];
        faults.Store4(0, uint4(0, 0, NO_HIT, epoch));
    }

    uint pixel = tileStart + id.x;
    uint2 xy = uint2(pixel % resolution.x, pixel / resolution.x);
    RWByteAddressBuffer scratch = ResourceDescriptorHeap[scratchIndex];
    uint offset = id.x * 64;

    for(uint k = 0; k < 4; ++k)
    {
        scratch.Store4(offset + 16 * k, 0);
    }

    PathTraceGBufferSample gb = LoadFrameGBuffer(xy);

#ifdef PIXEL_CAUSTIC_PROBES
    if((injectFlags & 64u) && pixel == probePixels0.x)
    {
        gb.depth = asfloat(0x7fc00000u);
    }
#endif

    StructuredBuffer<GPUMaterial> table = ResourceDescriptorHeap[materialTableIndex];
    PathTracePrimaryVertex vertex = (PathTracePrimaryVertex)0;
    bool decoded = PathTraceReconstructPrimaryVertex(xy, gb, FrameCamera(), table, energyTable, averageTable,
                                                     mapping, vertex);
    uint state = 0, eligible = decoded ? 1u : 0u, certified = 0;
    uint primaryFault =
        (!isfinite(gb.depth) || gb.depth < 0 || gb.depth > 1 || (!decoded && gb.depth < 1)) ? 512u : 0u;

    if(primaryFault != 0)
    {
        state = 6;
    }

    if(decoded)
    {
        CentreHit hit = CentreRay(xy);
        float3 preLift = PathTraceUnprojectCameraRelative(xy, gb.depth, FrameCamera());
        const bool depthAgrees = length(hit.direction * hit.t - preLift) <= 1e-4f * length(preLift);

        if(automaticReceivers == 0)
        {
            certified = (hit.status == COMMITTED_TRIANGLE_HIT && hit.instanceID == receiverDense &&
                         hit.instanceIndex == receiverNative && depthAgrees)
                            ? 1u
                            : 0u;
        }
        else
        {
            // P5D.2B: the pixel's rasterized fragment and the centre ray's TLAS hit must be the same eligible
            // receiver triangle (exact InstanceID, InstanceIndex, primitive and material index) at the same depth.
            // A disagreement only leaves this pixel uncertified; a TLAS instance outside the table is a broken
            // binding and faults the whole frame (bit 1024).
            const bool hitTriangle = hit.status == COMMITTED_TRIANGLE_HIT;

            if(hitTriangle && hit.instanceID >= receiverTableCount)
            {
                primaryFault |= 1024u;
            }

            const uint4 entry = hitTriangle && hit.instanceID < receiverTableCount ? ReceiverTable[hit.instanceID] : uint4(0, 0, 0, 0);
            Texture2D<uint2> identity = ResourceDescriptorHeap[identityIndex];
            const uint2 raster = identity.Load(int3(xy, 0));
            certified = (entry.x == 1 && entry.y == hit.instanceIndex && raster.x == hit.instanceID + 1 &&
                         raster.y == hit.primitive && vertex.materialIndex == entry.z && depthAgrees)
                            ? 1u
                            : 0u;
        }
        state = primaryFault != 0 ? 6u : certified != 0 ? (pixel % scheduleDivisor == 0 ? 4u : 3u) : 1u;
        scratch.Store3(offset, asuint(vertex.position));
    }

    scratch.Store4(offset + 32, uint4(state, primaryFault, eligible, certified));

#ifdef PIXEL_CAUSTIC_PROBES
    uint slot = ProbeSlot(pixel);

    if(captureProbes && slot != NO_HIT)
    {
        RWByteAddressBuffer tuples = ResourceDescriptorHeap[tupleIndex];
        PathTracePrimaryVertex pre = (PathTracePrimaryVertex)0;
        bool ok = PathTraceDecodePrimaryVertex(xy, gb, FrameCamera(), table, energyTable, averageTable,
                                               mapping, pre);
        CentreHit hit = CentreRay(xy);
        uint a = slot * 192;
        tuples.Store4(a, uint4(asuint(gb.depth), ok ? 1u : 0u, decoded ? 1u : 0u, pre.materialIndex));
        tuples.Store4(a + 16, asuint(gb.material));
        tuples.Store4(a + 32, asuint(gb.surface));
        tuples.Store4(a + 48, asuint(gb.geometricNormal));
        tuples.Store4(a + 64, asuint(float4(pre.cameraRelativePosition, 0)));
        tuples.Store4(a + 80, asuint(float4(vertex.position, 0)));
        tuples.Store4(a + 96, asuint(float4(vertex.surface.normal, vertex.surface.material.metallic)));
        tuples.Store4(a + 112,
                      asuint(float4(vertex.surface.geometricNormal, vertex.surface.material.roughness)));
        tuples.Store4(a + 128, uint4(asuint(vertex.surface.baseColor), vertex.surface.material.model));
        tuples.Store4(a + 144, asuint(float4(vertex.surface.view, hit.t)));
        tuples.Store4(a + 160, uint4(hit.instanceID, hit.instanceIndex, hit.primitive, hit.status));
        tuples.Store4(a + 176, asuint(float4(hit.direction, 0)));
    }
#endif
}

[numthreads(64, 1, 1)] void CSSetup(uint3 id : SV_DispatchThreadID)
{
    if(id.x >= tileCount)
    {
        return;
    }

    RWByteAddressBuffer scratch = ResourceDescriptorHeap[scratchIndex];

    if(scratch.Load(id.x * 64 + 32) != 4)
    {
        return;
    }

    RWByteAddressBuffer targets = ResourceDescriptorHeap[targetsIndex];
    float3 x = asfloat(scratch.Load3(id.x * 64));
    double sum = 0;
    uint flags = 0;

    for(uint k = 0; k < triangleCount; ++k)
    {
        Target t = TargetFor(ReadTriangle(k), x);
        sum += t.weight;
        uint addr = (id.x * triangleCount + k) * 32;
        float2 centre = (float2)t.centre, scale = (float2)t.scale;

        if(!all(isfinite(centre)) || !all(isfinite(scale)) || any(scale <= 0) || !isfinite((float)t.weight) ||
           t.weight < 0)
        {
            flags |= 2u;
        }

        targets.Store4(addr, asuint(float4(centre, scale)));
        uint lo, hi;
        asuint(t.weight, lo, hi);
        targets.Store2(addr + 24, uint2(lo, hi));
    }

    double cumulative = 0;

    for(uint j = 0; j < triangleCount; ++j)
    {
        uint addr = (id.x * triangleCount + j) * 32;
        uint2 bits = targets.Load2(addr + 24);
        double raw = asdouble(bits.x, bits.y);
        double w = sum > 0 ? raw / sum : 0;
        cumulative += w;
        targets.Store2(addr + 16, asuint(float2((float)w, (float)cumulative)));
    }

    scratch.Store(id.x * 64 + 12, asuint(sum > 0 ? 0.1f : 1.0f));
    scratch.Store(id.x * 64 + 36, flags);
}

[numthreads(64, 1, 1)] void CSSampleReduce(uint3 id : SV_DispatchThreadID)
{
    if(id.x >= tileCount)
    {
        return;
    }

    g_local = id.x;
    uint pixel = tileStart + id.x;
    RWByteAddressBuffer scratch = ResourceDescriptorHeap[scratchIndex];
    uint offset = id.x * 64;
    uint state = scratch.Load(offset + 32);

    if(state != 4)
    {
        return;
    }

    uint flags = scratch.Load(offset + 36);
    uint2 xy = uint2(pixel % resolution.x, pixel / resolution.x);
    PathTraceGBufferSample gb = LoadFrameGBuffer(xy);
    StructuredBuffer<GPUMaterial> table = ResourceDescriptorHeap[materialTableIndex];
    PathTracePrimaryVertex vertex = (PathTracePrimaryVertex)0;

    if(!PathTraceReconstructPrimaryVertex(xy, gb, FrameCamera(), table, energyTable, averageTable, mapping,
                                          vertex))
    {
        flags |= 16u;
    }

    g_receiverPoint = vertex.position;
    g_receiverSurface = vertex.surface;
    g_broad = asfloat(scratch.Load(offset + 12));
    g_lightQueries = 0;
    g_receiverQueries = 0;
    double3 diffuseSum = 0, specularSum = 0;
    uint nulls = 0;

    if(flags == 0)
    {
        for(uint k = 0; k < sampleCount; ++k)
        {
            float3 d, s;
            uint f;
            bool isNull;
            Attempt(pixel, k, d, s, f, isNull);
            flags |= f;
            nulls += isNull ? 1u : 0u;

            if(f == 0)
            {
                diffuseSum += (double3)d;
                specularSum += (double3)s;
            }
        }
    }

    float3 d = (float3)(diffuseSum / (double)sampleCount), s = (float3)(specularSum / (double)sampleCount);

    if(!all(isfinite(d)) || !all(isfinite(s)) || any(d < 0) || any(s < 0) || any(d > 65504) || any(s > 65504))
    {
        flags |= 64u;
    }

    scratch.Store4(offset, uint4(asuint(d), epoch));
    scratch.Store4(offset + 16, uint4(asuint(s), sampleCount));
    scratch.Store(offset + 36, flags);
    scratch.Store4(offset + 48, uint4(sampleCount, nulls, g_lightQueries, g_receiverQueries));
}

[numthreads(64, 1, 1)] void CSPublish(uint3 id : SV_DispatchThreadID)
{
    if(id.x >= tileCount)
    {
        return;
    }

    RWByteAddressBuffer scratch = ResourceDescriptorHeap[scratchIndex];
    uint src = id.x * 64;
    uint4 meta = scratch.Load4(src + 32), counts = scratch.Load4(src + 48);
    float3 d = 0, s = 0;
    uint acceptedEpoch = 0, k = 0;

    if(meta.x == 4)
    {
        uint4 a = scratch.Load4(src), b = scratch.Load4(src + 16);
        k = sampleCount;

        if(epoch != expectedEpoch || a.w != epoch || b.w != sampleCount || sampleCount == 0)
        {
            meta.y |= 1u;
        }

        if(meta.y == 0)
        {
            d = asfloat(a.xyz);
            s = asfloat(b.xyz);
            acceptedEpoch = epoch;
            meta.x = any(d > 0) || any(s > 0) ? 5u : 4u;
        }
        else
        {
            meta.x = 6;
        }
    }

    if(meta.x == 6)
    {
        RWByteAddressBuffer faults = ResourceDescriptorHeap[faultIndex];
        uint old;
        faults.InterlockedOr(0, meta.y, old);
        faults.InterlockedAdd(4, 1, old);
        faults.InterlockedMin(8, tileStart + id.x, old);
    }

    // Both lobes, or neither: a fault publishes zero radiance in both with state 6.
    uint pixel = tileStart + id.x;
    uint2 xy = uint2(pixel % resolution.x, pixel / resolution.x);
    RWTexture2D<float4> diffuseOutput = ResourceDescriptorHeap[diffuseOutputIndex];
    RWTexture2D<float4> specularOutput = ResourceDescriptorHeap[specularOutputIndex];
    diffuseOutput[xy] = float4(d, (float)meta.x);
    specularOutput[xy] = float4(s, (float)meta.y);

#ifdef PIXEL_CAUSTIC_PROBES
    // The B3-T 64-byte record of each selected probe, for the independent assessor.
    uint slot = ProbeSlot(pixel);

    if(captureProbes && slot != NO_HIT)
    {
        RWByteAddressBuffer output = ResourceDescriptorHeap[probeOutputIndex];
        uint dst = slot * 64;
        output.Store4(dst, uint4(asuint(d), acceptedEpoch));
        output.Store4(dst + 16, uint4(asuint(s), k));
        output.Store4(dst + 32, meta);
        output.Store4(dst + 48, counts);
    }
#endif
}
