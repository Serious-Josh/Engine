// RTXPT D.2B: temporal reprojection and accumulation of the path tracer's extracted diffuse and specular radiance
// (PathTraceTemporalPass). One thread per pixel:
//
// 1. Reprojection. The G-buffer motion is current UV - previous UV (Triangle.hlsl), so the previous position is
//    previousUV = currentUV - motion.xy, currentUV = (p + 0.5) / resolution, previousPixel = previousUV * resolution
//    - 0.5. The motion z is previous view depth - current view depth, so the expected previous view depth of this
//    surface is currentViewDepth + motion.z (rigid object motion included).
// 2. Validated bilinear history. Each of the four taps around previousPixel must be in bounds, a previous surface
//    with history, within 2% of the expected previous view depth, with a geometric normal close to the current one
//    and the same material index. The weights of the passing taps are renormalized; below a total of 0.05 the
//    history is rejected. Diffuse and specular test normals separately (0.90 and 0.95).
// 3. Diffuse demodulation (RTXPT D.2C.1B). The extracted diffuse is the diffuse lobe's albedo times lighting
//    (PathTraceDiffuseAlbedo: baseColor * (1 - metallic), the G-buffer's material.rgb and geometric normal .w). The
//    diffuse history stores diffuse / albedo, so reprojection resamples lighting, not texture, and the output is
//    remodulated with this pixel's albedo. Albedo channels are clamped below to the same floor on both sides, so
//    remodulation inverts demodulation exactly; a channel darker than the floor keeps (scaled) radiance in history.
// 4. Accumulation. Accepted history of length N gives N' = min(N + 1, cap) and L' = H + (L - H) / N'; rejected
//    history gives N' = 1 and L' = L. The diffuse cap is constant; the specular cap falls with roughness (no reuse at
//    roughness <= 0.1) and with the change of viewing direction since the previous frame.
// 5. Luminance second moments (RTXPT D.2C.3), observational only: they never feed the radiance. Per signal, with Y
//    the luminance of the signal's own history representation (diffuse: demodulated lighting; specular: radiance),
//    M2 is reprojected through exactly the taps and weights that signal's radiance accepted, then accumulated with
//    the same length: M2' = M2 + (Y^2 - M2) / N'. Rejected history gives M2' = Y^2. Stored unrounded in RG32F.
//    max(M2 - Y(history RGB)^2, 0) is the variance of the incoming per-frame samples over the history window (not
//    the uncertainty of the accumulated mean, which is about that variance / N_eff).
//
// 6. SHARC.6A provisional history (experimental; priorIndex != 0). Where the diffuse history is rejected, a history
//    prior P in demodulated units: this frame's direct diffuse light D0 / albedo (written by the path tracer,
//    PathTraceRadianceCacheQuery.hlsli) plus SHARC's light after x0, rho T(v) E / (pi albedo) with E the irradiance of
//    x0's primary-class entry (RadianceCacheD6Irradiance) and T(v) the view transmittance
//    (PathTraceTemporalViewTransmittance, an approximation). The entry is looked up here, only
//    for rejected pixels, through the frame's query binding (priorCache): the key is built from this pixel's world
//    position and its geometric normal turned to the camera. It needs a published entry with at least the binding's
//    queryThreshold samples; its weight is the binding's priorWeight times the level's trust
//    (RadianceCacheD6PriorLevelTrust) times the entry's sample confidence n / (n + 256): an entry's irradiance carries
//    noise that is correlated over its cell, about 1 / n in variance, and at weight 2 the 64-sample entries made the
//    sealed room's disocclusions worse (build/sharc-6a/REPORT.md section 7). The prior is provisional: it changes only the value, L' = (w P + L) / (w + 1),
//    while the stored history length stays 1 and the luminance moment stays the frame's own. So every consumer of the
//    length (this pass's running average, the spatial pass's variance blend and effective count) sees a fresh pixel,
//    and the next frames replace the prior like any first frame: its share after k frames is w / ((w + 1) k). It is
//    never refreshed while history is accepted. (Storing N' = 1 + w measured worse from frame 4 on: the spatial pass
//    took the prior for real samples and filtered less, build/sharc-6a/REPORT.md section 4.) Accepted history, the
//    specular signal and pixels without a prior are unchanged.
//
// 7. DENOISE.3B extended diffuse history (experimental; extendedIndex != 0). A second, in-place running mean of this
//    frame's demodulated diffuse lighting, one RGBA16F texture: RGB the mean, A its length, M' = M + (L - M) / N',
//    N' = min(N + 1, extendedMaximum), stored rounded to nearest. The renderer restarts it (extendedRestart: the frame's
//    value alone) whenever the scene or view may have changed (progressive accumulation reset, invalid history, a
//    scene-lighting epoch change), so it only ever holds frames of one unchanged scene and view, and needs no
//    reprojection. extendedPresent: the presentation uses its mean for diffuse. The history, moments and specular
//    above are unchanged; the renderer decides when the spatial pass reads it instead of the diffuse history.
//
// The G-buffer has no object identity: depth, normal and material tests cannot tell apart two adjacent objects
// that share a material at the same depth and orientation. Pixels without a vertex-0 surface write zero radiance
// and history length zero. Optionally the result is presented: remodulated diffuse + specular replaces the path
// tracer's presentation value on surface pixels; background pixels keep it.

struct PathTraceTemporalConstants
{
    row_major float4x4 inverseCameraRelativeViewProjection;
    row_major float4x4 previousInverseCameraRelativeViewProjection;

    float3 cameraForward;
    float diffuseMaximumHistory;

    float3 previousCameraForward;
    float specularMaximumHistory;

    // The previous camera's position relative to the current camera.
    float3 previousCameraOffset;
    float depthTolerance;

    uint2 resolution;
    uint historyValid;
    uint present;

    uint currentDiffuseIndex;
    uint currentSpecularIndex;
    uint depthIndex;
    uint surfaceIndex;

    uint geometricNormalIndex;
    uint materialIndex;
    uint motionIndex;
    uint previousDepthIndex;

    uint previousSurfaceIndex;
    uint previousGeometricNormalIndex;
    uint historyDiffuseIndex;
    uint historySpecularIndex;

    uint outputDiffuseIndex;
    uint outputSpecularIndex;
    uint presentationIndex;
    float minimumHistoryWeight;

    float diffuseNormalThreshold;
    float specularNormalThreshold;
    float specularRoughnessStart;
    float specularRoughnessRange;

    uint historyMomentsIndex;
    uint outputMomentsIndex;
    // SHARC.6A: the history prior (0 = off) and its counters (0 = not counted).
    uint priorIndex;
    uint priorCountersIndex;

    // DENOISE.3B extended diffuse history (0 = off).
    uint extendedIndex;
    uint extendedRestart;
    float extendedMaximum;
    uint extendedPresent;
};

ConstantBuffer<PathTraceTemporalConstants> constants : register(b0);

#include "RadianceCacheD6.hlsli"

// SHARC.6A: the frame's SHARC query binding (read only where priorIndex != 0).
cbuffer PathTraceTemporalPriorCache : register(b1)
{
    RadianceCacheD6Binding priorCache;
};

static const float PATH_TRACE_TEMPORAL_HALF_MAXIMUM = 65504.0f;
// The G-buffer stores the material index as fp16, exact up to 2048 (PathTracePrimarySurface.hlsli).
static const float PATH_TRACE_TEMPORAL_MAXIMUM_MATERIALS = 2048.0f;
// The diffuse demodulation albedo floor: bounds the division's amplification to 50.
static const float PATH_TRACE_TEMPORAL_MINIMUM_ALBEDO = 0.02f;

bool PathTraceTemporalExactMaterial(float storedIndex)
{
    return isfinite(storedIndex) && storedIndex >= 0.0f && storedIndex == floor(storedIndex) &&
        storedIndex < PATH_TRACE_TEMPORAL_MAXIMUM_MATERIALS;
}

// The pixel centre's position relative to the camera that rendered it.
float3 PathTraceTemporalCameraRelative(uint2 pixel, float depth, float4x4 inverseCameraRelativeViewProjection)
{
    float2 uv = (float2(pixel) + 0.5f) / float2(constants.resolution);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 relative = mul(inverseCameraRelativeViewProjection, float4(ndc, depth, 1.0f));
    return relative.xyz / relative.w;
}

// The nearest RGBA16F-representable value. The history is stored as RGBA16F, and this device's float to half
// conversion truncates (D3D permits it); in a running average of N frames that one-sided rounding accumulates to
// about N half-ulps of downward bias (RTXPT D.2B measured -0.6% at N = 32). An exactly representable value
// converts exactly. Magnitudes beyond the half range clamp to its maximum.
float4 PathTraceTemporalRoundToHalf(float4 value)
{
    float4 magnitude = min(abs(value), PATH_TRACE_TEMPORAL_HALF_MAXIMUM);
    // A half's ulp is 2^(exponent - 10), and 2^-24 for its subnormals; exact powers of two, so the division and the
    // multiplication below are exact.
    float4 exponentScale = asfloat(asuint(magnitude) & 0x7F800000u);
    float4 ulp = max(exponentScale * (1.0f / 1024.0f), 5.9604645e-8f);
    return sign(value) * round(magnitude / ulp) * ulp;
}

// SHARC.6A prior counters (RADIANCE_CACHE_D6_PRIOR_*, RadianceCacheD6.hlsli): rejected diffuse histories, priors used
// and their weights (1/1024 units), and, among rejected histories, the prior's absence reasons: unsupported, absent,
// low confidence, coarse, non-finite.
static const uint PATH_TRACE_TEMPORAL_PRIOR_REJECTED_HISTORY = 7u;
static const uint PATH_TRACE_TEMPORAL_PRIOR_USED = 8u;
static const uint PATH_TRACE_TEMPORAL_PRIOR_WEIGHT_SUM = 9u;
static const uint PATH_TRACE_TEMPORAL_PRIOR_REJECTED_REASON = 10u;

void PathTraceTemporalCountPrior(uint counter, uint amount)
{
    uint total = WaveActiveSum(amount);

    if(WaveIsFirstLane() && total > 0u)
    {
        RWByteAddressBuffer counters = ResourceDescriptorHeap[constants.priorCountersIndex];
        counters.InterlockedAdd(counter * 4u, total);
    }
}

// SHARC.6A: the view transmittance T(v) = 1 - specular albedo(n.v) of a supported (dielectric) surface, by Karis's
// analytic fit of the GGX environment BRDF with F0 = 0.04 (the path tracer's own value is table based, with the
// material's ior and its shading normal; this pass has neither, so the prior inherits the fit's error).
float PathTraceTemporalViewTransmittance(float roughness, float normalDotView)
{
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    float4 r = saturate(roughness) * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28f * saturate(normalDotView))) * r.x + r.y;
    float2 scaleBias = float2(-1.04f, 1.04f) * a004 + r.zw;
    return saturate(1.0f - (0.04f * scaleBias.x + scaleBias.y));
}

// Rec. 709 luminance, the definition used throughout the path tracer (PathTraceLuminance).
float PathTraceTemporalLuminance(float3 value)
{
    return dot(value, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 PathTraceTemporalNormal(float3 normal)
{
    float lengthSquared = dot(normal, normal);
    return lengthSquared > 1.0e-12f ? normal * rsqrt(lengthSquared) : float3(0.0f, 0.0f, 0.0f);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;

    if(any(pixel >= constants.resolution))
    {
        return;
    }

    Texture2D<float4> currentDiffuse = ResourceDescriptorHeap[constants.currentDiffuseIndex];
    Texture2D<float4> currentSpecular = ResourceDescriptorHeap[constants.currentSpecularIndex];
    Texture2D<float> depthTexture = ResourceDescriptorHeap[constants.depthIndex];
    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[constants.surfaceIndex];
    Texture2D<float4> geometricNormalTexture = ResourceDescriptorHeap[constants.geometricNormalIndex];
    Texture2D<float4> materialTexture = ResourceDescriptorHeap[constants.materialIndex];
    Texture2D<float4> motionTexture = ResourceDescriptorHeap[constants.motionIndex];
    RWTexture2D<float4> outputDiffuse = ResourceDescriptorHeap[constants.outputDiffuseIndex];
    RWTexture2D<float4> outputSpecular = ResourceDescriptorHeap[constants.outputSpecularIndex];
    RWTexture2D<float2> outputMoments = ResourceDescriptorHeap[constants.outputMomentsIndex];

    float depth = depthTexture.Load(int3(pixel, 0));
    float materialIndex = surfaceTexture.Load(int3(pixel, 0)).w;

    // No vertex-0 surface: no signal and no history. The presentation keeps the path tracer's value.
    if(!(depth < 1.0f) || !(depth >= 0.0f) || !PathTraceTemporalExactMaterial(materialIndex))
    {
        outputDiffuse[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        outputSpecular[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        outputMoments[pixel] = float2(0.0f, 0.0f);

        if(constants.extendedIndex != 0u)
        {
            RWTexture2D<float4> extended = ResourceDescriptorHeap[constants.extendedIndex];
            extended[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        }

        return;
    }

    float3 diffuse = currentDiffuse.Load(int3(pixel, 0)).rgb;
    float3 specular = currentSpecular.Load(int3(pixel, 0)).rgb;
    diffuse = all(isfinite(diffuse)) ? diffuse : float3(0.0f, 0.0f, 0.0f);
    specular = all(isfinite(specular)) ? specular : float3(0.0f, 0.0f, 0.0f);

    float4 geometricNormal = geometricNormalTexture.Load(int3(pixel, 0));
    float3 normal = PathTraceTemporalNormal(geometricNormal.xyz);
    float4 material = materialTexture.Load(int3(pixel, 0));
    float roughness = material.a;
    float3 diffuseAlbedo = max(material.rgb * (1.0f - saturate(geometricNormal.w)), PATH_TRACE_TEMPORAL_MINIMUM_ALBEDO);
    float3 lighting = diffuse / diffuseAlbedo;
    float4 motion = motionTexture.Load(int3(pixel, 0));
    float3 relative = PathTraceTemporalCameraRelative(pixel, depth, constants.inverseCameraRelativeViewProjection);
    float expectedPreviousDepth = dot(constants.cameraForward, relative) + motion.z;

    // Validated bilinear taps.
    float diffuseWeight = 0.0f;
    float specularWeight = 0.0f;
    float4 diffuseHistory = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 specularHistory = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float diffuseMomentHistory = 0.0f;
    float specularMomentHistory = 0.0f;

    float2 currentUV = (float2(pixel) + 0.5f) / float2(constants.resolution);
    float2 previousUV = currentUV - motion.xy;

    if(constants.historyValid != 0u && all(isfinite(motion)) && expectedPreviousDepth > 0.0f &&
        all(previousUV >= 0.0f) && all(previousUV < 1.0f))
    {
        Texture2D<float> previousDepthTexture = ResourceDescriptorHeap[constants.previousDepthIndex];
        Texture2D<float4> previousSurfaceTexture = ResourceDescriptorHeap[constants.previousSurfaceIndex];
        Texture2D<float4> previousNormalTexture = ResourceDescriptorHeap[constants.previousGeometricNormalIndex];
        Texture2D<float4> historyDiffuse = ResourceDescriptorHeap[constants.historyDiffuseIndex];
        Texture2D<float4> historySpecular = ResourceDescriptorHeap[constants.historySpecularIndex];
        Texture2D<float2> historyMoments = ResourceDescriptorHeap[constants.historyMomentsIndex];

        float2 previousPixel = previousUV * float2(constants.resolution) - 0.5f;
        int2 base = int2(floor(previousPixel));
        float2 blend = previousPixel - float2(base);

        for(uint tap = 0u; tap < 4u; ++tap)
        {
            int2 offset = int2(tap & 1u, tap >> 1u);
            int2 tapPixel = base + offset;
            float weight = (offset.x != 0 ? blend.x : 1.0f - blend.x) * (offset.y != 0 ? blend.y : 1.0f - blend.y);

            if(weight <= 0.0f || any(tapPixel < 0) || any(tapPixel >= int2(constants.resolution)))
            {
                continue;
            }

            float tapDepth = previousDepthTexture.Load(int3(tapPixel, 0));

            if(!(tapDepth < 1.0f) || !(tapDepth >= 0.0f) ||
                previousSurfaceTexture.Load(int3(tapPixel, 0)).w != materialIndex)
            {
                continue;
            }

            float3 tapRelative = PathTraceTemporalCameraRelative(uint2(tapPixel), tapDepth,
                constants.previousInverseCameraRelativeViewProjection);
            float tapViewDepth = dot(constants.previousCameraForward, tapRelative);

            if(!(abs(tapViewDepth - expectedPreviousDepth) <= constants.depthTolerance * expectedPreviousDepth))
            {
                continue;
            }

            float normalAgreement = dot(normal, PathTraceTemporalNormal(previousNormalTexture.Load(int3(tapPixel, 0)).xyz));
            float4 tapDiffuse = historyDiffuse.Load(int3(tapPixel, 0));
            float4 tapSpecular = historySpecular.Load(int3(tapPixel, 0));
            float2 tapMoments = historyMoments.Load(int3(tapPixel, 0));

            // A tap without history (written for a pixel without a surface, or non-finite) contributes nothing.
            if(normalAgreement >= constants.diffuseNormalThreshold && tapDiffuse.a >= 1.0f && all(isfinite(tapDiffuse)))
            {
                diffuseHistory += weight * tapDiffuse;
                diffuseWeight += weight;
                diffuseMomentHistory += weight * tapMoments.x;
            }

            if(normalAgreement >= constants.specularNormalThreshold && tapSpecular.a >= 1.0f &&
                all(isfinite(tapSpecular)))
            {
                specularHistory += weight * tapSpecular;
                specularWeight += weight;
                specularMomentHistory += weight * tapMoments.y;
            }
        }
    }

    // Diffuse (demodulated): history length is the weight-averaged length of the accepted taps.
    float4 diffuseResult = float4(lighting, 1.0f);
    float diffuseLuminance = PathTraceTemporalLuminance(lighting);
    float diffuseMoment = diffuseLuminance * diffuseLuminance;

    if(diffuseWeight >= constants.minimumHistoryWeight)
    {
        float4 history = diffuseHistory / diffuseWeight;
        float length = min(history.a + 1.0f, constants.diffuseMaximumHistory);
        diffuseResult = float4(history.rgb + (lighting - history.rgb) / length, length);
        float momentHistory = diffuseMomentHistory / diffuseWeight;
        diffuseMoment = momentHistory + (diffuseMoment - momentHistory) / length;
    }
    else if(constants.priorIndex != 0u)
    {
        Texture2D<float4> priorTexture = ResourceDescriptorHeap[constants.priorIndex];
        float4 prior = priorTexture.Load(int3(pixel, 0));
        // Absence reason 0 unsupported, 1 absent, 2 low confidence, 3 coarse, 4 non-finite; 5 none (used).
        uint reason = prior.a > 0.0f ? 1u : 0u;
        float weight = 0.0f;
        float3 priorLighting = float3(0.0f, 0.0f, 0.0f);

        if(reason == 1u)
        {
            // The training's x0 is the path tracer's primary vertex: lifted off the surface along the facing geometric
            // normal by PATH_TRACE_PRIMARY_SURFACE_LIFT times its distance (PathTracePrimarySurface.hlsli). The same
            // lift keeps surfaces on grid planes (floors at y = 0) on the training's side of a cell boundary.
            float3 facing = dot(normal, relative) > 0.0f ? -normal : normal;
            float3 lifted = relative + facing * (2.0e-4f * length(relative));
            uint2 key;
            uint level;
            bool keyed = RadianceCacheD6MakeClassKey(priorCache, priorCache.anchor + lifted, facing, true, key, level);
            uint slot = keyed && RadianceCacheD6BindingAccepted(priorCache) ? RadianceCacheD6FindSlot(priorCache, key) :
                RADIANCE_CACHE_D6_NO_SLOT;

            if(slot != RADIANCE_CACHE_D6_NO_SLOT)
            {
                RWByteAddressBuffer entries = ResourceDescriptorHeap[priorCache.publishedIndex];
                uint base = slot * RADIANCE_CACHE_D6_PUBLISHED_WORDS * 4u;
                float sampleCount = asfloat(entries.Load(base + 18u * 4u));
                bool published = (entries.Load(base + 22u * 4u) & RADIANCE_CACHE_D6_FLAG_PUBLISHED) != 0u;
                float trust = RadianceCacheD6PriorLevelTrust(level);
                if(!published)
                {
                    reason = 1u;
                }
                else if(sampleCount < priorCache.queryThreshold)
                {
                    reason = 2u;
                }
                else if(trust <= 0.0f)
                {
                    reason = 3u;
                }
                else
                {
                    reason = 5u;
                }

                if(reason == 5u)
                {
                    float3 irradiance = RadianceCacheD6Irradiance(asfloat(entries.Load3(base)),
                        asfloat(entries.Load3(base + 12u)));
                    float3 rho = material.rgb * (1.0f - saturate(geometricNormal.w));
                    float transmittance = PathTraceTemporalViewTransmittance(roughness,
                        dot(facing, PathTraceTemporalNormal(-relative)));
                    priorLighting = (prior.rgb + rho * transmittance * irradiance * (1.0f / 3.14159265f)) /
                        diffuseAlbedo;
                    weight = priorCache.priorWeight * trust * sampleCount / (sampleCount + 256.0f);
                    reason = all(isfinite(priorLighting)) ? 5u : 4u;
                }
            }
        }

        bool used = reason == 5u && weight > 0.0f;

        if(used)
        {
            diffuseResult = float4((weight * priorLighting + lighting) / (weight + 1.0f), 1.0f);
        }

        if(constants.priorCountersIndex != 0u)
        {
            PathTraceTemporalCountPrior(PATH_TRACE_TEMPORAL_PRIOR_REJECTED_HISTORY, 1u);
            PathTraceTemporalCountPrior(PATH_TRACE_TEMPORAL_PRIOR_USED, used ? 1u : 0u);
            PathTraceTemporalCountPrior(PATH_TRACE_TEMPORAL_PRIOR_WEIGHT_SUM, used ? uint(weight * 1024.0f + 0.5f) : 0u);

            [unroll]
            for(uint counted = 0u; counted < 5u; ++counted)
            {
                PathTraceTemporalCountPrior(PATH_TRACE_TEMPORAL_PRIOR_REJECTED_REASON + counted, reason == counted ? 1u : 0u);
            }
        }
    }

    // Specular: the cap falls with roughness and with the angle between this and the previous viewing direction,
    // N = cap(roughness) * exp(-angle / max(roughness^2, 0.02)), never below one.
    float roughnessCap = 1.0f + (constants.specularMaximumHistory - 1.0f) *
        saturate((roughness - constants.specularRoughnessStart) / constants.specularRoughnessRange);
    float3 viewCurrent = PathTraceTemporalNormal(-relative);
    float3 viewPrevious = PathTraceTemporalNormal(constants.previousCameraOffset - relative);
    float viewAngle = acos(clamp(dot(viewCurrent, viewPrevious), -1.0f, 1.0f));
    float specularCap = max(roughnessCap * exp(-viewAngle / max(roughness * roughness, 0.02f)), 1.0f);
    float4 specularResult = float4(specular, 1.0f);
    float specularLuminance = PathTraceTemporalLuminance(specular);
    float specularMoment = specularLuminance * specularLuminance;

    if(specularWeight >= constants.minimumHistoryWeight && specularCap > 1.0f)
    {
        float4 history = specularHistory / specularWeight;
        float length = min(history.a + 1.0f, specularCap);
        specularResult = float4(history.rgb + (specular - history.rgb) / length, length);
        float momentHistory = specularMomentHistory / specularWeight;
        specularMoment = momentHistory + (specularMoment - momentHistory) / length;
    }

    diffuseResult = PathTraceTemporalRoundToHalf(diffuseResult);
    specularResult = PathTraceTemporalRoundToHalf(specularResult);
    outputDiffuse[pixel] = diffuseResult;
    outputSpecular[pixel] = specularResult;
    outputMoments[pixel] = float2(diffuseMoment, specularMoment);

    float3 presentedLighting = diffuseResult.rgb;

    if(constants.extendedIndex != 0u)
    {
        RWTexture2D<float4> extended = ResourceDescriptorHeap[constants.extendedIndex];
        float4 previous = constants.extendedRestart != 0u ? float4(0.0f, 0.0f, 0.0f, 0.0f) : extended[pixel];
        previous = all(isfinite(previous)) && previous.a >= 0.0f ? previous : float4(0.0f, 0.0f, 0.0f, 0.0f);
        float length = min(previous.a + 1.0f, constants.extendedMaximum);
        float4 value = PathTraceTemporalRoundToHalf(float4(previous.rgb + (lighting - previous.rgb) / length, length));
        extended[pixel] = value;
        presentedLighting = constants.extendedPresent != 0u ? value.rgb : presentedLighting;
    }

    if(constants.present != 0u)
    {
        RWTexture2D<float4> presentation = ResourceDescriptorHeap[constants.presentationIndex];
        float3 radiance = presentedLighting * diffuseAlbedo + specularResult.rgb;
        presentation[pixel] = float4(min(radiance, PATH_TRACE_TEMPORAL_HALF_MAXIMUM), 1.0f);
    }
}
