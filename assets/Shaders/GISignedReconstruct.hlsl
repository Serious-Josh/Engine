struct GISignedReconstructConstants
{
    uint historyIndex;
    uint depthIndex;
    uint normalIndex;
    uint baselineIndex;

    uint productionGIIndex;
    uint outputIndex;

    uint fullWidth;
    uint fullHeight;
    uint giWidth;
    uint giHeight;
    uint2 padding;
};

ConstantBuffer<GISignedReconstructConstants> constants : register(b0);
SamplerState materialSampler : register(s0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;

    if(any(pixel >= uint2(constants.giWidth, constants.giHeight)))
    {
        return;
    }

    Texture2D<float4> productionGI = ResourceDescriptorHeap[constants.productionGIIndex];

    RWTexture2D<float4> outputGI = ResourceDescriptorHeap[constants.outputIndex];
    float4 original = productionGI.Load(int3(pixel, 0));

    // Preserve production invalid/background handling and alpha; hit distance is reused separately.
    outputGI[pixel] = original;

    if(constants.fullWidth == 0u || constants.fullHeight == 0u)
    {
        return;
    }

    uint2 representative = min(
        pixel * 2u,
        uint2(constants.fullWidth, constants.fullHeight) - 1u);

    Texture2D<float> depthTexture = ResourceDescriptorHeap[constants.depthIndex];

    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[constants.normalIndex];
    float depth = depthTexture.Load(int3(representative, 0));
    float3 normal = surfaceTexture.Load(int3(representative, 0)).rgb;
    float lengthSquared = dot(normal, normal);

    if(!isfinite(depth) || depth < 0.0f || depth >= 1.0f || !all(isfinite(normal)) ||
       !isfinite(lengthSquared) || lengthSquared < 1.0e-8f)
    {
        return;
    }

    Texture2D<float4> historyTexture = ResourceDescriptorHeap[constants.historyIndex];
    float4 history = historyTexture.Load(int3(pixel, 0));

    if(!all(isfinite(history)) || history.a < 1.0f)
    {
        return;
    }

    // Legacy pass/CBV names remain stable for cooked bundles. New bundles store absolute
    // irradiance in the history; historical bundles retain their matched reconstruction shader.
    float3 irradiance = history.rgb;

    if(all(isfinite(irradiance)))
    {
        outputGI[pixel] = float4(clamp(irradiance, 0.0f, 65504.0f), original.a);
    }
}
