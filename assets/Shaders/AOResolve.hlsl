struct AOResolveConstants
{
    uint inputAOIndex;
    uint depthIndex;
    uint surfaceIndex;
    uint outputAOIndex;

    uint width;
    uint height;
    uint padding0;
    uint padding1;
};

ConstantBuffer<AOResolveConstants> constants : register(b0);

// Relative device-depth tolerance and normal alignment sharpness.
static const float DEPTH_SENSITIVITY = 64.0f;
static const float NORMAL_POWER = 32.0f;

bool ValidDepth(float depth)
{
    return isfinite(depth) && depth >= 0.0f && depth < 1.0f;
}

bool NormalizeWorldNormal(float3 surfaceNormal, out float3 normal)
{
    normal = 0.0f;
    float lengthSquared = dot(surfaceNormal, surfaceNormal);
    if(!all(isfinite(surfaceNormal)) || !isfinite(lengthSquared) || lengthSquared < 1.0e-8f)
    {
        return false;
    }

    // Surface RGB is already a world-space normal.
    normal = surfaceNormal * rsqrt(lengthSquared);
    return all(isfinite(normal));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 fullPixel = dispatchThreadID.xy;
    uint2 fullResolution = uint2(constants.width, constants.height);

    if(any(fullPixel >= fullResolution))
    {
        return;
    }

    Texture2D<float> inputAO = ResourceDescriptorHeap[constants.inputAOIndex];
    Texture2D<float> depthTexture = ResourceDescriptorHeap[constants.depthIndex];
    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[constants.surfaceIndex];
    RWTexture2D<float> outputAO = ResourceDescriptorHeap[constants.outputAOIndex];

    uint2 aoDimensions;
    inputAO.GetDimensions(aoDimensions.x, aoDimensions.y);

    if(any(aoDimensions == 0u))
    {
        outputAO[fullPixel] = 1.0f;
        return;
    }

    float centerDepth = depthTexture.Load(int3(fullPixel, 0));
    if(!ValidDepth(centerDepth))
    {
        outputAO[fullPixel] = 1.0f;
        return;
    }

    bool sameResolution = all(aoDimensions == fullResolution);
    uint2 aoPixel = sameResolution
        ? fullPixel
        : min(fullPixel / 2u, aoDimensions - 1u);
    float visibility = inputAO.Load(int3(aoPixel, 0));
    visibility = isfinite(visibility) ? saturate(visibility) : 1.0f;

    float3 centerNormal;
    if(sameResolution ||
       !NormalizeWorldNormal(surfaceTexture.Load(int3(fullPixel, 0)).rgb, centerNormal))
    {
        outputAO[fullPixel] = visibility;
        return;
    }

    // Convert full-resolution pixel centers into half-resolution texel space.
    float2 aoPosition = (float2(fullPixel) + 0.5f) * 0.5f - 0.5f;
    int2 basePixel = int2(floor(aoPosition));
    float2 fraction = frac(aoPosition);
    float weightedVisibility = 0.0f;
    float totalWeight = 0.0f;

    [unroll]
    for(int y = 0; y < 2; ++y)
    {
        [unroll]
        for(int x = 0; x < 2; ++x)
        {
            int2 candidatePixel = basePixel + int2(x, y);
            if(any(candidatePixel < 0) || any(candidatePixel >= int2(aoDimensions)))
            {
                continue;
            }

            float candidateVisibility = inputAO.Load(int3(candidatePixel, 0));
            if(!isfinite(candidateVisibility))
            {
                continue;
            }

            uint2 candidateFullPixel = min(uint2(candidatePixel) * 2u + 1u, fullResolution - 1u);
            float candidateDepth = depthTexture.Load(int3(candidateFullPixel, 0));
            if(!ValidDepth(candidateDepth))
            {
                continue;
            }

            float3 candidateNormal;
            if(!NormalizeWorldNormal(surfaceTexture.Load(int3(candidateFullPixel, 0)).rgb, candidateNormal))
            {
                continue;
            }

            float2 spatialWeights = float2(x == 0 ? 1.0f - fraction.x : fraction.x,
                                          y == 0 ? 1.0f - fraction.y : fraction.y);
            float relativeDepthDifference = abs(candidateDepth - centerDepth) /
                max(max(abs(candidateDepth), abs(centerDepth)), 1.0e-6f);
            float depthWeight = exp(-relativeDepthDifference * DEPTH_SENSITIVITY);
            float normalWeight = pow(saturate(dot(centerNormal, candidateNormal)), NORMAL_POWER);
            float weight = spatialWeights.x * spatialWeights.y * depthWeight * normalWeight;
            if(!isfinite(weight) || weight <= 0.0f)
            {
                continue;
            }

            weightedVisibility += saturate(candidateVisibility) * weight;
            totalWeight += weight;
        }
    }

    if(isfinite(totalWeight) && totalWeight > 1.0e-8f && isfinite(weightedVisibility))
    {
        visibility = weightedVisibility / totalWeight;
    }
    outputAO[fullPixel] = isfinite(visibility) ? saturate(visibility) : 1.0f;
}
