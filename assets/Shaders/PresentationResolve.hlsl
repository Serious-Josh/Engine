// UPSCALING.2C diagnostic presentation resolve: render-resolution scene to output resolution, recorded only while the
// render extent differs from the output extent. Not an upscaler: it exists so the unchanged tone mapping, selection
// outline and Editor overlays can run at the output extent, and the future DLSS evaluation replaces it.
//   colour  linear HDR, bilinear (clamped at the edges), before tone mapping
//   mask    selection mask, nearest render pixel (a coverage value, never blended)
//   depth   scene depth of the nearest render pixel, carried to this output pixel's centre along that pixel's own surface
//           (written through SV_Depth). NDC depth is linear in screen space on a plane, so a planar surface gets its
//           exact depth and coplanar overlays (the grid on a floor) keep testing equal. Each axis uses the smaller
//           one-sided difference, which never steps across a silhouette onto another surface or the sky.
// Output pixel p maps to render UV (p + 0.5) / output: pixel centres align, so odd extents need no special case.
//
// UPSCALING.3C.3 jitter: under camera jitter the sample of render texel k lies at k + 0.5 + jitter (render pixels, x
// right, y down; Renderer CameraJitterStatus), while the Editor overlays drawn on the result use the unjittered camera.
// Mask and depth therefore take the texel whose sample is nearest the output pixel's unjittered position and
// extrapolate depth from that sample's true position, so overlay depth tests and outlines no longer follow the jitter
// phase. With jitter 0 every expression reduces to the unjittered one.

struct PresentationResolveConstants
{
    uint sceneColorIndex;
    uint depthIndex;
    uint selectionMaskIndex;
    uint renderWidth;
    uint renderHeight;
    uint outputWidth;
    uint outputHeight;
    float jitterX;
    float jitterY;
};

ConstantBuffer<PresentationResolveConstants> resolveConstants : register(b0);

SamplerState linearClamp : register(s0);

struct VSOutput
{
    float4 position : SV_POSITION;
};

VSOutput VSMain(uint vertexID : SV_VertexID)
{
    VSOutput output;

    float2 position;

    if(vertexID == 0)
    {
        position = float2(-1.0f, -1.0f);
    }
    else if(vertexID == 1)
    {
        position = float2(-1.0f, 3.0f);
    }
    else
    {
        position = float2(3.0f, -1.0f);
    }

    output.position = float4(position, 0.0f, 1.0f);

    return output;
}

// Depth change per render pixel along axis at pixel: the smaller of the two one-sided differences whose neighbour is a
// surface (0 when neither is). At the image border the missing side is skipped.
float SurfaceSlope(Texture2D<float> depthTexture, uint2 pixel, int2 axis, uint2 lastPixel, float depth)
{
    float slope = 0.0f;
    float magnitude = 1.0e30f;
    const int2 sides[2] = {axis, -axis};

    for(uint side = 0; side < 2; ++side)
    {
        const int2 neighbour = int2(pixel) + sides[side];

        if(any(neighbour < 0) || any(neighbour > int2(lastPixel)))
        {
            continue;
        }

        const float neighbourDepth = depthTexture.Load(int3(neighbour, 0));

        if(neighbourDepth >= 1.0f)
        {
            continue;
        }

        // Per +axis pixel.
        const float difference = side == 0 ? neighbourDepth - depth : depth - neighbourDepth;

        if(abs(difference) < magnitude)
        {
            magnitude = abs(difference);
            slope = difference;
        }
    }

    return slope;
}

struct PSOutput
{
    float4 color : SV_Target0;
    float selectionMask : SV_Target1;
    float depth : SV_Depth;
};

PSOutput PSMain(VSOutput input)
{
    Texture2D<float4> sceneColor = ResourceDescriptorHeap[resolveConstants.sceneColorIndex];
    Texture2D<float> depthTexture = ResourceDescriptorHeap[resolveConstants.depthIndex];
    Texture2D<float> selectionMask = ResourceDescriptorHeap[resolveConstants.selectionMaskIndex];

    const uint2 outputPixel = uint2(input.position.xy);
    const float2 outputSize = float2(resolveConstants.outputWidth, resolveConstants.outputHeight);
    const float2 renderSize = float2(resolveConstants.renderWidth, resolveConstants.renderHeight);
    const float2 uv = (float2(outputPixel) + 0.5f) / outputSize;

    // The render texel whose (jittered) sample is nearest this output pixel's centre, and that centre relative to the
    // texel's sample grid.
    const float2 samplePosition = uv * renderSize - float2(resolveConstants.jitterX, resolveConstants.jitterY);
    const uint2 lastRenderPixel = uint2(resolveConstants.renderWidth - 1u, resolveConstants.renderHeight - 1u);
    const uint2 renderPixel = min(uint2(max(samplePosition, 0.0f)), lastRenderPixel);

    PSOutput output;
    output.color = sceneColor.SampleLevel(linearClamp, uv, 0.0f);
    output.selectionMask = selectionMask.Load(int3(renderPixel, 0));

    const float depth = depthTexture.Load(int3(renderPixel, 0));
    output.depth = depth;

    // The sky (depth 1) has no surface to follow.
    if(depth < 1.0f)
    {
        // Offset of the output pixel centre from the render texel's sample, in render pixels (|offset| <= 0.5 + rounding).
        const float2 offset = samplePosition - (float2(renderPixel) + 0.5f);
        output.depth = saturate(depth + offset.x * SurfaceSlope(depthTexture, renderPixel, int2(1, 0), lastRenderPixel, depth) +
            offset.y * SurfaceSlope(depthTexture, renderPixel, int2(0, 1), lastRenderPixel, depth));
    }

    return output;
}
