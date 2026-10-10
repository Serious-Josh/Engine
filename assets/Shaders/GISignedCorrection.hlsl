struct GISignedCorrectionConstants
{
    row_major float4x4 inverseViewProjection;
    row_major float4x4 previousViewProjection;
    row_major float4x4 previousInverseViewProjection;

    uint correctionIndex;
    uint previousHistoryIndex;
    uint outputHistoryIndex;
    uint depthIndex;
    uint normalIndex;
    uint previousDepthIndex;
    uint previousNormalIndex;
    uint historyValid;

    uint fullWidth;
    uint fullHeight;
    uint giWidth;
    uint giHeight;
};

ConstantBuffer<GISignedCorrectionConstants> constants : register(b0);

bool ReconstructPosition(
    uint2 pixel,
    float depth,
    float4x4 inverseVP,
    out float3 position)
{
    position = 0.0f;

    if(!isfinite(depth) || depth < 0.0f || depth >= 1.0f)
    {
        return false;
    }

    float2 uv = (float2(pixel) + 0.5f) / float2(constants.fullWidth, constants.fullHeight);
    float4 world = mul(
        inverseVP,
        float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f));

    if(!all(isfinite(world)) || abs(world.w) < 1.0e-8f)
    {
        return false;
    }

    position = world.xyz / world.w;
    return all(isfinite(position));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;

    if(any(pixel >= uint2(constants.giWidth, constants.giHeight)))
    {
        return;
    }

    RWTexture2D<float4> outputHistory = ResourceDescriptorHeap[constants.outputHistoryIndex];
    outputHistory[pixel] = 0.0f;

    if(constants.fullWidth == 0u || constants.fullHeight == 0u)
    {
        return;
    }

    // Match GI's top-left representative full-resolution pixel, including odd sizes.
    uint2 gbufferPixel = min(
        pixel * 2u,
        uint2(constants.fullWidth, constants.fullHeight) - 1u);

    Texture2D<float> depthTexture = ResourceDescriptorHeap[constants.depthIndex];

    Texture2D<float4> normalTexture = ResourceDescriptorHeap[constants.normalIndex];
    float depth = depthTexture.Load(int3(gbufferPixel, 0));
    float3 normal = normalTexture.Load(int3(gbufferPixel, 0)).rgb;
    float normalLength = dot(normal, normal);

    if(!isfinite(depth) || depth < 0.0f || depth >= 1.0f ||
       !all(isfinite(normal)) || !isfinite(normalLength) || normalLength < 1.0e-8f)
    {
        return;
    }

    Texture2D<float4> correctionTexture = ResourceDescriptorHeap[constants.correctionIndex];
    float4 signedSample = correctionTexture.Load(int3(pixel, 0));

    if(!all(isfinite(signedSample)) || signedSample.a != 1.0f)
    {
        return;
    }

    float3 correction = signedSample.rgb;

    // A valid current sample always survives rejected history without any RGB clipping.
    outputHistory[pixel] = float4(correction, 1.0f);

    if(constants.historyValid == 0u)
    {
        return;
    }

    float3 position;

    if(!ReconstructPosition(gbufferPixel, depth, constants.inverseViewProjection, position))
    {
        return;
    }

    float4 clip = mul(constants.previousViewProjection, float4(position, 1.0f));

    if(!all(isfinite(clip)) || clip.w < 1.0e-8f)
    {
        return;
    }

    float3 ndc = clip.xyz / clip.w;
    float2 previousUV = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);

    if(!all(isfinite(ndc)) || ndc.z < 0.0f || ndc.z > 1.0f ||
       any(previousUV < 0.0f) || any(previousUV >= 1.0f))
    {
        return;
    }

    // Half-resolution history centers represent full-resolution centers at (2*x, 2*y).
    float2 previousPixel = (previousUV * float2(constants.fullWidth, constants.fullHeight) - 0.5f) * 0.5f;
    int2 basePixel = int2(floor(previousPixel));

    Texture2D<float4> historyTexture = ResourceDescriptorHeap[constants.previousHistoryIndex];

    Texture2D<float> previousDepth = ResourceDescriptorHeap[constants.previousDepthIndex];

    Texture2D<float4> previousNormal = ResourceDescriptorHeap[constants.previousNormalIndex];
    normal = normalize(normal);
    float4 selectedHistory = 0.0f;
    float bestGeometryScore = 0.0f;
    float bestScreenDistance = 0.0f;
    bool foundHistory = false;

    for(int y = 0; y < 2; ++y)
    {
        for(int x = 0; x < 2; ++x)
        {
            int2 candidate = basePixel + int2(x, y);
            if(any(candidate < 0) ||
               any(candidate >= int2(constants.giWidth, constants.giHeight)))
            {
                continue;
            }

            uint2 representative = min(uint2(candidate) * 2u,
                uint2(constants.fullWidth, constants.fullHeight) - 1u);
            float3 previousPosition;

            if(!ReconstructPosition(representative, previousDepth.Load(int3(representative, 0)),
                constants.previousInverseViewProjection, previousPosition))
            {
                continue;
            }

            float3 difference = previousPosition - position;
            float planeDistance = abs(dot(difference, normal));
            float worldDistance = length(difference);

            // Start with AO's geometry rejection thresholds, plus a normal discontinuity test.

            if(!isfinite(planeDistance) || !isfinite(worldDistance) ||
               planeDistance > 0.01f || worldDistance > 0.10f)
            {
                continue;
            }

            float3 candidateNormal = previousNormal.Load(int3(representative, 0)).rgb;
            float lengthSquared = dot(candidateNormal, candidateNormal);

            if(!all(isfinite(candidateNormal)) || !isfinite(lengthSquared) || lengthSquared < 1.0e-8f)
            {
                continue;
            }

            float normalSimilarity = dot(normal, normalize(candidateNormal));

            if(normalSimilarity < 0.9f)
            {
                continue;
            }

            float4 history = historyTexture.Load(int3(candidate, 0));

            if(!all(isfinite(history)) || history.a < 1.0f)
            {
                continue;
            }

            // Rank geometry errors relative to their existing rejection thresholds.
            // Screen-space proximity only resolves geometrically equivalent taps.
            float geometryScore = planeDistance / 0.01f + worldDistance / 0.10f +
                (1.0f - saturate(normalSimilarity)) / 0.10f;
            float2 screenOffset = float2(candidate) - previousPixel;
            float screenDistance = dot(screenOffset, screenOffset);
            bool geometryTie = abs(geometryScore - bestGeometryScore) <= 1.0e-6f;

            if(!foundHistory || geometryScore < bestGeometryScore - 1.0e-6f ||
               (geometryTie && screenDistance < bestScreenDistance))
            {
                selectedHistory = history;
                bestGeometryScore = geometryScore;
                bestScreenDistance = screenDistance;
                foundHistory = true;
            }
        }
    }

    if(!foundHistory)
    {
        return;
    }

    float historyLength = min(selectedHistory.a + 1.0f, 16.0f);
    float3 accumulated = lerp(selectedHistory.rgb, correction, 1.0f / historyLength);

    if(all(isfinite(accumulated)))
    {
        outputHistory[pixel] = float4(accumulated, historyLength);
    }
}
