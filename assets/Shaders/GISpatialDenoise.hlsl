cbuffer GISpatialDenoiseConstants : register(b0)
{
    uint inputIndex;
    uint outputIndex;
    uint depthIndex;
    uint normalIndex;

    uint width;
    uint height;
    uint stepWidth;
    float depthSigma;

    float normalPower;
    uint padding0;
    uint padding1;
    uint padding2;
};

// Match Camera.h and NrdDiffuseResolve.hlsl: D3D depth [0, 1],
// positive view Z, near plane 0.1 and far plane 1000.
float HardwareDepthToViewZ(float depth)
{
    const float nearPlane = 0.1f;
    const float farPlane = 1000.0f;
    return nearPlane * farPlane / (farPlane - depth * (farPlane - nearPlane));
}

bool ValidDepth(float depth)
{
    return isfinite(depth) && depth >= 0.0f && depth < 1.0f;
}

bool ValidNormal(float3 normal)
{
    float lengthSquared = dot(normal, normal);
    return all(isfinite(normal)) && isfinite(lengthSquared) && lengthSquared > 1.0e-8f;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;

    if(pixel.x >= width || pixel.y >= height)
    {
        return;
    }

    Texture2D<float4> inputTexture = ResourceDescriptorHeap[inputIndex];
    RWTexture2D<float4> outputTexture = ResourceDescriptorHeap[outputIndex];
    Texture2D<float> depthTexture = ResourceDescriptorHeap[depthIndex];
    Texture2D<float4> normalTexture = ResourceDescriptorHeap[normalIndex];

    float4 center = inputTexture.Load(int3(pixel, 0));
    float3 centerRadiance = all(isfinite(center.rgb)) ? max(center.rgb, 0.0f) : 0.0f;
    float alpha = isfinite(center.a) ? center.a : 1.0f;
    outputTexture[pixel] = float4(min(centerRadiance, 65504.0f), alpha);

    float centerDepth = depthTexture.Load(int3(pixel, 0));
    // Surface RGB stores signed world-space shading normals directly.
    float3 centerNormal = normalTexture.Load(int3(pixel, 0)).rgb;

    if(!ValidDepth(centerDepth) || !ValidNormal(centerNormal) ||
       !all(isfinite(center.rgb)) || !isfinite(depthSigma) || depthSigma <= 0.0f ||
       !isfinite(normalPower) || normalPower < 0.0f)
    {
        return;
    }

    float centerViewZ = HardwareDepthToViewZ(centerDepth);

    if(!isfinite(centerViewZ) || centerViewZ <= 0.0f)
    {
        return;
    }

    centerNormal = normalize(centerNormal);

    // Same relative depth scale as the GI reconstruction, with depthSigma
    // controlling its width. Reject beyond three sigmas at discontinuities.
    float depthScale = depthSigma * max(centerViewZ * 0.01f, 0.01f);

    if(!isfinite(depthScale) || depthScale <= 0.0f)
    {
        return;
    }

    static const float kernel[5] = {1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f};
    float3 accumulated = 0.0f;
    float totalWeight = 0.0f;
    [unroll]
    for(int y = -2; y <= 2; ++y)
    {
        [unroll]
        for(int x = -2; x <= 2; ++x)
        {
            int2 samplePixel = int2(pixel) + int2(x, y) * int(stepWidth);

            if(any(samplePixel < 0) || any(samplePixel >= int2(width, height)))
            {
                continue;
            }

            float sampleDepth = depthTexture.Load(int3(samplePixel, 0));
            float3 sampleNormal = normalTexture.Load(int3(samplePixel, 0)).rgb;

            if(!ValidDepth(sampleDepth) || !ValidNormal(sampleNormal))
            {
                continue;
            }

            float sampleViewZ = HardwareDepthToViewZ(sampleDepth);

            if(!isfinite(sampleViewZ) || sampleViewZ <= 0.0f)
            {
                continue;
            }

            float depthDifference = abs(sampleViewZ - centerViewZ) / depthScale;
            float normalSimilarity = saturate(dot(centerNormal, normalize(sampleNormal)));

            if(depthDifference > 3.0f || normalSimilarity <= 0.0f)
            {
                continue;
            }

            float3 radiance = inputTexture.Load(int3(samplePixel, 0)).rgb;

            if(!all(isfinite(radiance)))
            {
                continue;
            }

            float weight = kernel[x + 2] * kernel[y + 2] *
                exp(-0.5f * depthDifference * depthDifference) * pow(normalSimilarity, normalPower);

            if(!isfinite(weight) || weight <= 0.0f)
            {
                continue;
            }

            accumulated += min(max(radiance, 0.0f), 65504.0f) * weight;
            totalWeight += weight;
        }
    }

    if(isfinite(totalWeight) && totalWeight > 1.0e-8f)
    {
        float3 filtered = accumulated / totalWeight;

        if(all(isfinite(filtered)))
        {
            outputTexture[pixel] = float4(min(max(filtered, 0.0f), 65504.0f), alpha);
        }
    }
}
