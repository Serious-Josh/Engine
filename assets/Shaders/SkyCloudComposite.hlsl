// Physical sky S5 (Physical_Sky_Design_Proposal.md §14.3): the camera clouds over the frame. A full-screen triangle into the HDR scene
// colour at output resolution, after the aerial perspective and before haze, mist and tone mapping, in every lighting mode. Each pixel
// reads the quarter-resolution cloud image (SkyAtmosphere.hlsl's CSCloudReproject: in-scattered light through the air in front, and
// the clouds' transmittance of the background) and its cloud depth. Geometry nearer than the clouds is untouched; the sky and farther
// geometry become scene * T + light. The camera background itself is the cloud-free sky-view map, so the clouds are applied once.
// Sun behind clouds (2026-11-25): the blend multiplies the scene by T itself (ONE, SRC_ALPHA: Pipeline's transmittanceBlend), not by
// 1 - (1 - T), so a small T (a disk behind a thick cloud: 60,000 x T against a cloud near 1) keeps its value instead of rounding
// to 1 - alpha in the blender, and thin clouds need no light / (1 - T) division.

#include "Common.hlsli"

cbuffer CloudCompositeConstants : register(b0)
{
    row_major float4x4 inverseViewProjection;

    float3 cameraPosition;
    uint depthIndex;

    uint cloudIndex;
    uint cloudDepthIndex;
    uint2 compositePadding;
}

SamplerState linearClamp : register(s0);

struct CompositeVertex
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

CompositeVertex VSComposite(uint vertex : SV_VertexID)
{
    CompositeVertex output;
    output.uv = float2((vertex << 1) & 2, vertex & 2);
    output.position = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float4 PSComposite(CompositeVertex input) : SV_Target0
{
    Texture2D<float> depthTexture = ResourceDescriptorHeap[depthIndex];
    Texture2D<float4> clouds = ResourceDescriptorHeap[cloudIndex];
    Texture2D<float> cloudDepth = ResourceDescriptorHeap[cloudDepthIndex];
    const float depth = depthTexture.Load(int3(int2(input.position.xy), 0));
    const float4 value = clouds.SampleLevel(linearClamp, input.uv, 0.0f);

    if(depth < 1.0f)
    {
        // Geometry: only clouds in front of it (km).
        const float4 world = mul(inverseViewProjection, float4(input.uv.x * 2.0f - 1.0f, 1.0f - input.uv.y * 2.0f, depth, 1.0f));
        const float distance = length(world.xyz / world.w - cameraPosition) / 1000.0f;

        if(distance < cloudDepth.SampleLevel(linearClamp, input.uv, 0.0f))
        {
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
        }
    }

    return float4(value.rgb, saturate(value.a));
}
