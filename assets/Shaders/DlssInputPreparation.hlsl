// UPSCALING.3B DLSS input preparation (diagnostic until UPSCALING.3C evaluates DLSS). Reads Engine's G-buffer and
// writes DLSS-specific inputs; Engine's own motion vectors are never modified.
//
// Motion vectors (RG16F, render extent). DLSS contract (Programming Guide 3.6, 8.1): the vector added to the pixel's
// current position gives its position in the previous frame, in render-resolution pixels, origin at the top-left, y
// down, jitter excluded (NVSDK_NGX_DLSS_Feature_Flags_MVJittered = 0, MVLowRes = 1, InMVScale = 1).
//   surface  Engine stores currentUV - previousUV of the rasterized sample (unjittered, UV y down; Triangle.hlsl), so
//            the DLSS vector is -motion.xy * renderSize. Invalid pixels (Engine writes 0) stay 0.
//   sky      (depth 1) has no motion in the G-buffer. Its direction is reconstructed through the render (jittered)
//            camera-relative inverse view-projection at the pixel centre, then projected as a direction (w = 0) with
//            the current and previous unjittered view-projections: rotation only, no translation parallax. A direction
//            behind the previous camera (previous clip w <= 0) gets 0.
// Exposure (R16F, 1x1): the fixed exposure the tone mapper applies (ToneMapPass: 1.0); DLSS pInExposureTexture.

struct DlssInputPreparationConstants
{
    row_major float4x4 inverseCameraRelativeRenderViewProjection;
    row_major float4x4 viewProjection;
    row_major float4x4 previousViewProjection;
    uint motionIndex;
    uint depthIndex;
    uint outputMotionIndex;
    uint exposureIndex;
    uint width;
    uint height;
    float exposure;
    uint padding;
};

ConstantBuffer<DlssInputPreparationConstants> constants : register(b0);

float2 ClipToUV(float4 clip)
{
    const float2 ndc = clip.xy / clip.w;
    return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThread : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThread.xy;

    if(all(pixel == 0))
    {
        RWTexture2D<float> exposureTexture = ResourceDescriptorHeap[constants.exposureIndex];
        exposureTexture[uint2(0, 0)] = constants.exposure;
    }

    if(pixel.x >= constants.width || pixel.y >= constants.height)
    {
        return;
    }

    Texture2D<float4> motionTexture = ResourceDescriptorHeap[constants.motionIndex];
    Texture2D<float> depthTexture = ResourceDescriptorHeap[constants.depthIndex];
    RWTexture2D<float2> outputMotion = ResourceDescriptorHeap[constants.outputMotionIndex];

    const float2 size = float2(constants.width, constants.height);
    const float depth = depthTexture.Load(int3(pixel, 0));
    float2 motion = float2(0.0f, 0.0f);

    if(depth < 1.0f)
    {
        motion = -motionTexture.Load(int3(pixel, 0)).xy * size;
    }
    else
    {
        // The sample direction of this pixel: any point on its ray, relative to the camera (far plane, w divided out).
        const float2 uv = (float2(pixel) + 0.5f) / size;
        const float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 1.0f, 1.0f);
        const float4 farPoint = mul(constants.inverseCameraRelativeRenderViewProjection, ndc);
        const float3 direction = farPoint.xyz / farPoint.w;

        const float4 currentClip = mul(constants.viewProjection, float4(direction, 0.0f));
        const float4 previousClip = mul(constants.previousViewProjection, float4(direction, 0.0f));

        if(currentClip.w > 0.0f && previousClip.w > 0.0f)
        {
            motion = (ClipToUV(previousClip) - ClipToUV(currentClip)) * size;
        }
    }

    outputMotion[pixel] = all(isfinite(motion)) ? motion : float2(0.0f, 0.0f);
}
