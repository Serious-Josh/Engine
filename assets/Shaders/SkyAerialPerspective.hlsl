// Physical sky S3 (§4.7): the aerial-perspective composite. A full-screen triangle into the HDR scene colour at output resolution,
// after upscaling and before haze, mist and tone mapping, in every mode (packaged builds included). Each geometry pixel reads the
// volume SkyAtmosphere::RecordAerialPerspective built this frame at its distance from the camera and blends straight alpha
// (light / (1 - T), 1 - T), which gives scene * T + light. Sky pixels (depth 1) already carry the atmosphere and are untouched.

#include "Common.hlsli"

cbuffer AerialPerspectiveConstants : register(b0)
{
    row_major float4x4 inverseViewProjection;

    float3 cameraPosition;
    uint depthIndex;

    uint volumeIndex;
    uint slices;
    uint tiles;
    // Kilometres; scene units are metres.
    float maxDistance;
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

// One slice of the atlas at a tile-space uv (bilinear inside the slice only).
float4 SampleSlice(Texture2D<float4> volume, float2 uv, uint slice)
{
    const float2 inside = clamp(uv * float(tiles), 0.5f, float(tiles) - 0.5f);
    const float2 atlas = float2(float(slice * tiles) + inside.x, inside.y) / float2(float(slices * tiles), float(tiles));
    return volume.SampleLevel(linearClamp, atlas, 0.0f);
}

float4 PSComposite(CompositeVertex input) : SV_Target0
{
    Texture2D<float> depthTexture = ResourceDescriptorHeap[depthIndex];
    const float depth = depthTexture.Load(int3(int2(input.position.xy), 0));

    if(!(depth < 1.0f))
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    const float4 world = mul(inverseViewProjection, float4(input.uv.x * 2.0f - 1.0f, 1.0f - input.uv.y * 2.0f, depth, 1.0f));
    const float distance = length(world.xyz / world.w - cameraPosition) / 1000.0f;

    // Slice k ends at maxDistance ((k + 1) / slices)^2; before the first slice, blend from nothing.
    Texture2D<float4> volume = ResourceDescriptorHeap[volumeIndex];
    const float position = sqrt(saturate(distance / maxDistance)) * float(slices) - 1.0f;
    float4 value;

    if(position < 0.0f)
    {
        value = lerp(float4(0.0f, 0.0f, 0.0f, 1.0f), SampleSlice(volume, input.uv, 0u), saturate(position + 1.0f));
    }
    else
    {
        const uint lower = min(uint(position), slices - 1u);
        const uint upper = min(lower + 1u, slices - 1u);
        value = lerp(SampleSlice(volume, input.uv, lower), SampleSlice(volume, input.uv, upper), frac(position));
    }

    const float alpha = saturate(1.0f - value.a);
    return alpha > 1.0e-4f ? float4(value.rgb / alpha, alpha) : float4(0.0f, 0.0f, 0.0f, 0.0f);
}
