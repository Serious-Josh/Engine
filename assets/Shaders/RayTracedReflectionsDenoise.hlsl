struct ReflectionDenoiseConstants
{
    row_major float4x4 inverseViewProjection;

    uint reflectionIndex;
    uint depthIndex;
    uint surfaceIndex;
    uint materialIndex;

    uint outputIndex;
    uint width;
    uint height;
    uint stepWidth;

    uint hitDistanceIndex;
    uint3 padding;
};

ConstantBuffer<ReflectionDenoiseConstants> constants :
    register(b0);

static const int FILTER_RADIUS = 2;

float3 ReconstructWorldPosition(
    uint2 pixel,
    float depth)
{
    float2 uv =
        (float2(pixel) + 0.5f) /
        float2(
            constants.width,
            constants.height);

    float2 ndc;

    ndc.x =
        uv.x * 2.0f - 1.0f;

    ndc.y =
        1.0f - uv.y * 2.0f;

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

    return
        worldPosition.xyz /
        worldPosition.w;
}

[numthreads(8, 8, 1)]
void CSMain(
    uint3 dispatchThreadID :
        SV_DispatchThreadID)
{
    uint2 pixel =
        dispatchThreadID.xy;

    if(pixel.x >= constants.width ||
       pixel.y >= constants.height)
    {
        return;
    }

    Texture2D<float4> reflectionTexture =
        ResourceDescriptorHeap[
            constants.reflectionIndex];

    Texture2D<float> depthTexture =
        ResourceDescriptorHeap[
            constants.depthIndex];

    Texture2D<float4> surfaceTexture =
        ResourceDescriptorHeap[
            constants.surfaceIndex];

    Texture2D<float4> materialTexture =
        ResourceDescriptorHeap[
            constants.materialIndex];

    Texture2D<float> hitDistanceTexture =
        ResourceDescriptorHeap[
            constants.hitDistanceIndex];

    RWTexture2D<float4> outputTexture =
        ResourceDescriptorHeap[
            constants.outputIndex];

    float4 centerReflection =
        reflectionTexture[pixel];

    float centerDepth =
        depthTexture[pixel];

    if(centerDepth >= 1.0f)
    {
        outputTexture[pixel] =
            centerReflection;

        return;
    }

    float3 centerNormal =
        normalize(
            surfaceTexture[pixel].xyz);

    float centerRoughness =
        materialTexture[pixel].a;

    float centerHitDistance =
        hitDistanceTexture[pixel];

    bool centerHitGeometry =
        centerHitDistance >= 0.0f;

    float3 centerPosition =
        ReconstructWorldPosition(
            pixel,
            centerDepth);

    //
    // A perfectly smooth surface should retain
    // almost all of its original reflection detail.
    //
    // As roughness increases, neighboring stochastic
    // reflection samples become increasingly useful.
    //
    float roughnessFilterStrength =
        saturate(
            centerRoughness *
            centerRoughness);

    //
    // Preserve a very small amount of filtering even
    // at low roughness, but make rough surfaces much
    // more willing to accept neighboring samples.
    //
    float spatialSigma =
        lerp(
            0.75f,
            4.0f,
            roughnessFilterStrength);

    float weightedReflectionWeight =
        0.0f;

    float3 weightedReflection =
        float3(
            0.0f,
            0.0f,
            0.0f);

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
                int2(pixel) +
                offset;

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

            float sampleRoughness =
                materialTexture[
                    samplePixel].a;

            float sampleHitDistance =
                hitDistanceTexture[
                    samplePixel];

            bool sampleHitGeometry =
                sampleHitDistance >= 0.0f;

            //
            // A geometry reflection hit and an environment
            // reflection miss represent fundamentally
            // different reflection events. Do not blur
            // radiance across that boundary.
            //
            //if(centerHitGeometry !=
            //   sampleHitGeometry)
            //{
            //    continue;
            //}

            float3 samplePosition =
                ReconstructWorldPosition(
                    uint2(samplePixel),
                    sampleDepth);

            float3 sampleReflection =
                reflectionTexture[
                    samplePixel].rgb;

            float distanceSquared =
                float(
                    x * x +
                    y * y);

            float spatialWeight =
                exp(
                    -distanceSquared /
                    (2.0f *
                     spatialSigma *
                     spatialSigma));

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

            //
            // Avoid mixing reflection distributions
            // belonging to substantially different
            // roughness values.
            //
            float roughnessDifference =
                abs(
                    sampleRoughness -
                    centerRoughness);

            float roughnessWeight =
                exp(
                    -roughnessDifference *
                    16.0f);

            //
            // If both rays hit geometry, compare their
            // reflection hit distances. Use a relative
            // difference so this behaves reasonably
            // across both near and distant geometry.
            //
            // If both rays missed into the environment,
            // hit distance provides no additional
            // distinction and the weight remains 1.
            //
            float hitDistanceWeight =
                1.0f;

            if(centerHitGeometry &&
               sampleHitGeometry)
            {
                float hitDistanceDifference =
                    abs(
                        sampleHitDistance -
                        centerHitDistance);

                float hitDistanceScale =
                    max(
                        min(
                            centerHitDistance,
                            sampleHitDistance),
                        0.1f);

                float relativeHitDistanceDifference =
                    hitDistanceDifference /
                    hitDistanceScale;

                hitDistanceWeight =
                    exp(
                        -relativeHitDistanceDifference *
                        4.0f);
            }

            float weight =
                spatialWeight *
                positionWeight *
                normalWeight *
                roughnessWeight;

            weightedReflection +=
                sampleReflection *
                weight;

            weightedReflectionWeight +=
                weight;
        }
    }

    if(weightedReflectionWeight > 0.0f)
    {
        float3 filteredReflection =
            weightedReflection /
            weightedReflectionWeight;

        //
        // Smooth surfaces stay close to their original
        // stochastic reflection. Rough surfaces receive
        // progressively more of the spatially filtered
        // result.
        //
        float filterBlend =
            saturate(
                0.05f +
                roughnessFilterStrength *
                0.95f);

        float3 result =
            lerp(
                centerReflection.rgb,
                filteredReflection,
                filterBlend);

        outputTexture[pixel] =
            float4(
                result,
                centerReflection.a);
    }
    else
    {
        outputTexture[pixel] =
            centerReflection;
    }
}