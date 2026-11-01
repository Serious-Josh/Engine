#ifndef ENGINE_PSR_ELIGIBILITY
#define ENGINE_PSR_ELIGIBILITY

// PSR.2 raster-time eligibility (authoritative for PSR.2): PSR.1's PathTracePsrEligible evaluated in the geometry pass
// on the values the G-buffer will store. The path tracer's decode (PathTraceReconstructPrimaryVertex) runs unchanged
// on an emulation of what PathTraceLoadGBuffer reads back: 8-bit UNORM roughness, 8-bit sRGB base colour, fp16
// normals, metallic and material index, and the D32 depth of this fragment. maxPathDepth > 0 is checked on the CPU.

#include "PathTracePrimarySurface.hlsli"
#include "PsrIdentity.hlsli"

float PsrStoredUnorm8(float value)
{
    return round(saturate(value) * 255.0f) / 255.0f;
}

// Linear to the stored sRGB byte and back (the sampled value of an RGBA8_UNORM_SRGB texel).
float PsrStoredSrgb8(float linearValue)
{
    float value = saturate(linearValue);
    float encoded = value <= 0.0031308f ? value * 12.92f : 1.055f * pow(value, 1.0f / 2.4f) - 0.055f;
    float stored = round(saturate(encoded) * 255.0f) / 255.0f;
    return stored <= 0.04045f ? stored / 12.92f : pow((stored + 0.055f) / 1.055f, 2.4f);
}

float4 PsrStoredHalf(float4 value)
{
    return f16tof32(f32tof16(value));
}

// The G-buffer outputs of one fragment at pixel, with its depth (SV_Position.z).
bool PsrRasterEligible(uint2 pixel, float depth, float4 material, float4 surface, float4 geometricNormal,
    PathTracePrimaryCamera camera, StructuredBuffer<GPUMaterial> materials)
{
    PathTraceGBufferSample gBuffer;
    gBuffer.depth = depth;
    gBuffer.material.a = PsrStoredUnorm8(material.a);

    // deltaSpecular is decoded roughness <= PATH_TRACE_DELTA_ROUGHNESS, which only a zero byte satisfies; every other
    // fragment is ineligible, and the delta-only decode below never reads the material tables.
    if(!(gBuffer.depth < 1.0f) || gBuffer.material.a > PATH_TRACE_DELTA_ROUGHNESS)
    {
        return false;
    }

    gBuffer.material.rgb = float3(PsrStoredSrgb8(material.r), PsrStoredSrgb8(material.g), PsrStoredSrgb8(material.b));
    gBuffer.surface = PsrStoredHalf(surface);
    gBuffer.geometricNormal = PsrStoredHalf(geometricNormal);

    PathTracePrimaryVertex vertex;

    if(!PathTraceReconstructPrimaryVertex(pixel, gBuffer, camera, materials, 0u, 0u, 0u, vertex))
    {
        return false;
    }

    return vertex.surface.deltaSpecular && vertex.surface.specularProbability >= 1.0f;
}

#endif
