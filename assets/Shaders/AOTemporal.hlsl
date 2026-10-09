struct AOTemporalConstants
{
    row_major float4x4 inverseViewProjection;
    row_major float4x4 previousViewProjection;
    row_major float4x4 previousInverseViewProjection;

    uint rawAOIndex;
    uint depthIndex;
    uint surfaceIndex;
    uint previousAOIndex;

    uint previousDepthIndex;
    uint previousHistoryLengthIndex;
    uint outputAOIndex;
    uint outputDepthIndex;

    uint outputHistoryLengthIndex;
    uint width;
    uint height;
    uint accumulatedFrameCount;
};

ConstantBuffer<AOTemporalConstants> constants : register(b0);

bool ReconstructWorldPosition(int2 pixel, float depth, uint2 dimensions,
    float4x4 inverseVP, out float3 position)
{
    position = 0.0f;
    if(!isfinite(depth) || depth < 0.0f || depth >= 1.0f)
        return false;

    // Match shadow reconstruction: pixel centers, flipped NDC Y, D3D depth.
    float2 uv = (float2(pixel) + 0.5f) / float2(dimensions);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 world = mul(inverseVP, float4(ndc, depth, 1.0f));
    if(!all(isfinite(world)) || abs(world.w) < 1.0e-8f)
        return false;

    position = world.xyz / world.w;
    return all(isfinite(position));
}

bool ReprojectWorldPosition(float3 position, uint2 dimensions, out float2 previousPixel)
{
    previousPixel = 0.0f;
    float4 clip = mul(constants.previousViewProjection, float4(position, 1.0f));
    if(!all(isfinite(clip)) || clip.w < 1.0e-8f)
        return false;

    float3 ndc = clip.xyz / clip.w;
    if(!all(isfinite(ndc)) || ndc.z < 0.0f || ndc.z > 1.0f)
        return false;

    float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
    if(any(uv < 0.0f) || any(uv >= 1.0f))
        return false;

    previousPixel = uv * float2(dimensions) - 0.5f;
    return all(isfinite(previousPixel));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;
    uint2 dimensions = uint2(constants.width, constants.height);
    if(any(pixel >= dimensions))
        return;

    Texture2D<float> rawAOTexture = ResourceDescriptorHeap[constants.rawAOIndex];
    Texture2D<float> depthTexture = ResourceDescriptorHeap[constants.depthIndex];
    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[constants.surfaceIndex];
    RWTexture2D<float> outputAO = ResourceDescriptorHeap[constants.outputAOIndex];
    RWTexture2D<float> outputDepth = ResourceDescriptorHeap[constants.outputDepthIndex];
    RWTexture2D<float> outputHistoryLength = ResourceDescriptorHeap[constants.outputHistoryLengthIndex];

    float rawVisibility = rawAOTexture.Load(int3(pixel, 0));
    float depth = depthTexture.Load(int3(pixel, 0));
    float3 surfaceNormal = surfaceTexture.Load(int3(pixel, 0)).rgb;
    outputDepth[pixel] = depth;

    if(!isfinite(depth) || depth < 0.0f || depth >= 1.0f)
    {
        outputAO[pixel] = 1.0f;
        outputHistoryLength[pixel] = 1.0f;
        return;
    }

    // Invalid raw input falls back to unoccluded and starts a fresh history.
    float accumulatedAO = isfinite(rawVisibility) ? rawVisibility : 1.0f;
    float historyLength = 1.0f;
    float normalLengthSquared = dot(surfaceNormal, surfaceNormal);

    if(constants.accumulatedFrameCount > 0 && isfinite(rawVisibility) &&
       all(isfinite(surfaceNormal)) && isfinite(normalLengthSquared) &&
       normalLengthSquared >= 1.0e-8f)
    {
        float3 worldPosition;
        float2 previousPixelPosition;
        if(ReconstructWorldPosition(int2(pixel), depth, dimensions,
            constants.inverseViewProjection, worldPosition) &&
           ReprojectWorldPosition(worldPosition, dimensions, previousPixelPosition))
        {
            // History resources are accessed only after the first frame and valid reprojection.
            Texture2D<float> previousAO = ResourceDescriptorHeap[constants.previousAOIndex];
            Texture2D<float> previousDepth = ResourceDescriptorHeap[constants.previousDepthIndex];
            Texture2D<float> previousHistoryLength =
                ResourceDescriptorHeap[constants.previousHistoryLengthIndex];

            float3 normal = normalize(surfaceNormal);
            int2 basePixel = int2(floor(previousPixelPosition));
            float2 fraction = frac(previousPixelPosition);
            float weights[4] = {
                (1.0f - fraction.x) * (1.0f - fraction.y),
                fraction.x * (1.0f - fraction.y),
                (1.0f - fraction.x) * fraction.y,
                fraction.x * fraction.y
            };
            int2 offsets[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };
            float weightedAO = 0.0f;
            float weightedHistoryLength = 0.0f;
            float totalValidWeight = 0.0f;
            const float planeDistanceThreshold = 0.01f;
            const float worldDistanceThreshold = 0.10f;

            for(int i = 0; i < 4; ++i)
            {
                int2 candidate = basePixel + offsets[i];
                if(weights[i] <= 0.0f || any(candidate < 0) || any(candidate >= int2(dimensions)))
                    continue;

                float candidateDepth = previousDepth.Load(int3(candidate, 0));
                float3 previousWorldPosition;
                if(!ReconstructWorldPosition(candidate, candidateDepth, dimensions,
                    constants.previousInverseViewProjection, previousWorldPosition))
                    continue;

                float3 difference = previousWorldPosition - worldPosition;
                float planeDistance = abs(dot(difference, normal));
                float worldDistance = length(difference);
                if(!isfinite(planeDistance) || !isfinite(worldDistance) ||
                   planeDistance > planeDistanceThreshold || worldDistance > worldDistanceThreshold)
                    continue;

                float candidateAO = previousAO.Load(int3(candidate, 0));
                float candidateHistoryLength = previousHistoryLength.Load(int3(candidate, 0));
                if(!isfinite(candidateAO) || !isfinite(candidateHistoryLength) ||
                   candidateHistoryLength < 1.0f)
                    continue;

                weightedAO += candidateAO * weights[i];
                weightedHistoryLength += min(candidateHistoryLength, 20.0f) * weights[i];
                totalValidWeight += weights[i];
            }

            if(totalValidWeight > 0.0f)
            {
                float previousVisibility = weightedAO / totalValidWeight;
                float previousLength = weightedHistoryLength / totalValidWeight;
                historyLength = min(previousLength + 1.0f, 20.0f);
                float currentWeight = 1.0f / historyLength;
                accumulatedAO = lerp(previousVisibility, rawVisibility, currentWeight);
            }
        }
    }

    outputAO[pixel] = accumulatedAO;
    outputHistoryLength[pixel] = historyLength;
}
