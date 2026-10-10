struct AODenoiseConstants
{
    row_major float4x4 inverseViewProjection;

    uint inputAOIndex;
    uint depthIndex;
    uint surfaceIndex;
    uint outputAOIndex;

    uint width;
    uint height;
    uint stepWidth;
    uint padding;
};

ConstantBuffer<AODenoiseConstants> constants : register(b0);

static const int FILTER_RADIUS = 2;

// exp(-(x*x + y*y) / 8), indexed by abs(x) + 3 * abs(y).
static const float SPATIAL_WEIGHTS[9] = {
    1.0f,          0.8824969026f, 0.6065306597f,
    0.8824969026f, 0.7788007831f, 0.5352614285f,
    0.6065306597f, 0.5352614285f, 0.3678794412f
};

float NormalWeight(float3 centerNormal, float3 sampleNormal)
{
    float weight = saturate(dot(centerNormal, sampleNormal));
    // Five squarings evaluate the same integer power without log2/exp2.
    weight *= weight;
    weight *= weight;
    weight *= weight;
    weight *= weight;
    weight *= weight;
    return weight;
}

uint2 AOToFullPixel(uint2 aoPixel, uint2 fullResolution)
{
    if(all(uint2(constants.width, constants.height) == fullResolution))
    {
        return aoPixel;
    }

    return min(aoPixel * 2u + 1u, fullResolution - 1u);
}

bool ReconstructWorldPosition(uint2 fullPixel, float depth, uint2 fullResolution, out float3 position)
{
    position = 0.0f;

    if(!isfinite(depth) || depth < 0.0f || depth >= 1.0f)
    {
        return false;
    }

    // Match ShadowDenoise: pixel centers, flipped NDC Y, D3D [0, 1] depth.
    float2 uv = (float2(fullPixel) + 0.5f) / float2(fullResolution);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 worldPosition = mul(constants.inverseViewProjection, float4(ndc, depth, 1.0f));

    if(!all(isfinite(worldPosition)) || abs(worldPosition.w) < 1.0e-8f)
    {
        return false;
    }

    position = worldPosition.xyz / worldPosition.w;
    return all(isfinite(position));
}

bool NormalizeWorldNormal(float3 surfaceNormal, out float3 normal)
{
    normal = 0.0f;
    float lengthSquared = dot(surfaceNormal, surfaceNormal);

    if(!all(isfinite(surfaceNormal)) || !isfinite(lengthSquared) || lengthSquared < 1.0e-8f)
    {
        return false;
    }

    // Surface RGB already contains a world-space normal, without remapping.
    normal = normalize(surfaceNormal);
    return all(isfinite(normal));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 aoPixel = dispatchThreadID.xy;

    if(aoPixel.x >= constants.width || aoPixel.y >= constants.height)
    {
        return;
    }

    Texture2D<float> aoTexture = ResourceDescriptorHeap[constants.inputAOIndex];
    Texture2D<float> depthTexture = ResourceDescriptorHeap[constants.depthIndex];
    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[constants.surfaceIndex];
    RWTexture2D<float> outputTexture = ResourceDescriptorHeap[constants.outputAOIndex];

    uint2 fullResolution;
    depthTexture.GetDimensions(fullResolution.x, fullResolution.y);

    if(any(fullResolution == 0u))
    {
        outputTexture[aoPixel] = 1.0f;
        return;
    }

    uint2 fullPixel = AOToFullPixel(aoPixel, fullResolution);
    float centerVisibility = aoTexture.Load(int3(aoPixel, 0));
    float centerDepth = depthTexture.Load(int3(fullPixel, 0));
    if(!isfinite(centerDepth) || centerDepth < 0.0f || centerDepth >= 1.0f)
    {
        outputTexture[aoPixel] = 1.0f;
        return;
    }

    // Keep the original validity separate: invalid raw samples never contribute.
    bool centerVisibilityValid = isfinite(centerVisibility);

    if(!centerVisibilityValid)
    {
        centerVisibility = 1.0f;
    }

    float3 centerSurfaceNormal = surfaceTexture.Load(int3(fullPixel, 0)).rgb;
    float3 centerNormal;
    float3 centerPosition;
    if(!NormalizeWorldNormal(centerSurfaceNormal, centerNormal) ||
       !ReconstructWorldPosition(fullPixel, centerDepth, fullResolution, centerPosition))
    {
        outputTexture[aoPixel] = saturate(centerVisibility);
        return;
    }

    float weightedVisibility = 0.0f;
    float totalWeight = 0.0f;
    for(int y = -FILTER_RADIUS; y <= FILTER_RADIUS; ++y)
    {
        for(int x = -FILTER_RADIUS; x <= FILTER_RADIUS; ++x)
        {
            if(x == 0 && y == 0)
            {
                if(centerVisibilityValid)
                {
                    // Retain the self-normal weight, including normalization rounding.
                    float weight = NormalWeight(centerNormal, centerNormal);
                    weightedVisibility += centerVisibility * weight;
                    totalWeight += weight;
                }
                continue;
            }

            int2 offset = int2(x, y) * int(constants.stepWidth);
            int2 samplePixel = int2(aoPixel) + offset;

            if(any(samplePixel < 0) ||
               samplePixel.x >= int(constants.width) || samplePixel.y >= int(constants.height))
            {
                continue;
            }

            uint2 sampleFullPixel = AOToFullPixel(uint2(samplePixel), fullResolution);
            float sampleDepth = depthTexture.Load(int3(sampleFullPixel, 0));

            if(!isfinite(sampleDepth) || sampleDepth < 0.0f || sampleDepth >= 1.0f)
            {
                continue;
            }

            float3 sampleNormal;

            if(!NormalizeWorldNormal(surfaceTexture.Load(int3(sampleFullPixel, 0)).rgb, sampleNormal))
            {
                continue;
            }

            float normalWeight = NormalWeight(centerNormal, sampleNormal);

            if(normalWeight == 0.0f)
            {
                continue;
            }

            float sampleVisibility = aoTexture.Load(int3(samplePixel, 0));

            if(!isfinite(sampleVisibility))
            {
                continue;
            }

            float3 samplePosition;

            if(!ReconstructWorldPosition(sampleFullPixel, sampleDepth, fullResolution, samplePosition))
            {
                continue;
            }

            // Spatial distance uses kernel coordinates, as in ShadowDenoise.
            float spatialWeight = SPATIAL_WEIGHTS[abs(x) + 3 * abs(y)];
            float3 positionDifference = samplePosition - centerPosition;
            float planeDistance = abs(dot(positionDifference, centerNormal));

            if(!all(isfinite(positionDifference)) || !isfinite(planeDistance))
            {
                continue;
            }

            float positionWeight = exp(-planeDistance * 20.0f);
            float weight = spatialWeight * positionWeight * normalWeight;
            weightedVisibility += sampleVisibility * weight;
            totalWeight += weight;
        }
    }

    float visibility = totalWeight > 0.0f ? weightedVisibility / totalWeight : centerVisibility;
    outputTexture[aoPixel] = saturate(visibility);
}
