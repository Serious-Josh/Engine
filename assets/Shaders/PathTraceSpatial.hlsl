// RTXPT D.3B: one 5x5 variance-guided, edge-aware spatial filtering iteration over the temporal pass's outputs
// (PathTraceSpatialPass). One thread per pixel, surface pixels only (background pixels keep the presentation):
//
// 1. Guides. Camera-relative position and view depth from hardware depth and the inverse view projection (as the
//    temporal pass), geometric normal, shading normal, material index (G-buffer surface.w), roughness.
// 2. Compatibility of a neighbour q with the centre p. Hard rejection: no surface, another material index, or a
//    geometric-normal dot below the threshold. Continuous geometry weight g = w_z * w_n with
//    w_z = exp(-|dot(n_p, x_q - x_p)| / (depthSigma * z_p))   (plane distance relative to view depth),
//    w_n = max(dot(s_p, s_q), 0)^normalPower                   (shading normals).
// 3. Uncertainty of the reconstructed value, per signal (diffuse: demodulated lighting; specular: radiance), from a
//    3x3 neighbourhood weighted by g (D.2C.3 moments, D.3A section 5):
//    temporal:  sigma^2_t = mean_g( max(M2_q - Y_q^2, 0) / Neff_q ),  Neff = N while growing, 2 * cap - 1 at the
//               diffuse cap of 32; specular uses Neff = N (its cap varies per pixel);
//    spatial:   sigma^2_s = mean_g(Y^2) - mean_g(Y)^2 of the temporal outputs, which are already history averages
//               (the young-history fallback);
//    sigma^2 = lerp(sigma^2_s, sigma^2_t, saturate((N_p - 1) / 7)): history 1 uses the neighbourhood only, 8 and
//    beyond the moments only. Without at least two compatible neighbours there is no reliable estimate, and the
//    pixel keeps its temporal value.
// 4. Filter, per signal: binomial 5x5 weights h ([1 4 6 4 1] / 16, squared) times g times the luminance stop
//    w_l = exp(-|Y_p - Y_q| / (luminanceSigma * sigma + epsilon)); out = sum(w x_q) / sum(w). The centre always
//    contributes (h > 0, g = w_l = 1), so the weights never collapse. Specular is filtered only at roughness >=
//    specularRoughnessMinimum, with neighbours of roughness within the tolerance, the normal power raised to
//    max(normalPower, 8 / r^2), and the non-centre taps scaled by saturate((r - min) / min): no filtering at the
//    threshold, the full kernel from twice it. Below the threshold (near-mirror) specular is unchanged.
// 5. Presentation: filtered diffuse lighting times this pixel's diffuse albedo, max(baseColor * (1 - metallic),
//    0.02) as the temporal pass demodulates, plus filtered specular. A non-finite result keeps the temporal value.
// 6. RTXPT D.3B.0 diagnostic views (entry point CSDiagnostic; CSMain compiles without them, so the normal path is the
//    D.3B code exactly), computed from the same values in the same dispatch, written to the presentation instead of
//    step 5; background pixels become black. Signal 0 is diffuse lighting
//    (demodulated), 1 specular radiance. Radiance views are linear and go through the normal tone map; scalar views
//    (3, 4, 5) are display-encoded colours shown without tone mapping (ToneMapDebugView::PathTraceDenoiserScalar).
//    1 temporal: the temporal output x exposure.     2 spatial: the step 4 result x exposure.
//    3 difference: |Y(spatial) - Y(temporal)| x exposure x amplification, grey, sRGB-encoded, clipped at 1. It shows
//      what filtering changed, not error.
//    4 uncertainty: the sigma step 4 used, relative to Y(temporal), log10 mapped from the floor (blue) through green
//      to 1 (red); history 1 (no information): magenta; no estimate (centre-only fallback): cyan; history 2-7:
//      darkened diagonal stripes. It is the filter's estimate, not error.
//    5 history: the stored history length; 1 red, 2-7 orange to yellow, 8 and beyond yellow-green to green at 32.
//    6 raw: this frame's extraction (diffuse divided by the same albedo, so in the temporal representation) x exposure.
//
// All guides and signals are read from the group tile (see PathTraceSpatialStoreTile). Nothing here is written back
// to the temporal history.

struct PathTraceSpatialConstants
{
    row_major float4x4 inverseCameraRelativeViewProjection;

    float3 cameraForward;
    float depthSigma;

    uint2 resolution;
    uint diffuseIndex;
    uint specularIndex;

    uint momentsIndex;
    uint depthIndex;
    uint surfaceIndex;
    uint geometricNormalIndex;

    uint materialIndex;
    uint outputIndex;
    float luminanceSigma;
    float normalPower;

    float geometricNormalThreshold;
    float specularRoughnessMinimum;
    float specularRoughnessTolerance;
    float diffuseMaximumHistory;

    uint visualizationMode;
    uint visualizationSignal;
    float exposureScale;
    float differenceScale;

    uint rawDiffuseIndex;
    uint rawSpecularIndex;
    float uncertaintyFloorLog10;
    uint padding;
};

ConstantBuffer<PathTraceSpatialConstants> constants : register(b0);

static const float PATH_TRACE_SPATIAL_HALF_MAXIMUM = 65504.0f;
// The G-buffer stores the material index as fp16, exact up to 2048 (PathTracePrimarySurface.hlsli).
static const float PATH_TRACE_SPATIAL_MAXIMUM_MATERIALS = 2048.0f;
// The temporal pass's diffuse demodulation floor (PathTraceTemporal.hlsl).
static const float PATH_TRACE_SPATIAL_MINIMUM_ALBEDO = 0.02f;
static const float PATH_TRACE_SPATIAL_LUMINANCE_EPSILON = 1.0e-8f;

struct PathTraceSpatialSample
{
    bool valid;
    float3 position;
    float viewDepth;
    float3 geometricNormal;
    float3 shadingNormal;
    float materialIndex;
    float roughness;
    float4 diffuse;
    float4 specular;
    float2 moments;
};

// Rec. 709 luminance, the definition used throughout the path tracer (PathTraceLuminance).
float PathTraceSpatialLuminance(float3 value)
{
    return dot(value, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 PathTraceSpatialNormal(float3 normal)
{
    float lengthSquared = dot(normal, normal);
    return lengthSquared > 1.0e-12f ? normal * rsqrt(lengthSquared) : float3(0.0f, 0.0f, 0.0f);
}

PathTraceSpatialSample PathTraceSpatialLoad(int2 pixel)
{
    PathTraceSpatialSample result = (PathTraceSpatialSample)0;

    if(any(pixel < 0) || any(pixel >= int2(constants.resolution)))
    {
        return result;
    }

    Texture2D<float> depthTexture = ResourceDescriptorHeap[constants.depthIndex];
    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[constants.surfaceIndex];
    float depth = depthTexture.Load(int3(pixel, 0));
    float4 surface = surfaceTexture.Load(int3(pixel, 0));
    bool exactMaterial = isfinite(surface.w) && surface.w >= 0.0f && surface.w == floor(surface.w) &&
        surface.w < PATH_TRACE_SPATIAL_MAXIMUM_MATERIALS;

    if(!(depth < 1.0f) || !(depth >= 0.0f) || !exactMaterial)
    {
        return result;
    }

    Texture2D<float4> geometricNormalTexture = ResourceDescriptorHeap[constants.geometricNormalIndex];
    Texture2D<float4> materialTexture = ResourceDescriptorHeap[constants.materialIndex];
    Texture2D<float4> diffuseTexture = ResourceDescriptorHeap[constants.diffuseIndex];
    Texture2D<float4> specularTexture = ResourceDescriptorHeap[constants.specularIndex];
    Texture2D<float2> momentsTexture = ResourceDescriptorHeap[constants.momentsIndex];

    float2 uv = (float2(pixel) + 0.5f) / float2(constants.resolution);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 relative = mul(constants.inverseCameraRelativeViewProjection, float4(ndc, depth, 1.0f));

    result.position = relative.xyz / relative.w;
    result.viewDepth = dot(constants.cameraForward, result.position);
    result.geometricNormal = PathTraceSpatialNormal(geometricNormalTexture.Load(int3(pixel, 0)).xyz);
    result.shadingNormal = PathTraceSpatialNormal(surface.xyz);
    result.materialIndex = surface.w;
    result.roughness = materialTexture.Load(int3(pixel, 0)).a;
    result.diffuse = diffuseTexture.Load(int3(pixel, 0));
    result.specular = specularTexture.Load(int3(pixel, 0));
    result.moments = momentsTexture.Load(int3(pixel, 0));
    result.valid = all(isfinite(result.position)) && result.viewDepth > 0.0f && all(isfinite(result.diffuse)) &&
        all(isfinite(result.specular)) && all(isfinite(result.moments)) && result.diffuse.a >= 1.0f &&
        result.specular.a >= 1.0f;
    return result;
}

// The geometry weight g of a neighbour (step 2); zero when it is rejected.
float PathTraceSpatialGeometryWeight(PathTraceSpatialSample centre, PathTraceSpatialSample neighbour,
    float normalPower)
{
    if(!neighbour.valid || neighbour.materialIndex != centre.materialIndex ||
        dot(centre.geometricNormal, neighbour.geometricNormal) < constants.geometricNormalThreshold)
    {
        return 0.0f;
    }

    float planeDistance = abs(dot(centre.geometricNormal, neighbour.position - centre.position));
    float depthWeight = exp(-planeDistance / (constants.depthSigma * centre.viewDepth));
    float normalWeight = pow(saturate(dot(centre.shadingNormal, neighbour.shadingNormal)), normalPower);
    return depthWeight * normalWeight;
}

// RTXPT D.3B.2 group tile: the 8x8 thread group's 12x12 neighbourhood (its 8x8 pixels plus the 5x5 kernel's 2-pixel
// halo on every side), one entry per texel, loaded once per group by PathTraceSpatialLoad (bounds-tested: texels
// outside the image are stored invalid) and read by both the 3x3 and 5x5 loops. Tied to [numthreads(8, 8, 1)]:
// 144 texels over 64 threads, origin = group * 8 - 2. Every value is the one PathTraceSpatialLoad returns, so the
// filter's arithmetic is unchanged (RTXPT D.3B.1 measured bit-identical output).
static const int PATH_TRACE_SPATIAL_TILE = 12;
groupshared float4 g_tileDiffuse[144];
groupshared float4 g_tileSpecular[144];
groupshared float4 g_tilePosition[144];        // position, view depth
groupshared float4 g_tileGeometric[144];       // geometric normal, valid (1 / 0)
groupshared float4 g_tileShading[144];         // shading normal, material index
groupshared float4 g_tileMoments[144];         // moments, roughness, unused
static int2 g_tileOrigin;

void PathTraceSpatialStoreTile(uint index, PathTraceSpatialSample value)
{
    g_tileDiffuse[index] = value.diffuse;
    g_tileSpecular[index] = value.specular;
    g_tilePosition[index] = float4(value.position, value.viewDepth);
    g_tileGeometric[index] = float4(value.geometricNormal, value.valid ? 1.0f : 0.0f);
    g_tileShading[index] = float4(value.shadingNormal, value.materialIndex);
    g_tileMoments[index] = float4(value.moments, value.roughness, 0.0f);
}

PathTraceSpatialSample PathTraceSpatialTile(int2 pixel)
{
    int2 local = pixel - g_tileOrigin;
    uint index = uint(local.y * PATH_TRACE_SPATIAL_TILE + local.x);
    PathTraceSpatialSample result;
    result.diffuse = g_tileDiffuse[index];
    result.specular = g_tileSpecular[index];
    result.position = g_tilePosition[index].xyz;
    result.viewDepth = g_tilePosition[index].w;
    result.geometricNormal = g_tileGeometric[index].xyz;
    result.valid = g_tileGeometric[index].w > 0.5f;
    result.shadingNormal = g_tileShading[index].xyz;
    result.materialIndex = g_tileShading[index].w;
    result.moments = g_tileMoments[index].xy;
    result.roughness = g_tileMoments[index].z;
    return result;
}

// sRGB transfer of a linear value in [0, 1] (scalar diagnostic views are shown without tone mapping).
float3 PathTraceSpatialToSRGB(float3 linearValue)
{
    float3 value = saturate(linearValue);
    return select(value <= 0.0031308f, value * 12.92f, 1.055f * pow(value, 1.0f / 2.4f) - 0.055f);
}

// Effective sample count of a history of length N under the capped moving average.
float PathTraceSpatialEffectiveCount(float length, float cap)
{
    return length < cap ? length : 2.0f * cap - 1.0f;
}

void PathTraceSpatialFilter(uint3 dispatchThreadId, uint3 groupThreadId, uint3 groupId, bool diagnostics)
{
    int2 pixel = int2(dispatchThreadId.xy);
    // Every thread, in or out of the image, helps load the tile and reaches the barrier before any early return.
    g_tileOrigin = int2(groupId.xy) * 8 - 2;

    for(uint texel = groupThreadId.y * 8u + groupThreadId.x; texel < 144u; texel += 64u)
    {
        int2 tilePixel = g_tileOrigin + int2(texel % 12u, texel / 12u);
        PathTraceSpatialStoreTile(texel, PathTraceSpatialLoad(tilePixel));
    }

    GroupMemoryBarrierWithGroupSync();

    if(any(pixel >= int2(constants.resolution)))
    {
        return;
    }

    PathTraceSpatialSample centre = PathTraceSpatialTile(pixel);

    // Background or no temporal value: the presentation keeps what is there (diagnostic views: black).
    if(!centre.valid)
    {
        if(diagnostics)
        {
            RWTexture2D<float4> diagnosticOutput = ResourceDescriptorHeap[constants.outputIndex];
            diagnosticOutput[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        }

        return;
    }

    Texture2D<float4> materialTexture = ResourceDescriptorHeap[constants.materialIndex];
    Texture2D<float4> geometricNormalTexture = ResourceDescriptorHeap[constants.geometricNormalIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[constants.outputIndex];

    float roughness = centre.roughness;
    bool filterSpecular = roughness >= constants.specularRoughnessMinimum;
    float specularNormalPower = max(constants.normalPower, 8.0f / max(roughness * roughness, 1.0e-4f));
    float specularRingScale = saturate((roughness - constants.specularRoughnessMinimum) /
        constants.specularRoughnessMinimum);

    // Step 3: 3x3 uncertainty, per signal ([0] diffuse, [1] specular).
    float neighbourWeight[2] = {0.0f, 0.0f};
    float neighbourCount[2] = {0.0f, 0.0f};
    float luminanceSum[2] = {0.0f, 0.0f};
    float luminanceSquaredSum[2] = {0.0f, 0.0f};
    float temporalVarianceSum[2] = {0.0f, 0.0f};

    for(int y = -1; y <= 1; ++y)
    {
        for(int x = -1; x <= 1; ++x)
        {
            PathTraceSpatialSample neighbour = PathTraceSpatialTile(pixel + int2(x, y));

            for(uint signal = 0u; signal < 2u; ++signal)
            {
                float weight = (x == 0 && y == 0) ? 1.0f : PathTraceSpatialGeometryWeight(centre, neighbour,
                    signal == 0u ? constants.normalPower : specularNormalPower);

                if(signal == 1u && abs(neighbour.roughness - roughness) > constants.specularRoughnessTolerance)
                {
                    weight = 0.0f;
                }

                if(weight <= 0.0f)
                {
                    continue;
                }

                float4 value = signal == 0u ? neighbour.diffuse : neighbour.specular;
                float moment = signal == 0u ? neighbour.moments.x : neighbour.moments.y;
                float luminance = PathTraceSpatialLuminance(value.rgb);
                float cap = signal == 0u ? constants.diffuseMaximumHistory : 1.0e30f;
                float sampleVariance = max(moment - luminance * luminance, 0.0f);

                neighbourWeight[signal] += weight;
                neighbourCount[signal] += (x == 0 && y == 0) ? 0.0f : 1.0f;
                luminanceSum[signal] += weight * luminance;
                luminanceSquaredSum[signal] += weight * luminance * luminance;
                temporalVarianceSum[signal] += weight * sampleVariance / PathTraceSpatialEffectiveCount(value.a, cap);
            }
        }
    }

    float sigma[2];
    bool reliable[2];
    float centreLength[2] = {centre.diffuse.a, centre.specular.a};

    for(uint signal = 0u; signal < 2u; ++signal)
    {
        float meanLuminance = luminanceSum[signal] / neighbourWeight[signal];
        float spatialVariance = max(luminanceSquaredSum[signal] / neighbourWeight[signal] -
            meanLuminance * meanLuminance, 0.0f);
        // The neighbours' temporal outputs are already history averages: their spread is the uncertainty of the
        // reconstructed value directly, with no further division by the history length.
        float temporalEstimate = temporalVarianceSum[signal] / neighbourWeight[signal];
        float variance = lerp(spatialVariance, temporalEstimate, saturate((centreLength[signal] - 1.0f) / 7.0f));
        reliable[signal] = neighbourCount[signal] >= 2.0f && isfinite(variance);
        sigma[signal] = sqrt(max(variance, 0.0f));
    }

    // Step 4: 5x5 filter.
    static const float binomial[5] = {1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f};
    float centreLuminance[2] = {PathTraceSpatialLuminance(centre.diffuse.rgb),
        PathTraceSpatialLuminance(centre.specular.rgb)};
    bool filter[2] = {reliable[0], reliable[1] && filterSpecular};
    float3 sum[2] = {float3(0.0f, 0.0f, 0.0f), float3(0.0f, 0.0f, 0.0f)};
    float weightSum[2] = {0.0f, 0.0f};

    for(int y = -2; y <= 2; ++y)
    {
        for(int x = -2; x <= 2; ++x)
        {
            bool isCentre = x == 0 && y == 0;
            PathTraceSpatialSample neighbour = PathTraceSpatialTile(pixel + int2(x, y));
            float kernel = binomial[x + 2] * binomial[y + 2];

            for(uint signal = 0u; signal < 2u; ++signal)
            {
                if(!filter[signal] && !isCentre)
                {
                    continue;
                }

                float weight = kernel;

                if(!isCentre)
                {
                    weight *= PathTraceSpatialGeometryWeight(centre, neighbour,
                        signal == 0u ? constants.normalPower : specularNormalPower);

                    if(signal == 1u)
                    {
                        bool similarRoughness = abs(neighbour.roughness - roughness) <=
                            constants.specularRoughnessTolerance;
                        weight *= similarRoughness ? specularRingScale : 0.0f;
                    }
                }

                if(weight <= 0.0f)
                {
                    continue;
                }

                float3 value = signal == 0u ? neighbour.diffuse.rgb : neighbour.specular.rgb;

                if(!isCentre)
                {
                    float difference = abs(centreLuminance[signal] - PathTraceSpatialLuminance(value));
                    weight *= exp(-difference / (constants.luminanceSigma * sigma[signal] +
                        PATH_TRACE_SPATIAL_LUMINANCE_EPSILON));
                }

                sum[signal] += weight * value;
                weightSum[signal] += weight;
            }
        }
    }

    float3 lighting = sum[0] / weightSum[0];
    float3 specular = sum[1] / weightSum[1];

    // Step 6: diagnostic views.
    if(diagnostics)
    {
        uint signal = min(constants.visualizationSignal, 1u);
        float3 temporalValue = signal == 0u ? centre.diffuse.rgb : centre.specular.rgb;
        float3 spatialValue = signal == 0u ? lighting : specular;
        float temporalLuminance = PathTraceSpatialLuminance(temporalValue);
        float3 diagnostic = float3(0.0f, 0.0f, 0.0f);

        if(constants.visualizationMode == 1u)
        {
            diagnostic = temporalValue * constants.exposureScale;
        }
        else if(constants.visualizationMode == 2u)
        {
            diagnostic = spatialValue * constants.exposureScale;
        }
        else if(constants.visualizationMode == 3u)
        {
            float difference = abs(PathTraceSpatialLuminance(spatialValue) - temporalLuminance) *
                constants.exposureScale * constants.differenceScale;
            diagnostic = PathTraceSpatialToSRGB(difference.xxx);
        }
        else if(constants.visualizationMode == 4u)
        {
            float length = centreLength[signal];

            if(length <= 1.0f)
            {
                diagnostic = float3(1.0f, 0.0f, 1.0f);
            }
            else if(!reliable[signal])
            {
                diagnostic = float3(0.0f, 1.0f, 1.0f);
            }
            else
            {
                float relative = sigma[signal] / max(temporalLuminance, 1.0e-6f);
                float t = saturate((log10(max(relative, 1.0e-12f)) - constants.uncertaintyFloorLog10) /
                    -constants.uncertaintyFloorLog10);
                float3 low = float3(0.05f, 0.15f, 0.75f);
                float3 middle = float3(0.1f, 0.85f, 0.2f);
                float3 high = float3(1.0f, 0.15f, 0.05f);
                diagnostic = t < 0.5f ? lerp(low, middle, t * 2.0f) : lerp(middle, high, t * 2.0f - 1.0f);

                if(length < 8.0f && ((pixel.x + pixel.y) / 4) % 2 == 0)
                {
                    diagnostic *= 0.4f;
                }
            }
        }
        else if(constants.visualizationMode == 5u)
        {
            float length = centreLength[signal];

            if(length <= 1.0f)
            {
                diagnostic = float3(1.0f, 0.0f, 0.0f);
            }
            else if(length < 8.0f)
            {
                diagnostic = lerp(float3(1.0f, 0.45f, 0.0f), float3(1.0f, 1.0f, 0.0f), (length - 2.0f) / 5.0f);
            }
            else
            {
                diagnostic = lerp(float3(0.55f, 0.8f, 0.1f), float3(0.0f, 1.0f, 0.2f),
                    saturate((length - 8.0f) / 24.0f));
            }
        }
        else if(constants.visualizationMode == 6u)
        {
            Texture2D<float4> rawTexture = ResourceDescriptorHeap[signal == 0u ? constants.rawDiffuseIndex :
                constants.rawSpecularIndex];
            float3 raw = rawTexture.Load(int3(pixel, 0)).rgb;

            if(signal == 0u)
            {
                float4 rawMaterial = materialTexture.Load(int3(pixel, 0));
                float rawMetallic = geometricNormalTexture.Load(int3(pixel, 0)).w;
                raw /= max(rawMaterial.rgb * (1.0f - saturate(rawMetallic)), PATH_TRACE_SPATIAL_MINIMUM_ALBEDO);
            }

            diagnostic = raw * constants.exposureScale;
        }

        diagnostic = all(isfinite(diagnostic)) ? diagnostic : float3(0.0f, 0.0f, 0.0f);
        output[pixel] = float4(min(diagnostic, PATH_TRACE_SPATIAL_HALF_MAXIMUM), 1.0f);
        return;
    }

    // Step 5: remodulate with this pixel's diffuse albedo.
    float4 material = materialTexture.Load(int3(pixel, 0));
    float metallic = geometricNormalTexture.Load(int3(pixel, 0)).w;
    float3 diffuseAlbedo = max(material.rgb * (1.0f - saturate(metallic)), PATH_TRACE_SPATIAL_MINIMUM_ALBEDO);
    float3 radiance = lighting * diffuseAlbedo + specular;

    if(!all(isfinite(radiance)))
    {
        radiance = centre.diffuse.rgb * diffuseAlbedo + centre.specular.rgb;
    }

    output[pixel] = float4(min(radiance, PATH_TRACE_SPATIAL_HALF_MAXIMUM), 1.0f);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID)
{
    PathTraceSpatialFilter(dispatchThreadId, groupThreadId, groupId, false);
}

// RTXPT D.3B.0 diagnostic views (constants.visualizationMode 1-6).
[numthreads(8, 8, 1)]
void CSDiagnostic(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID)
{
    PathTraceSpatialFilter(dispatchThreadId, groupThreadId, groupId, true);
}
