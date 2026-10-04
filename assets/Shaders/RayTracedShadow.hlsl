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
    float3 padding;
};

RaytracingAccelerationStructure scene : register(t0);

RWTexture2D<float> visibilityTexture :
    register(u0);

ConstantBuffer<ShadowConstants> shadowConstants :
    register(b0);

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

    Texture2D<float4> surfaceTexture =
        ResourceDescriptorHeap[
            shadowConstants.surfaceIndex
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

    float3 normal =
        normalize(
            surfaceTexture.Load(
                int3(
                    pixelIndex,
                    0
                )
            ).rgb
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
        normal * 0.001f;

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
                            normal
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