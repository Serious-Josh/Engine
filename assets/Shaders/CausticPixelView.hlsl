// R.9I.3 pixel-centred caustics: caustics-only debug views (PixelCausticPass::RecordView). Reads the estimator's
// two lobe textures and the frame's fault latch; writes only the pass's own presentation texture, which the
// Renderer copies to scene colour in place of the ordinary path-traced presentation copy. No lighting signal,
// extraction texture or denoiser input is read or written.
cbuffer ViewConstants : register(b0)
{
    uint diffuseIndex;
    uint specularIndex;
    uint faultLatchIndex;
    uint outputIndex;
    uint2 resolution;
    uint view;  // 1 diffuse, 2 specular, 3 combined, 4 validity
    uint padding;
};

static const uint ViewDiffuse = 1;
static const uint ViewSpecular = 2;
static const uint ViewCombined = 3;
static const uint ViewValidity = 4;

float3 StateColour(uint state)
{
    switch(state)
    {
    case 0:
        return float3(0.0f, 0.0f, 0.0f);       // Background.
    case 1:
        return float3(0.05f, 0.05f, 0.05f);    // Decoded, not the certified receiver.
    case 3:
        return float3(0.0f, 0.05f, 0.4f);      // Certified, unscheduled.
    case 4:
        return float3(0.0f, 0.15f, 0.0f);      // Accepted zero.
    case 5:
        return float3(0.1f, 1.0f, 0.1f);       // Accepted nonzero.
    default:
        return float3(1.0f, 0.0f, 0.0f);       // Fault (6) or unknown.
    }
}

[numthreads(8, 8, 1)] void CSView(uint3 id : SV_DispatchThreadID)
{
    if(any(id.xy >= resolution))
    {
        return;
    }

    Texture2D<float4> diffuse = ResourceDescriptorHeap[diffuseIndex];
    Texture2D<float4> specular = ResourceDescriptorHeap[specularIndex];
    ByteAddressBuffer latch = ResourceDescriptorHeap[faultLatchIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[outputIndex];
    float4 d = diffuse.Load(int3(id.xy, 0));
    float4 s = specular.Load(int3(id.xy, 0));
    bool frameFaulted = latch.Load(0) != 0u;
    float3 colour;

    if(view == ViewValidity)
    {
        colour = StateColour((uint)d.w);
    }
    else if(frameFaulted)
    {
        // A faulted frame is refused as a whole (P2 inserts nothing); its radiance is never shown as valid.
        colour = ((id.x / 8u + id.y / 8u) & 1u) != 0u ? float3(1.0f, 0.0f, 1.0f) : float3(0.0f, 0.0f, 0.0f);
    }
    else if(view == ViewDiffuse)
    {
        colour = d.rgb;
    }
    else if(view == ViewSpecular)
    {
        colour = s.rgb;
    }
    else
    {
        colour = d.rgb + s.rgb;
    }

    output[id.xy] = float4(min(colour, 65504.0f), 1.0f);
}
