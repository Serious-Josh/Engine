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
    uint2 padding;
};

ConstantBuffer<PathTraceTemporalConstants> constants : register(b0);

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

    if(constants.present != 0u)
    {
        RWTexture2D<float4> presentation = ResourceDescriptorHeap[constants.presentationIndex];
        float3 radiance = diffuseResult.rgb * diffuseAlbedo + specularResult.rgb;
        presentation[pixel] = float4(min(radiance, PATH_TRACE_TEMPORAL_HALF_MAXIMUM), 1.0f);
    }
}
