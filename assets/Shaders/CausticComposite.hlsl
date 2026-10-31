// Transactional display composite. Validate the complete image, barrier, invalidate
// the epoch if needed, barrier, then publish. The immutable Reference source is copied
// to output before either dispatch; failure leaves that copy byte-for-byte unchanged.
cbuffer CausticCompositeConstants : register(b0)
{
    uint2 resolution;
    uint outputIndex;
    uint historyIndex;
    uint controlIndex;
    uint causticOnly;
    uint2 padding;
};

float3 CompositeValue(float3 reference, float3 caustic)
{
    return causticOnly != 0u ? caustic : reference + caustic;
}

[numthreads(8, 8, 1)]
void CSValidateComposite(uint3 id : SV_DispatchThreadID)
{
    if(any(id.xy >= resolution))
    {
        return;
    }

    RWByteAddressBuffer control = ResourceDescriptorHeap[controlIndex];

    if(control.Load(0u) != 0u || control.Load(4u) == 0u)
    {
        return;
    }

    RWByteAddressBuffer history = ResourceDescriptorHeap[historyIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[outputIndex];
    const float4 caustic = asfloat(history.Load4((id.y * resolution.x + id.x) * 16u));
    const float4 reference = output[id.xy];
    precise float3 result = CompositeValue(reference.rgb, caustic.rgb);

    // RGB is additive; alpha belongs to Reference and is preserved. A nonfinite
    // Reference value refuses the dedicated image without changing Reference HDR.
    if(!all(isfinite(caustic)) || any(caustic.rgb < 0.0f) || caustic.a != 1.0f ||
        !all(isfinite(reference)) || !all(isfinite(result)) || any(abs(result) > 65504.0f))
    {
        control.InterlockedOr(0u, 32u);
    }
}

[numthreads(8, 8, 1)]
void CSComposite(uint3 id : SV_DispatchThreadID)
{
    if(any(id.xy >= resolution))
    {
        return;
    }

    RWByteAddressBuffer control = ResourceDescriptorHeap[controlIndex];

    if(control.Load(0u) != 0u || control.Load(4u) == 0u)
    {
        return;
    }

    RWByteAddressBuffer history = ResourceDescriptorHeap[historyIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[outputIndex];
    const float3 caustic = asfloat(history.Load3((id.y * resolution.x + id.x) * 16u));
    const float4 reference = output[id.xy];
    output[id.xy] = float4(CompositeValue(reference.rgb, caustic), reference.a);
}
