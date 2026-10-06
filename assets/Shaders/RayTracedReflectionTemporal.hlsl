struct ReflectionTemporalConstants
{
    row_major float4x4 inverseViewProjection;
    row_major float4x4 previousViewProjection;
    row_major float4x4 previousInverseViewProjection;

    uint rawReflectionIndex;
    uint depthIndex;
    uint surfaceIndex;
    uint historyIndex;

    uint previousDepthIndex;
    uint outputDepthIndex;
    uint previousHistoryLengthIndex;
    uint outputHistoryLengthIndex;

    uint outputReflectionIndex;
    uint width;
    uint height;
    uint accumulatedFrameCount;
};

ConstantBuffer<ReflectionTemporalConstants> constants :
    register(b0);

bool ReprojectWorldPosition(
    float3 worldPosition,
    uint2 dimensions,
    out float2 previousPixelPosition)
{
    float4 previousClipPosition =
        mul(
            constants.previousViewProjection,
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

float3 ReconstructWorldPosition(
    int2 pixelIndex,
    float depth,
    uint2 dimensions,
    row_major float4x4 inverseViewProjection)
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
            inverseViewProjection,
            clipPosition
        );

    return
        worldPosition.xyz /
        worldPosition.w;
}

float Luminance(float3 color)
{
    return
        dot(
            color,
            float3(
                0.2126f,
                0.7152f,
                0.0722f
            )
        );
}

float LogLuminance(float3 color)
{
    float luminance =
        max(
            Luminance(color),
            0.0f
        );

    return
        log2(
            1.0f + luminance
        );
}

float3 ClipHistoryLuminance(
    float3 historyColor,
    float minimumLogLuminance,
    float maximumLogLuminance)
{
    float historyLuminance =
        max(
            Luminance(historyColor),
            0.0f
        );

    if(historyLuminance <= 1.0e-6f)
    {
        return historyColor;
    }

    float historyLogLuminance =
        log2(
            1.0f + historyLuminance
        );

    float clippedLogLuminance =
        clamp(
            historyLogLuminance,
            minimumLogLuminance,
            maximumLogLuminance
        );

    float clippedLuminance =
        exp2(
            clippedLogLuminance
        ) -
        1.0f;

    float scale =
        clippedLuminance /
        historyLuminance;

    return
        historyColor *
        scale;
}

[numthreads(8, 8, 1)]
void CSMain(
    uint3 dispatchThreadID :
        SV_DispatchThreadID)
{
    uint2 pixelIndex =
        dispatchThreadID.xy;

    uint2 dimensions =
        uint2(
            constants.width,
            constants.height
        );

    if(
        pixelIndex.x >= dimensions.x ||
        pixelIndex.y >= dimensions.y)
    {
        return;
    }

    Texture2D<float4> rawReflectionTexture =
        ResourceDescriptorHeap[
            constants.rawReflectionIndex
        ];

    Texture2D<float> depthTexture =
        ResourceDescriptorHeap[
            constants.depthIndex
        ];

    Texture2D<float4> surfaceTexture =
        ResourceDescriptorHeap[
            constants.surfaceIndex
        ];

    Texture2D<float4> historyTexture =
        ResourceDescriptorHeap[
            constants.historyIndex
        ];

    Texture2D<float> previousDepthTexture =
        ResourceDescriptorHeap[
            constants.previousDepthIndex
        ];

    Texture2D<float> previousHistoryLengthTexture =
        ResourceDescriptorHeap[
            constants.previousHistoryLengthIndex
        ];

    RWTexture2D<float4> outputReflectionTexture =
        ResourceDescriptorHeap[
            constants.outputReflectionIndex
        ];

    RWTexture2D<float> outputDepthTexture =
        ResourceDescriptorHeap[
            constants.outputDepthIndex
        ];

    RWTexture2D<float> outputHistoryLengthTexture =
        ResourceDescriptorHeap[
            constants.outputHistoryLengthIndex
        ];

    float depth =
        depthTexture.Load(
            int3(pixelIndex, 0)
        );

    outputDepthTexture[pixelIndex] =
        depth;

    if(depth >= 1.0f)
    {
        outputReflectionTexture[pixelIndex] =
            float4(
                0.0f,
                0.0f,
                0.0f,
                1.0f
            );

        outputHistoryLengthTexture[pixelIndex] =
            1.0f;

        return;
    }

    float3 worldPosition =
        ReconstructWorldPosition(
            int2(pixelIndex),
            depth,
            dimensions,
            constants.inverseViewProjection
        );

    float4 surfaceData =
        surfaceTexture.Load(
            int3(pixelIndex, 0)
        );

    float3 normal =
        normalize(
            surfaceData.rgb
        );

    float3 currentReflection =
        rawReflectionTexture.Load(
            int3(pixelIndex, 0)
        ).rgb;

        float logLuminanceSum =
            0.0f;

        float logLuminanceSquaredSum =
            0.0f;

        float neighborhoodSampleCount =
            0.0f;

        for(int y = -1; y <= 1; ++y)
        {
            for(int x = -1; x <= 1; ++x)
            {
                int2 neighborPixel =
                    int2(pixelIndex) +
                    int2(x, y);

                if(!IsPixelInBounds(
                    neighborPixel,
                    dimensions))
                {
                    continue;
                }

                float3 neighborReflection =
                    rawReflectionTexture.Load(
                        int3(
                            neighborPixel,
                            0
                        )
                    ).rgb;

                if(any(!isfinite(neighborReflection)))
                {
                    continue;
                }

                float neighborLogLuminance =
                    LogLuminance(
                        neighborReflection
                    );

                logLuminanceSum +=
                    neighborLogLuminance;

                logLuminanceSquaredSum +=
                    neighborLogLuminance *
                    neighborLogLuminance;

                neighborhoodSampleCount +=
                    1.0f;
            }
        }

        float neighborhoodMean =
            logLuminanceSum /
            max(
                neighborhoodSampleCount,
                1.0f
            );

        float neighborhoodVariance =
            max(
                logLuminanceSquaredSum /
                max(
                    neighborhoodSampleCount,
                    1.0f
                ) -
                neighborhoodMean *
                neighborhoodMean,
                0.0f
            );

        float neighborhoodStandardDeviation =
            sqrt(
                neighborhoodVariance
            );

        const float historyClipSigma =
            2.5f;

        float minimumHistoryLogLuminance =
            max(
                neighborhoodMean -
                historyClipSigma *
                neighborhoodStandardDeviation,
                0.0f
            );

        float maximumHistoryLogLuminance =
            neighborhoodMean +
            historyClipSigma *
            neighborhoodStandardDeviation;

    float3 accumulatedReflection =
        currentReflection;

    float historyLength =
        1.0f;

    if(constants.accumulatedFrameCount > 0)
    {
        float2 previousPixelPosition;

        bool reprojectionValid =
            ReprojectWorldPosition(
                worldPosition,
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

            float3 weightedReflection =
                float3(
                    0.0f,
                    0.0f,
                    0.0f
                );

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
                    ReconstructWorldPosition(
                        candidatePixel,
                        candidateDepth,
                        dimensions,
                        constants.previousInverseViewProjection
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
                    worldPosition;

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

                float3 candidateReflection =
                    historyTexture.Load(
                        int3(
                            candidatePixel,
                            0
                        )
                    ).rgb;

                float candidateHistoryLength =
                    previousHistoryLengthTexture.Load(
                        int3(
                            candidatePixel,
                            0
                        )
                    );

                float candidateWeight =
                    weights[i];

                weightedReflection +=
                    candidateReflection *
                    candidateWeight;

                weightedHistoryLength +=
                    candidateHistoryLength *
                    candidateWeight;

                totalValidWeight +=
                    candidateWeight;
            }

            if(totalValidWeight > 0.0f)
            {
                float3 previousReflection =
                    weightedReflection /
                    totalValidWeight;

                float previousHistoryLength =
                    weightedHistoryLength /
                    totalValidWeight;

                previousReflection =
                    ClipHistoryLuminance(
                        previousReflection,
                        minimumHistoryLogLuminance,
                        maximumHistoryLogLuminance
                    );

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

                accumulatedReflection =
                    lerp(
                        previousReflection,
                        currentReflection,
                        currentWeight
                    );
            }
        }
    }

    outputReflectionTexture[pixelIndex] =
        float4(
            accumulatedReflection,
            1.0f
        );

    outputHistoryLengthTexture[pixelIndex] =
        historyLength;
}
