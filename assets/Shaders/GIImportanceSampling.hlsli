#ifndef ENGINE_GI_IMPORTANCE_SAMPLING
#define ENGINE_GI_IMPORTANCE_SAMPLING

// Cosine/environment mixture with exact solid-angle sampling inside each CDF cell.
// Folding both environment hemispheres gives full support and weight cos(theta)/q <= 2*PI.
uint GISampleCDF(StructuredBuffer<float> cdf, uint offset, uint count, float randomValue)
{
    uint low = 0u;
    uint high = count - 1u;

    while(low < high)
    {
        uint middle = low + (high - low) / 2u;

        if(randomValue < cdf[offset + middle])
        {
            high = middle;
        }
        else
        {
            low = middle + 1u;
        }
    }

    return low;
}

float GIEnvironmentPDF(float3 direction)
{
    float2 uv = DirectionToEquirectangularUV(direction);
    uint x = min(uint(frac(uv.x) * importanceWidth), importanceWidth - 1u);
    uint y = min(uint(saturate(uv.y) * importanceHeight), importanceHeight - 1u);
    StructuredBuffer<float> rows = ResourceDescriptorHeap[importanceMarginalCDFIndex];
    StructuredBuffer<float> columns = ResourceDescriptorHeap[importanceConditionalCDFIndex];
    uint rowOffset = y * importanceWidth;
    float rowMass = max(rows[y] - (y > 0u ? rows[y - 1u] : 0.0f), 0.0f);
    float columnMass = max(columns[rowOffset + x] - (x > 0u ? columns[rowOffset + x - 1u] : 0.0f), 0.0f);
    float solidAngle = (2.0f * PI / float(importanceWidth)) *
        (cos(PI * float(y) / float(importanceHeight)) - cos(PI * float(y + 1u) / float(importanceHeight)));
    return rowMass * columnMass / max(solidAngle, 1.0e-20f);
}

float GIRandomDimension(uint2 pixel, uint sample, uint dimension)
{
    uint seed = GIHash(pixel.x ^ GIHash(pixel.y) ^ GIHash(sample) ^ GIHash(dimension));
    return float(seed >> 8u) * (1.0f / 16777216.0f);
}

float3 SampleGIImportanceHemisphere(float3 normal, uint2 pixel, uint sample, out float irradianceWeight)
{
    float3 direction = SampleGICosineHemisphere(normal, pixel, sample);
    irradianceWeight = PI;

    if(importanceWidth == 0u || importanceHeight == 0u || !isfinite(importanceTotalWeight) ||
        importanceTotalWeight <= 0.0f || importanceConditionalCDFIndex == 0xFFFFFFFFu ||
        importanceMarginalCDFIndex == 0xFFFFFFFFu)
    {
        return direction;
    }

    if(GIRandomDimension(pixel, sample, 101u) < 0.5f)
    {
        StructuredBuffer<float> rows = ResourceDescriptorHeap[importanceMarginalCDFIndex];
        StructuredBuffer<float> columns = ResourceDescriptorHeap[importanceConditionalCDFIndex];
        uint y = GISampleCDF(rows, 0u, importanceHeight, GIRandomDimension(pixel, sample, 102u));
        uint x = GISampleCDF(columns, y * importanceWidth, importanceWidth, GIRandomDimension(pixel, sample, 103u));
        float u = (float(x) + GIRandomDimension(pixel, sample, 104u)) / float(importanceWidth);
        float cosTheta = lerp(cos(PI * float(y) / float(importanceHeight)),
            cos(PI * float(y + 1u) / float(importanceHeight)), GIRandomDimension(pixel, sample, 105u));
        float v = acos(clamp(cosTheta, -1.0f, 1.0f)) / PI;
        direction = EquirectangularUVToDirection(float2(u, v));

        // Fold the lower hemisphere instead of discarding zero samples before denoising.
        if(dot(normal, direction) < 0.0f)
        {
            direction = reflect(direction, normal);
        }
    }

    float cosine = max(dot(normal, direction), 0.0f);
    float3 opposite = reflect(direction, normal);
    float pdf = 0.5f * cosine / PI + 0.5f * (GIEnvironmentPDF(direction) + GIEnvironmentPDF(opposite));
    irradianceWeight = pdf > 0.0f ? cosine / pdf : 0.0f;
    return direction;
}

#endif
