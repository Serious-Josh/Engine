// Experimental NRD-input limiter. Raw estimator and alpha remain untouched.
cbuffer Constants : register(b0)
{
    uint inputIndex;
    uint outputIndex;
    uint width;
    uint height;
};
static const float3 LUMA = float3(0.2126f, 0.7152f, 0.0722f);
static const float TAIL_MULTIPLIER = 32.0f;
static const float EPSILON = 1.0e-6f; // Numerical guard, not an HDR ceiling.

[numthreads(8, 8, 1)]
void CSMain(uint3 thread : SV_DispatchThreadID)
{
    uint2 pixel = thread.xy;
    if(pixel.x >= width || pixel.y >= height) return;
    Texture2D<float4> raw = ResourceDescriptorHeap[inputIndex];
    RWTexture2D<float4> limited = ResourceDescriptorHeap[outputIndex];
    float4 center = raw.Load(int3(pixel, 0));
    float luminance = dot(center.rgb, LUMA);
    float samples[24];
    uint count = 0;
    // Use only in-bounds, finite neighbors, excluding the center contribution.
    for(int y = -2; y <= 2; ++y)
    for(int x = -2; x <= 2; ++x)
    {
        int2 q = int2(pixel) + int2(x, y);
        if((x == 0 && y == 0) || any(q < 0) || q.x >= int(width) || q.y >= int(height)) continue;
        float value = dot(raw.Load(int3(q, 0)).rgb, LUMA);
        if(!isfinite(value)) continue;
        value = max(value, 0.0f);
        // Insertion sort: at most 24 scalar values, no stochastic samples/rays.
        int i = int(count);
        while(i > 0 && samples[i - 1] > value)
        {
            samples[i] = samples[i - 1];
            --i;
        }
        samples[i] = value;
        ++count;
    }
    if(count >= 8 && all(isfinite(center.rgb)) && isfinite(luminance) && luminance > EPSILON)
    {
        // Nearest-rank 75th percentile. Full neighborhood: sorted index 17.
        float reference = samples[(3 * count + 3) / 4 - 1];
        // With no meaningful local reference, pass through rather than imposing a black-region ceiling.
        if(reference > EPSILON)
        {
            float bound = TAIL_MULTIPLIER * reference;
            if(luminance > bound) center.rgb *= bound / max(luminance, EPSILON);
        }
    }
    limited[pixel] = center;
}
