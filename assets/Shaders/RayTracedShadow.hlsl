#include "PrimarySurfaceLift.hlsli"
#include "SurfaceDetail.hlsli"
#include "RayTracingInstanceData.hlsli"

struct ShadowPayload
{
    uint visible;
};

struct ShadowConstants
{
    row_major float4x4 inverseViewProjection;
    row_major float4x4 previousViewProjection;
    row_major float4x4 previousInverseViewProjection;

    float3 lightDirection;
    uint depthIndex;

    uint surfaceIndex;
    float lightAngularRadius;
    uint frameIndex;
    uint historyIndex;

    uint previousDepthIndex;
    uint outputDepthIndex;
    uint previousHistoryLengthIndex;
    uint outputHistoryLengthIndex;

    uint accumulatedFrameCount;
    uint geometricNormalIndex;
    // For a parallax pixel's self-shadowing (GetReliefVisibility): the ray tracing instance records and the materials.
    uint instanceDataIndex;
    uint materialBufferIndex;
};

RaytracingAccelerationStructure scene : register(t0);

RWTexture2D<float> visibilityTexture :
    register(u0);

ConstantBuffer<ShadowConstants> shadowConstants :
    register(b0);

// The G-buffer pass's material sampler (trilinear clamp), which the relief was marched with.
SamplerState reliefSampler :
    register(s2);

// The sampled direction to the sun already lies inside its disc, so the relief's own shadow needs no penumbra of its
// own: this small angle (about one degree) only hides the steps of the march.
static const float ReliefShadowSoftness = 0.02f;

float Random(
    uint2 pixelIndex,
    uint frameIndex)
{
    uint state =
        pixelIndex.x * 1973u +
        pixelIndex.y * 9277u +
        frameIndex * 26699u +
        911u;

    state =
        (state << 13u) ^
        state;

    uint value =
        state *
        (
            state * state * 15731u +
            789221u
        ) +
        1376312589u;

    return
        float(value & 0x00FFFFFFu) /
        float(0x01000000u);
}

float2 Random2(
    uint2 pixelIndex,
    uint frameIndex)
{
    return float2(
        Random(
            pixelIndex,
            frameIndex * 2u
        ),
        Random(
            pixelIndex,
            frameIndex * 2u + 1u
        )
    );
}

float3 SampleDirectionalLight(
    float3 directionToLight,
    float angularRadius,
    float2 randomSample)
{
    float cosThetaMax =
        cos(angularRadius);

    float cosTheta =
        lerp(
            1.0f,
            cosThetaMax,
            randomSample.x
        );

    float sinTheta =
        sqrt(
            max(
                0.0f,
                1.0f -
                cosTheta * cosTheta
            )
        );

    float phi =
        6.28318530718f *
        randomSample.y;

    float3 referenceAxis =
        abs(directionToLight.y) < 0.999f
            ? float3(0.0f, 1.0f, 0.0f)
            : float3(1.0f, 0.0f, 0.0f);

    float3 tangent =
        normalize(
            cross(
                referenceAxis,
                directionToLight
            )
        );

    float3 bitangent =
        cross(
            directionToLight,
            tangent
        );

    float3 sampledDirection =
        tangent *
            (cos(phi) * sinTheta) +
        bitangent *
            (sin(phi) * sinTheta) +
        directionToLight *
            cosTheta;

    return normalize(sampledDirection);
}

bool ReprojectWorldPosition(
    float3 worldPosition,
    uint2 dimensions,
    out float2 previousPixelPosition)
{
    float4 previousClipPosition =
        mul(
            shadowConstants.previousViewProjection,
            float4(
                worldPosition,
                1.0f
            )
        );

    if(previousClipPosition.w <= 0.0f)
    {
        previousPixelPosition =
            float2(0.0f, 0.0f);

        return false;
    }

    float3 previousNDC =
        previousClipPosition.xyz /
        previousClipPosition.w;

    if(
        previousNDC.z < 0.0f ||
        previousNDC.z > 1.0f)
    {
        previousPixelPosition =
            float2(0.0f, 0.0f);

        return false;
    }

    float2 previousUV;

    previousUV.x =
        previousNDC.x * 0.5f + 0.5f;

    previousUV.y =
        0.5f - previousNDC.y * 0.5f;

    if(
        previousUV.x < 0.0f ||
        previousUV.x >= 1.0f ||
        previousUV.y < 0.0f ||
        previousUV.y >= 1.0f)
    {
        previousPixelPosition =
            float2(0.0f, 0.0f);

        return false;
    }

    previousPixelPosition =
        previousUV *
        float2(dimensions) -
        0.5f;

    return true;
}

bool IsPixelInBounds(
    int2 pixelIndex,
    uint2 dimensions)
{
    return
        pixelIndex.x >= 0 &&
        pixelIndex.y >= 0 &&
        pixelIndex.x < int(dimensions.x) &&
        pixelIndex.y < int(dimensions.y);
}

float3 ReconstructPreviousWorldPosition(
    int2 pixelIndex,
    float depth,
    uint2 dimensions)
{
    float2 uv =
        (float2(pixelIndex) + 0.5f) /
        float2(dimensions);

    float2 ndc;

    ndc.x =
        uv.x * 2.0f - 1.0f;

    ndc.y =
        1.0f - uv.y * 2.0f;

    float4 clipPosition =
        float4(
            ndc,
            depth,
            1.0f
        );

    float4 worldPosition =
        mul(
            shadowConstants.previousInverseViewProjection,
            clipPosition
        );

    return
        worldPosition.xyz /
        worldPosition.w;
}

// The relief's own shadow on a parallax pixel, towards directionToLight (SurfaceDetailShellVisibility): 1 for an open
// view of the light, 0 where the relief hides it. The traced shadow ray starts on the mesh surface and never sees the
// relief below it.
//
// One short ray from the lifted origin straight down the normal finds the mesh triangle under it. The triangle's UV
// there is the relief point's UV, and the rest of the way down to reliefPosition (the point decoded from depth) is the
// relief's height. A ray that finds something else (a prop standing in the relief) leaves the pixel unshadowed here.
float GetReliefVisibility(uint2 pixel, uint2 dimensions, float depth, float3 reliefPosition, float3 geometricNormal,
    float surfaceLift, float3 directionToLight)
{
    // The pixel's material, as the G-buffer stores it.
    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[shadowConstants.surfaceIndex];
    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[shadowConstants.materialBufferIndex];
    uint materialCount;
    uint materialStride;
    materials.GetDimensions(materialCount, materialStride);
    float storedMaterial = surfaceTexture.Load(int3(pixel, 0)).a;

    if(!(storedMaterial >= 0.0f) || !(storedMaterial < float(materialCount)))
    {
        return 1.0f;
    }

    uint materialIndex = uint(storedMaterial);
    GPUMaterial material = materials[materialIndex];

    if(!SurfaceDetailUsesParallax(material))
    {
        return 1.0f;
    }

    // The mesh triangle under the lifted origin.
    const float probeStart = surfaceLift + 0.001f;
    RayDesc probe;
    probe.Origin = reliefPosition + geometricNormal * probeStart;
    probe.Direction = -geometricNormal;
    probe.TMin = 0.0f;
    probe.TMax = 0.002f + 0.5f * surfaceLift;

    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, probe);
    query.Proceed();

    if(query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return 1.0f;
    }

    StructuredBuffer<RayTracingInstanceData> instances = ResourceDescriptorHeap[shadowConstants.instanceDataIndex];
    RayTracingInstanceData instance = instances[query.CommittedInstanceID()];
    uint primitiveIndex = query.CommittedPrimitiveIndex();

    if(ResolveHitMaterial(instance, primitiveIndex) != materialIndex)
    {
        return 1.0f;
    }

    // The triangle's corners: positions and tangents to world space, UVs as authored.
    uint3 indices = uint3(primitiveIndex * 3u, primitiveIndex * 3u + 1u, primitiveIndex * 3u + 2u);

    if(instance.indexed != 0u)
    {
        ByteAddressBuffer indexBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(instance.indexBufferIndex)];
        indices = indexBuffer.Load3(primitiveIndex * 3u * 4u);
    }

    ByteAddressBuffer vertexBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(instance.vertexBufferIndex)];
    uint3 vertexOffsets = indices * instance.vertexStride;
    float3x3 objectToWorld = (float3x3)query.CommittedObjectToWorld3x4();
    float3 position0 = asfloat(vertexBuffer.Load3(vertexOffsets.x));
    float3 edge1 = mul(objectToWorld, asfloat(vertexBuffer.Load3(vertexOffsets.y)) - position0);
    float3 edge2 = mul(objectToWorld, asfloat(vertexBuffer.Load3(vertexOffsets.z)) - position0);
    float2 uv0 = asfloat(vertexBuffer.Load2(vertexOffsets.x + instance.uvOffset));
    float2 uvEdge1 = asfloat(vertexBuffer.Load2(vertexOffsets.y + instance.uvOffset)) - uv0;
    float2 uvEdge2 = asfloat(vertexBuffer.Load2(vertexOffsets.z + instance.uvOffset)) - uv0;
    float2 barycentrics = query.CommittedTriangleBarycentrics();

    // Without vertex tangents SurfaceDetailUVPerHeight solves the mapping from the edges alone.
    float4 tangent = float4(0.0f, 0.0f, 0.0f, 1.0f);

    if(instance.tangentOffset != 0xFFFFFFFFu)
    {
        float3 tangent0 = asfloat(vertexBuffer.Load3(vertexOffsets.x + instance.tangentOffset));
        float3 tangent1 = asfloat(vertexBuffer.Load3(vertexOffsets.y + instance.tangentOffset));
        float3 tangent2 = asfloat(vertexBuffer.Load3(vertexOffsets.z + instance.tangentOffset));
        tangent.xyz = mul(objectToWorld, tangent0 + (tangent1 - tangent0) * barycentrics.x +
            (tangent2 - tangent0) * barycentrics.y);
    }

    // The relief point: its UV, and how far under the mesh it lies along the normal.
    float2 hitUV = uv0 + uvEdge1 * barycentrics.x + uvEdge2 * barycentrics.y;
    float hitHeight = min(query.CommittedRayT() - probeStart, 0.0f);

    float2 lightUVPerHeight;
    float lightCosine;

    if(!SurfaceDetailUVPerHeight(geometricNormal, tangent, directionToLight, edge1, edge2, uvEdge1, uvEdge2,
        lightUVPerHeight, lightCosine))
    {
        // A light at the mesh surface's horizon reaches only the top of the relief (or the mapping is degenerate, and
        // then nothing below the top can be judged).
        return hitHeight < -1.0e-4f ? 0.0f : 1.0f;
    }

    // The pixel's size in UV, for the height mip: its width in the world over the triangle's world length per UV.
    float2 neighbourUV = (float2(pixel) + float2(1.5f, 0.5f)) / float2(dimensions);
    float4 neighbourPosition = mul(shadowConstants.inverseViewProjection,
        float4(neighbourUV.x * 2.0f - 1.0f, 1.0f - neighbourUV.y * 2.0f, depth, 1.0f));
    float pixelSize = length(neighbourPosition.xyz / neighbourPosition.w - reliefPosition);
    float worldAreaPerUVArea = length(cross(edge1, edge2)) /
        max(abs(uvEdge1.x * uvEdge2.y - uvEdge1.y * uvEdge2.x), 1.0e-20f);
    float uvFootprint = pixelSize * rsqrt(max(worldAreaPerUVArea, 1.0e-20f));

    float visibility = SurfaceDetailShellVisibility(SurfaceDetailHeightFieldOf(material), reliefSampler, hitUV,
        hitHeight, uvFootprint, lightUVPerHeight, lightCosine, ReliefShadowSoftness);

    return isfinite(visibility) ? visibility : 1.0f;
}

[shader("raygeneration")]
void RayGeneration()
{
    uint2 pixelIndex =
        DispatchRaysIndex().xy;

    uint2 dimensions =
        DispatchRaysDimensions().xy;

    Texture2D<float> depthTexture =
        ResourceDescriptorHeap[
            shadowConstants.depthIndex
        ];

    Texture2D<float4> geometricNormalTexture =
        ResourceDescriptorHeap[
            shadowConstants.geometricNormalIndex
        ];

    Texture2D<float> historyTexture =
        ResourceDescriptorHeap[
            shadowConstants.historyIndex
        ];

    Texture2D<float> previousDepthTexture =
        ResourceDescriptorHeap[
            shadowConstants.previousDepthIndex
        ];

    RWTexture2D<float> outputDepthTexture =
        ResourceDescriptorHeap[
            shadowConstants.outputDepthIndex
        ];

    Texture2D<float> previousHistoryLengthTexture =
        ResourceDescriptorHeap[
            shadowConstants.previousHistoryLengthIndex
        ];

    RWTexture2D<float> outputHistoryLengthTexture =
        ResourceDescriptorHeap[
            shadowConstants.outputHistoryLengthIndex
        ];

    float depth =
        depthTexture.Load(
            int3(
                pixelIndex,
                0
            )
        );

    outputDepthTexture[pixelIndex] =
        depth;

    if(depth >= 1.0f)
    {
        visibilityTexture[pixelIndex] =
            1.0f;

        outputHistoryLengthTexture[pixelIndex] =
            1.0f;

        return;
    }

    float2 uv =
        (float2(pixelIndex) + 0.5f) /
        float2(dimensions);

    float2 ndc;

    ndc.x =
        uv.x * 2.0f - 1.0f;

    ndc.y =
        1.0f - uv.y * 2.0f;

    float4 clipPosition =
        float4(
            ndc,
            depth,
            1.0f
        );

    float4 worldPosition =
        mul(
            shadowConstants.inverseViewProjection,
            clipPosition
        );

    worldPosition /=
        worldPosition.w;

    float3 storedGeometricNormal =
        geometricNormalTexture.Load(
            int3(
                pixelIndex,
                0
            )
        ).rgb;

    float3 geometricNormal =
        normalize(
            storedGeometricNormal
        );

    // A parallax pixel's depth is the relief's, under the mesh: its stored lift takes the origin back to the mesh
    // surface, or the mesh's own triangle would shadow it. 0 for every other pixel.
    float surfaceLift =
        PrimarySurfaceStoredLift(
            pixelIndex,
            dimensions,
            depth,
            worldPosition.xyz,
            storedGeometricNormal,
            shadowConstants.inverseViewProjection
        );

    float3 directionToLight =
        normalize(
            -shadowConstants.lightDirection
        );

    float2 randomSample =
        Random2(
            pixelIndex,
            shadowConstants.frameIndex
        );

    float3 sampledDirectionToLight =
        SampleDirectionalLight(
            directionToLight,
            shadowConstants.lightAngularRadius,
            randomSample
        );

    RayDesc ray;

    ray.Origin =
        worldPosition.xyz +
        geometricNormal * (0.001f + surfaceLift);

    ray.Direction =
        sampledDirectionToLight;

    ray.TMin =
        0.001f;

    ray.TMax =
        10000.0f;

    ShadowPayload payload;
    payload.visible = 0;

    TraceRay(
        scene,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
        0xFF,
        0,
        1,
        0,
        ray,
        payload
    );

    float currentVisibility =
        payload.visible != 0
            ? 1.0f
            : 0.0f;

    // A parallax pixel is also shadowed by its own relief, which the traced ray cannot see.
    if(surfaceLift > 0.0f)
    {
        currentVisibility *=
            GetReliefVisibility(
                pixelIndex,
                dimensions,
                depth,
                worldPosition.xyz,
                geometricNormal,
                surfaceLift,
                sampledDirectionToLight
            );
    }

    float accumulatedVisibility =
        currentVisibility;

    float historyLength =
        1.0f;

    if(shadowConstants.accumulatedFrameCount > 0)
    {
        float2 previousPixelPosition;

        bool reprojectionValid =
            ReprojectWorldPosition(
                worldPosition.xyz,
                dimensions,
                previousPixelPosition
            );

        if(reprojectionValid)
        {
            int2 basePixel =
                int2(
                    floor(
                        previousPixelPosition
                    )
                );

            float2 fraction =
                frac(
                    previousPixelPosition
                );

            float weights[4];

            weights[0] =
                (1.0f - fraction.x) *
                (1.0f - fraction.y);

            weights[1] =
                fraction.x *
                (1.0f - fraction.y);

            weights[2] =
                (1.0f - fraction.x) *
                fraction.y;

            weights[3] =
                fraction.x *
                fraction.y;

            int2 candidatePixels[4];

            candidatePixels[0] =
                basePixel +
                int2(0, 0);

            candidatePixels[1] =
                basePixel +
                int2(1, 0);

            candidatePixels[2] =
                basePixel +
                int2(0, 1);

            candidatePixels[3] =
                basePixel +
                int2(1, 1);

            float weightedVisibility =
                0.0f;

            float weightedHistoryLength =
                0.0f;

            float totalValidWeight =
                0.0f;

            const float planeDistanceThreshold =
                0.01f;

            const float worldDistanceThreshold =
                0.10f;

            for(int i = 0; i < 4; ++i)
            {
                int2 candidatePixel =
                    candidatePixels[i];

                if(!IsPixelInBounds(
                    candidatePixel,
                    dimensions))
                {
                    continue;
                }

                float candidateDepth =
                    previousDepthTexture.Load(
                        int3(
                            candidatePixel,
                            0
                        )
                    );

                if(
                    candidateDepth >= 1.0f ||
                    !isfinite(candidateDepth))
                {
                    continue;
                }

                float3 previousWorldPosition =
                    ReconstructPreviousWorldPosition(
                        candidatePixel,
                        candidateDepth,
                        dimensions
                    );

                if(
                    any(
                        !isfinite(
                            previousWorldPosition
                        )
                    ))
                {
                    continue;
                }

                float3 positionDifference =
                    previousWorldPosition -
                    worldPosition.xyz;

                float planeDistance =
                    abs(
                        dot(
                            positionDifference,
                            geometricNormal
                        )
                    );

                float worldDistance =
                    length(
                        positionDifference
                    );

                if(
                    planeDistance >
                        planeDistanceThreshold ||
                    worldDistance >
                        worldDistanceThreshold)
                {
                    continue;
                }

                float candidateVisibility =
                    historyTexture.Load(
                        int3(
                            candidatePixel,
                            0
                        )
                    );

                float candidateHistoryLength =
                    previousHistoryLengthTexture.Load(
                        int3(
                            candidatePixel,
                            0
                        )
                    );

                float candidateWeight =
                    weights[i];

                weightedVisibility +=
                    candidateVisibility *
                    candidateWeight;

                weightedHistoryLength +=
                    candidateHistoryLength *
                    candidateWeight;

                totalValidWeight +=
                    candidateWeight;
            }

            if(totalValidWeight > 0.0f)
            {
                float previousVisibility =
                    weightedVisibility /
                    totalValidWeight;

                float previousHistoryLength =
                    weightedHistoryLength /
                    totalValidWeight;

                const float maximumHistoryLength =
                    20.0f;

                historyLength =
                    min(
                        previousHistoryLength +
                        1.0f,
                        maximumHistoryLength
                    );

                float currentWeight =
                    1.0f /
                    historyLength;

                accumulatedVisibility =
                    lerp(
                        previousVisibility,
                        currentVisibility,
                        currentWeight
                    );
            }
        }
    }

    visibilityTexture[pixelIndex] =
        accumulatedVisibility;

    outputHistoryLengthTexture[pixelIndex] =
        historyLength;
}

[shader("miss")]
void Miss(
    inout ShadowPayload payload)
{
    payload.visible = 1;
}

[shader("closesthit")]
void ClosestHit(
    inout ShadowPayload payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    payload.visible = 0;
}
