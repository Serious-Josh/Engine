// RTXPT D.3C.2: variance-guided 3x3 a-trous spatial filtering, one or two iterations (steps 1 and 2), over the
// temporal pass's outputs (PathTraceSpatialPass). The D.3B.2 single 5x5 pass (PathTraceSpatial.hlsl) stays the
// zero-iteration path and the fallback; this file follows the D.3C.1 replica (build/rtxpt-d3c1/scripts/d3c1_lib.py).
//
// Shared per pixel: guides (camera-relative position and view depth from hardware depth and the inverse view
// projection, geometric normal, shading normal, material index, roughness) exactly as PathTraceSpatial.hlsl; the
// geometry weight g of a neighbour (hard rejection: invalid, another material, geometric-normal dot below the
// threshold; continuous: depth-plane distance and shading-normal power); specular gated by roughness with the ring
// taper and the roughness tolerance.
//
// Iteration 1 (step 1, CSFirst / CSFirstPresent): the initial variance V of each pixel is D.3B step 3 (3x3
// geometry-weighted prefilter with the young-history spatial fallback), computed for the whole 10x10 neighbourhood of
// the group into group-shared memory so that every neighbour's own V is available. Then per signal the 3x3 binomial
// ([1 2 1] / 4)^2 filter with w = h * g * exp(-|Y_p - Y_q| / (luminanceSigma * sqrt(V_p) + 1e-8)), Y the temporal
// luminance; out = sum(w x_q) / sum(w); propagated V_out = sum(w^2 V_q) / (sum w)^2. A pixel without an estimate
// (fewer than two compatible neighbours) keeps its value (centre only), as D.3B.
// Iteration 2 (step 2, CSSecond): the same filter at offsets of 2 on iteration 1's output, guided by its luminance and
// propagated V (SVGF), with the geometry from the original G-buffer.
//
// Intermediate (iteration 1 -> 2): per signal one RGBA16F texture, RGB the filtered value (diffuse demodulated,
// specular radiance) and A the standard deviation sqrt(V_out) with the sign as a flag: +sigma estimate available;
// -sigma (including -0) no estimate, tested by the sign bit, never by comparison; -infinity no valid surface.
//
// Final iteration (CSFirstPresent with one iteration, CSSecond with two): diffuse lighting times this pixel's diffuse
// albedo max(baseColor * (1 - metallic), 0.02), plus specular, to the presentation on surface pixels (background
// untouched). The diagnostic entry points (CSFirstPresentDiagnostic, CSSecondDiagnostic) instead write D.3B.0 views 2
// (spatial radiance) and 3 (spatial - temporal difference) of the final iteration; the other views come from the
// D.3B.2 diagnostic entry point. Nothing here writes the temporal history.

struct PathTraceAtrousConstants
{
    row_major float4x4 inverseCameraRelativeViewProjection;

    float3 cameraForward;
    float depthSigma;

    uint2 resolution;
    uint temporalDiffuseIndex;
    uint temporalSpecularIndex;

    uint momentsIndex;
    uint depthIndex;
    uint surfaceIndex;
    uint geometricNormalIndex;

    uint materialIndex;
    // Shader-resource views when an iteration reads the intermediate, unordered-access views when it writes it.
    uint intermediateDiffuseIndex;
    uint intermediateSpecularIndex;
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
    uint2 padding;
};

ConstantBuffer<PathTraceAtrousConstants> constants : register(b0);

static const float PATH_TRACE_ATROUS_HALF_MAXIMUM = 65504.0f;
static const float PATH_TRACE_ATROUS_MAXIMUM_MATERIALS = 2048.0f;
static const float PATH_TRACE_ATROUS_MINIMUM_ALBEDO = 0.02f;
static const float PATH_TRACE_ATROUS_LUMINANCE_EPSILON = 1.0e-8f;
static const uint PATH_TRACE_ATROUS_SIGN_BIT = 0x80000000u;

// One tile texel: guides and the two signals with their variance (iteration 1: temporal value and moments; iteration
// 2: the intermediate value and its propagated variance).
struct PathTraceAtrousSample
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

float PathTraceAtrousLuminance(float3 value)
{
    return dot(value, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 PathTraceAtrousNormal(float3 normal)
{
    float lengthSquared = dot(normal, normal);
    return lengthSquared > 1.0e-12f ? normal * rsqrt(lengthSquared) : float3(0.0f, 0.0f, 0.0f);
}

float PathTraceAtrousEffectiveCount(float length, float cap)
{
    return length < cap ? length : 2.0f * cap - 1.0f;
}

// The intermediate alpha: +sigma (estimate), -sigma (no estimate, sign bit set even for zero), -infinity (no surface).
float PathTraceAtrousEncodeSigma(float sigma, bool reliable)
{
    return reliable ? sigma : asfloat(asuint(sigma) | PATH_TRACE_ATROUS_SIGN_BIT);
}

bool PathTraceAtrousReliable(float encoded)
{
    return (asuint(encoded) & PATH_TRACE_ATROUS_SIGN_BIT) == 0u;
}

// Guides of a pixel, as PathTraceSpatialLoad: invalid outside the image, without a surface or with an inexact
// material index.
PathTraceAtrousSample PathTraceAtrousLoadGuides(int2 pixel)
{
    PathTraceAtrousSample result = (PathTraceAtrousSample)0;

    if(any(pixel < 0) || any(pixel >= int2(constants.resolution)))
    {
        return result;
    }

    Texture2D<float> depthTexture = ResourceDescriptorHeap[constants.depthIndex];
    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[constants.surfaceIndex];
    float depth = depthTexture.Load(int3(pixel, 0));
    float4 surface = surfaceTexture.Load(int3(pixel, 0));
    bool exactMaterial = isfinite(surface.w) && surface.w >= 0.0f && surface.w == floor(surface.w) &&
        surface.w < PATH_TRACE_ATROUS_MAXIMUM_MATERIALS;

    if(!(depth < 1.0f) || !(depth >= 0.0f) || !exactMaterial)
    {
        return result;
    }

    Texture2D<float4> geometricNormalTexture = ResourceDescriptorHeap[constants.geometricNormalIndex];
    Texture2D<float4> materialTexture = ResourceDescriptorHeap[constants.materialIndex];

    float2 uv = (float2(pixel) + 0.5f) / float2(constants.resolution);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 relative = mul(constants.inverseCameraRelativeViewProjection, float4(ndc, depth, 1.0f));

    result.position = relative.xyz / relative.w;
    result.viewDepth = dot(constants.cameraForward, result.position);
    result.geometricNormal = PathTraceAtrousNormal(geometricNormalTexture.Load(int3(pixel, 0)).xyz);
    result.shadingNormal = PathTraceAtrousNormal(surface.xyz);
    result.materialIndex = surface.w;
    result.roughness = materialTexture.Load(int3(pixel, 0)).a;
    result.valid = all(isfinite(result.position)) && result.viewDepth > 0.0f;
    return result;
}

// Iteration 1 input: the temporal outputs and moments, valid as in PathTraceSpatialLoad.
PathTraceAtrousSample PathTraceAtrousLoadTemporal(int2 pixel)
{
    PathTraceAtrousSample result = PathTraceAtrousLoadGuides(pixel);

    if(!result.valid)
    {
        return result;
    }

    Texture2D<float4> diffuseTexture = ResourceDescriptorHeap[constants.temporalDiffuseIndex];
    Texture2D<float4> specularTexture = ResourceDescriptorHeap[constants.temporalSpecularIndex];
    Texture2D<float2> momentsTexture = ResourceDescriptorHeap[constants.momentsIndex];
    result.diffuse = diffuseTexture.Load(int3(pixel, 0));
    result.specular = specularTexture.Load(int3(pixel, 0));
    result.moments = momentsTexture.Load(int3(pixel, 0));
    result.valid = all(isfinite(result.diffuse)) && all(isfinite(result.specular)) && all(isfinite(result.moments)) &&
        result.diffuse.a >= 1.0f && result.specular.a >= 1.0f;
    return result;
}

// Iteration 2 input: the intermediate, valid where iteration 1 found a surface (alpha not -infinity).
PathTraceAtrousSample PathTraceAtrousLoadIntermediate(int2 pixel)
{
    PathTraceAtrousSample result = PathTraceAtrousLoadGuides(pixel);

    if(!result.valid)
    {
        return result;
    }

    Texture2D<float4> diffuseTexture = ResourceDescriptorHeap[constants.intermediateDiffuseIndex];
    Texture2D<float4> specularTexture = ResourceDescriptorHeap[constants.intermediateSpecularIndex];
    result.diffuse = diffuseTexture.Load(int3(pixel, 0));
    result.specular = specularTexture.Load(int3(pixel, 0));
    result.valid = all(isfinite(result.diffuse)) && all(isfinite(result.specular));
    return result;
}

float PathTraceAtrousGeometryWeight(PathTraceAtrousSample centre, PathTraceAtrousSample neighbour, float normalPower)
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

float3 PathTraceAtrousToSRGB(float3 linearValue)
{
    float3 value = saturate(linearValue);
    return select(value <= 0.0031308f, value * 12.92f, 1.055f * pow(value, 1.0f / 2.4f) - 0.055f);
}

// The group's 12x12 neighbourhood (8x8 pixels + 2-pixel halo): iteration 1 needs the variance of every pixel within
// one of the group (10x10), which needs their own 3x3; iteration 2 needs offsets of 2. Tied to [numthreads(8, 8, 1)].
static const int PATH_TRACE_ATROUS_TILE = 12;
groupshared float4 g_atrousDiffuse[144];
groupshared float4 g_atrousSpecular[144];
groupshared float4 g_atrousPosition[144];      // position, view depth
groupshared float4 g_atrousGeometric[144];     // geometric normal, valid (1 / 0)
groupshared float4 g_atrousShading[144];       // shading normal, material index
groupshared float4 g_atrousMoments[144];       // moments, roughness, unused
// Iteration 1: per texel of the inner 10x10, variance diffuse, variance specular, reliable diffuse, reliable specular.
groupshared float4 g_atrousVariance[100];
static int2 g_atrousOrigin;

void PathTraceAtrousStore(uint index, PathTraceAtrousSample value)
{
    g_atrousDiffuse[index] = value.diffuse;
    g_atrousSpecular[index] = value.specular;
    g_atrousPosition[index] = float4(value.position, value.viewDepth);
    g_atrousGeometric[index] = float4(value.geometricNormal, value.valid ? 1.0f : 0.0f);
    g_atrousShading[index] = float4(value.shadingNormal, value.materialIndex);
    g_atrousMoments[index] = float4(value.moments, value.roughness, 0.0f);
}

PathTraceAtrousSample PathTraceAtrousTile(int2 pixel)
{
    int2 local = pixel - g_atrousOrigin;
    uint index = uint(local.y * PATH_TRACE_ATROUS_TILE + local.x);
    PathTraceAtrousSample result;
    result.diffuse = g_atrousDiffuse[index];
    result.specular = g_atrousSpecular[index];
    result.position = g_atrousPosition[index].xyz;
    result.viewDepth = g_atrousPosition[index].w;
    result.geometricNormal = g_atrousGeometric[index].xyz;
    result.valid = g_atrousGeometric[index].w > 0.5f;
    result.shadingNormal = g_atrousShading[index].xyz;
    result.materialIndex = g_atrousShading[index].w;
    result.moments = g_atrousMoments[index].xy;
    result.roughness = g_atrousMoments[index].z;
    return result;
}

// Variance and reliability of the inner-10x10 texel covering pixel (iteration 1).
float4 PathTraceAtrousVariance(int2 pixel)
{
    int2 local = pixel - g_atrousOrigin - 1;
    return g_atrousVariance[uint(local.y * 10 + local.x)];
}

// D.3B step 3 for one pixel: the initial variance of the reconstructed value and its reliability, per signal.
float4 PathTraceAtrousInitialVariance(int2 pixel)
{
    PathTraceAtrousSample centre = PathTraceAtrousTile(pixel);

    if(!centre.valid)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    float roughness = centre.roughness;
    float specularNormalPower = max(constants.normalPower, 8.0f / max(roughness * roughness, 1.0e-4f));
    float neighbourWeight[2] = {0.0f, 0.0f};
    float neighbourCount[2] = {0.0f, 0.0f};
    float luminanceSum[2] = {0.0f, 0.0f};
    float luminanceSquaredSum[2] = {0.0f, 0.0f};
    float temporalVarianceSum[2] = {0.0f, 0.0f};

    for(int y = -1; y <= 1; ++y)
    {
        for(int x = -1; x <= 1; ++x)
        {
            PathTraceAtrousSample neighbour = PathTraceAtrousTile(pixel + int2(x, y));

            for(uint signal = 0u; signal < 2u; ++signal)
            {
                float weight = (x == 0 && y == 0) ? 1.0f : PathTraceAtrousGeometryWeight(centre, neighbour,
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
                float luminance = PathTraceAtrousLuminance(value.rgb);
                float cap = signal == 0u ? constants.diffuseMaximumHistory : 1.0e30f;
                float sampleVariance = max(moment - luminance * luminance, 0.0f);

                neighbourWeight[signal] += weight;
                neighbourCount[signal] += (x == 0 && y == 0) ? 0.0f : 1.0f;
                luminanceSum[signal] += weight * luminance;
                luminanceSquaredSum[signal] += weight * luminance * luminance;
                temporalVarianceSum[signal] += weight * sampleVariance / PathTraceAtrousEffectiveCount(value.a, cap);
            }
        }
    }

    float4 result;
    float centreLength[2] = {centre.diffuse.a, centre.specular.a};

    for(uint signal = 0u; signal < 2u; ++signal)
    {
        float meanLuminance = luminanceSum[signal] / neighbourWeight[signal];
        float spatialVariance = max(luminanceSquaredSum[signal] / neighbourWeight[signal] -
            meanLuminance * meanLuminance, 0.0f);
        float temporalEstimate = temporalVarianceSum[signal] / neighbourWeight[signal];
        float variance = lerp(spatialVariance, temporalEstimate, saturate((centreLength[signal] - 1.0f) / 7.0f));
        bool reliable = neighbourCount[signal] >= 2.0f && isfinite(variance);
        result[signal] = max(variance, 0.0f);
        result[2u + signal] = reliable ? 1.0f : 0.0f;
    }

    return result;
}

// One 3x3 iteration at the given step for the centre pixel. Inputs from the tile (values) and, per pixel, the
// variance of the input value and the centre's reliability. Returns the filtered values and the propagated variance.
void PathTraceAtrousFilter(int2 pixel, int step, bool firstIteration, out float3 filtered[2],
    out float variance[2], out bool reliable[2])
{
    PathTraceAtrousSample centre = PathTraceAtrousTile(pixel);
    float roughness = centre.roughness;
    bool filterSpecular = roughness >= constants.specularRoughnessMinimum;
    float specularNormalPower = max(constants.normalPower, 8.0f / max(roughness * roughness, 1.0e-4f));
    float specularRingScale = saturate((roughness - constants.specularRoughnessMinimum) /
        constants.specularRoughnessMinimum);
    float centreVariance[2];

    if(firstIteration)
    {
        float4 initial = PathTraceAtrousVariance(pixel);
        centreVariance[0] = initial.x;
        centreVariance[1] = initial.y;
        reliable[0] = initial.z > 0.5f;
        reliable[1] = initial.w > 0.5f;
    }
    else
    {
        centreVariance[0] = centre.diffuse.a * centre.diffuse.a;
        centreVariance[1] = centre.specular.a * centre.specular.a;
        reliable[0] = PathTraceAtrousReliable(centre.diffuse.a);
        reliable[1] = PathTraceAtrousReliable(centre.specular.a);
    }

    static const float binomial[3] = {0.25f, 0.5f, 0.25f};
    float centreLuminance[2] = {PathTraceAtrousLuminance(centre.diffuse.rgb),
        PathTraceAtrousLuminance(centre.specular.rgb)};
    bool filter[2] = {reliable[0], reliable[1] && filterSpecular};
    float sigma[2] = {sqrt(centreVariance[0]), sqrt(centreVariance[1])};
    float3 sum[2] = {float3(0.0f, 0.0f, 0.0f), float3(0.0f, 0.0f, 0.0f)};
    float weightSum[2] = {0.0f, 0.0f};
    float varianceSum[2] = {0.0f, 0.0f};

    for(int y = -1; y <= 1; ++y)
    {
        for(int x = -1; x <= 1; ++x)
        {
            bool isCentre = x == 0 && y == 0;
            int2 neighbourPixel = pixel + int2(x, y) * step;
            PathTraceAtrousSample neighbour = PathTraceAtrousTile(neighbourPixel);
            float kernel = binomial[x + 1] * binomial[y + 1];
            float neighbourVariance[2];

            if(firstIteration)
            {
                // Only taps that pass the geometry test read their variance; the inner 10x10 covers every tap.
                float4 initial = PathTraceAtrousVariance(neighbourPixel);
                neighbourVariance[0] = initial.x;
                neighbourVariance[1] = initial.y;
            }
            else
            {
                neighbourVariance[0] = neighbour.diffuse.a * neighbour.diffuse.a;
                neighbourVariance[1] = neighbour.specular.a * neighbour.specular.a;
            }

            for(uint signal = 0u; signal < 2u; ++signal)
            {
                if(!filter[signal] && !isCentre)
                {
                    continue;
                }

                float weight = kernel;

                if(!isCentre)
                {
                    weight *= PathTraceAtrousGeometryWeight(centre, neighbour,
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
                    float difference = abs(centreLuminance[signal] - PathTraceAtrousLuminance(value));
                    weight *= exp(-difference / (constants.luminanceSigma * sigma[signal] +
                        PATH_TRACE_ATROUS_LUMINANCE_EPSILON));
                }

                sum[signal] += weight * value;
                weightSum[signal] += weight;
                varianceSum[signal] += weight * weight * neighbourVariance[signal];
            }
        }
    }

    for(uint signal = 0u; signal < 2u; ++signal)
    {
        filtered[signal] = sum[signal] / weightSum[signal];
        variance[signal] = varianceSum[signal] / (weightSum[signal] * weightSum[signal]);

        if(!all(isfinite(filtered[signal])) || !isfinite(variance[signal]))
        {
            filtered[signal] = signal == 0u ? centre.diffuse.rgb : centre.specular.rgb;
            variance[signal] = centreVariance[signal];
        }
    }
}

// The final output: remodulated presentation, or (diagnostics) views 2 and 3 of the final spatial value.
void PathTraceAtrousPresent(int2 pixel, float3 filtered[2], bool diagnostics)
{
    RWTexture2D<float4> output = ResourceDescriptorHeap[constants.outputIndex];

    if(diagnostics)
    {
        Texture2D<float4> temporalDiffuse = ResourceDescriptorHeap[constants.temporalDiffuseIndex];
        Texture2D<float4> temporalSpecular = ResourceDescriptorHeap[constants.temporalSpecularIndex];
        uint signal = min(constants.visualizationSignal, 1u);
        float3 temporalValue = signal == 0u ? temporalDiffuse.Load(int3(pixel, 0)).rgb :
            temporalSpecular.Load(int3(pixel, 0)).rgb;
        float3 spatialValue = filtered[signal];
        float3 diagnostic = float3(0.0f, 0.0f, 0.0f);

        if(constants.visualizationMode == 2u)
        {
            diagnostic = spatialValue * constants.exposureScale;
        }
        else if(constants.visualizationMode == 3u)
        {
            float difference = abs(PathTraceAtrousLuminance(spatialValue) - PathTraceAtrousLuminance(temporalValue)) *
                constants.exposureScale * constants.differenceScale;
            diagnostic = PathTraceAtrousToSRGB(difference.xxx);
        }

        diagnostic = all(isfinite(diagnostic)) ? diagnostic : float3(0.0f, 0.0f, 0.0f);
        output[pixel] = float4(min(diagnostic, PATH_TRACE_ATROUS_HALF_MAXIMUM), 1.0f);
        return;
    }

    Texture2D<float4> materialTexture = ResourceDescriptorHeap[constants.materialIndex];
    Texture2D<float4> geometricNormalTexture = ResourceDescriptorHeap[constants.geometricNormalIndex];
    float4 material = materialTexture.Load(int3(pixel, 0));
    float metallic = geometricNormalTexture.Load(int3(pixel, 0)).w;
    float3 diffuseAlbedo = max(material.rgb * (1.0f - saturate(metallic)), PATH_TRACE_ATROUS_MINIMUM_ALBEDO);
    float3 radiance = filtered[0] * diffuseAlbedo + filtered[1];
    output[pixel] = float4(min(radiance, PATH_TRACE_ATROUS_HALF_MAXIMUM), 1.0f);
}

void PathTraceAtrousBackground(int2 pixel, bool present, bool diagnostics)
{
    if(diagnostics)
    {
        RWTexture2D<float4> output = ResourceDescriptorHeap[constants.outputIndex];
        output[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    else if(!present)
    {
        // No surface: the intermediate marks it (alpha -infinity) for iteration 2.
        RWTexture2D<float4> diffuseOutput = ResourceDescriptorHeap[constants.intermediateDiffuseIndex];
        RWTexture2D<float4> specularOutput = ResourceDescriptorHeap[constants.intermediateSpecularIndex];
        float noSurface = asfloat(0xFF800000u);
        diffuseOutput[pixel] = float4(0.0f, 0.0f, 0.0f, noSurface);
        specularOutput[pixel] = float4(0.0f, 0.0f, 0.0f, noSurface);
    }
}

// Iteration 1. present: write the presentation (one-iteration mode) instead of the intermediate.
void PathTraceAtrousFirst(uint3 dispatchThreadId, uint3 groupThreadId, uint3 groupId, bool present, bool diagnostics)
{
    int2 pixel = int2(dispatchThreadId.xy);
    uint thread = groupThreadId.y * 8u + groupThreadId.x;
    g_atrousOrigin = int2(groupId.xy) * 8 - 2;

    // Every thread, in or out of the image, loads its share and reaches both barriers before any early return.
    for(uint texel = thread; texel < 144u; texel += 64u)
    {
        PathTraceAtrousStore(texel, PathTraceAtrousLoadTemporal(g_atrousOrigin + int2(texel % 12u, texel / 12u)));
    }

    GroupMemoryBarrierWithGroupSync();

    for(uint inner = thread; inner < 100u; inner += 64u)
    {
        g_atrousVariance[inner] = PathTraceAtrousInitialVariance(g_atrousOrigin + 1 + int2(inner % 10u, inner / 10u));
    }

    GroupMemoryBarrierWithGroupSync();

    if(any(pixel >= int2(constants.resolution)))
    {
        return;
    }

    if(!PathTraceAtrousTile(pixel).valid)
    {
        PathTraceAtrousBackground(pixel, present, diagnostics);
        return;
    }

    float3 filtered[2];
    float variance[2];
    bool reliable[2];
    PathTraceAtrousFilter(pixel, 1, true, filtered, variance, reliable);

    if(present)
    {
        PathTraceAtrousPresent(pixel, filtered, diagnostics);
        return;
    }

    RWTexture2D<float4> diffuseOutput = ResourceDescriptorHeap[constants.intermediateDiffuseIndex];
    RWTexture2D<float4> specularOutput = ResourceDescriptorHeap[constants.intermediateSpecularIndex];
    diffuseOutput[pixel] = float4(min(filtered[0], PATH_TRACE_ATROUS_HALF_MAXIMUM),
        PathTraceAtrousEncodeSigma(min(sqrt(variance[0]), PATH_TRACE_ATROUS_HALF_MAXIMUM), reliable[0]));
    specularOutput[pixel] = float4(min(filtered[1], PATH_TRACE_ATROUS_HALF_MAXIMUM),
        PathTraceAtrousEncodeSigma(min(sqrt(variance[1]), PATH_TRACE_ATROUS_HALF_MAXIMUM), reliable[1]));
}

// Iteration 2 (step 2), the final one: reads the intermediate, writes the presentation (or a diagnostic view).
void PathTraceAtrousSecond(uint3 dispatchThreadId, uint3 groupThreadId, uint3 groupId, bool diagnostics)
{
    int2 pixel = int2(dispatchThreadId.xy);
    uint thread = groupThreadId.y * 8u + groupThreadId.x;
    g_atrousOrigin = int2(groupId.xy) * 8 - 2;

    for(uint texel = thread; texel < 144u; texel += 64u)
    {
        PathTraceAtrousStore(texel, PathTraceAtrousLoadIntermediate(g_atrousOrigin + int2(texel % 12u, texel / 12u)));
    }

    GroupMemoryBarrierWithGroupSync();

    if(any(pixel >= int2(constants.resolution)))
    {
        return;
    }

    if(!PathTraceAtrousTile(pixel).valid)
    {
        PathTraceAtrousBackground(pixel, true, diagnostics);
        return;
    }

    float3 filtered[2];
    float variance[2];
    bool reliable[2];
    PathTraceAtrousFilter(pixel, 2, false, filtered, variance, reliable);
    PathTraceAtrousPresent(pixel, filtered, diagnostics);
}

[numthreads(8, 8, 1)]
void CSFirst(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID)
{
    PathTraceAtrousFirst(dispatchThreadId, groupThreadId, groupId, false, false);
}

[numthreads(8, 8, 1)]
void CSFirstPresent(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID)
{
    PathTraceAtrousFirst(dispatchThreadId, groupThreadId, groupId, true, false);
}

[numthreads(8, 8, 1)]
void CSSecond(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID)
{
    PathTraceAtrousSecond(dispatchThreadId, groupThreadId, groupId, false);
}

[numthreads(8, 8, 1)]
void CSFirstPresentDiagnostic(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID)
{
    PathTraceAtrousFirst(dispatchThreadId, groupThreadId, groupId, true, true);
}

[numthreads(8, 8, 1)]
void CSSecondDiagnostic(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID)
{
    PathTraceAtrousSecond(dispatchThreadId, groupThreadId, groupId, true);
}
