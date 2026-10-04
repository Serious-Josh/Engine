struct DenoiseConstants
{
    row_major float4x4 inverseViewProjection;

    uint visibilityIndex;
    uint depthIndex;
    uint surfaceIndex;
    uint outputIndex;

    uint width;
    uint height;
    uint stepWidth;
    uint padding;
};

ConstantBuffer<DenoiseConstants> constants : register(b0);

static const int FILTER_RADIUS = 2;

float3 ReconstructWorldPosition(
    uint2 pixel,
    float depth)
{
    float2 uv =
        (float2(pixel) + 0.5f) /
        float2(constants.width, constants.height);

    float2 ndc;
    ndc.x = uv.x * 2.0f - 1.0f;
    ndc.y = 1.0f - uv.y * 2.0f;

    float4 clipPosition =
        float4(
            ndc.x,
            ndc.y,
            depth,
            1.0f);

    float4 worldPosition =
        mul(
            constants.inverseViewProjection,
            clipPosition);

    return worldPosition.xyz /
           worldPosition.w;
}

[numthreads(8, 8, 1)]
void CSMain(
    uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;

    if(pixel.x >= constants.width ||
       pixel.y >= constants.height)
    {
        return;
    }

    Texture2D<float> visibilityTexture =
        ResourceDescriptorHeap[
            constants.visibilityIndex];

    Texture2D<float> depthTexture =
        ResourceDescriptorHeap[
            constants.depthIndex];

    Texture2D<float4> surfaceTexture =
        ResourceDescriptorHeap[
            constants.surfaceIndex];

    RWTexture2D<float> outputTexture =
        ResourceDescriptorHeap[
            constants.outputIndex];

    float centerVisibility =
        visibilityTexture[pixel];

    float centerDepth =
        depthTexture[pixel];

    if(centerDepth >= 1.0f)
    {
        outputTexture[pixel] =
            centerVisibility;

        return;
    }

    float3 centerNormal =
        normalize(
            surfaceTexture[pixel].xyz);

    float3 centerPosition =
        ReconstructWorldPosition(
            pixel,
            centerDepth);

    float weightedVisibility = 0.0f;
    float totalWeight = 0.0f;

    for(int y = -FILTER_RADIUS;
        y <= FILTER_RADIUS;
        ++y)
    {
        for(int x = -FILTER_RADIUS;
            x <= FILTER_RADIUS;
            ++x)
        {
            int2 offset =
                int2(x, y) *
                int(constants.stepWidth);

            int2 samplePixel =
                int2(pixel) + offset;

            samplePixel.x =
                clamp(
                    samplePixel.x,
                    0,
                    int(constants.width) - 1);

            samplePixel.y =
                clamp(
                    samplePixel.y,
                    0,
                    int(constants.height) - 1);

            float sampleVisibility =
                visibilityTexture[
                    samplePixel];

            float sampleDepth =
                depthTexture[
                    samplePixel];

            if(sampleDepth >= 1.0f)
            {
                continue;
            }

            float3 sampleNormal =
                normalize(
                    surfaceTexture[
                        samplePixel].xyz);

            float3 samplePosition =
                ReconstructWorldPosition(
                    uint2(samplePixel),
                    sampleDepth);

            float distanceSquared =
                float(x * x + y * y);

            float spatialWeight =
                exp(
                    -distanceSquared /
                    8.0f);

            float3 positionDifference =
                samplePosition -
                centerPosition;

            float planeDistance =
                abs(
                    dot(
                        positionDifference,
                        centerNormal));

            float positionWeight =
                exp(
                    -planeDistance *
                    20.0f);

            float normalSimilarity =
                saturate(
                    dot(
                        centerNormal,
                        sampleNormal));

            float normalWeight =
                pow(
                    normalSimilarity,
                    32.0f);

            float weight =
                spatialWeight *
                positionWeight *
                normalWeight;

            weightedVisibility +=
                sampleVisibility *
                weight;

            totalWeight += weight;
        }
    }

    if(totalWeight > 0.0f)
    {
        outputTexture[pixel] =
            weightedVisibility /
            totalWeight;
    }
    else
    {
        outputTexture[pixel] =
            centerVisibility;
    }
}