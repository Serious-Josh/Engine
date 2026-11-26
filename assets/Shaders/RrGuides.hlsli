#ifndef ENGINE_RR_GUIDES
#define ENGINE_RR_GUIDES

// RR.1B: the reflectance guides of DLSS Ray Reconstruction (DLSS-RR Integration Guide 3.4.1, 3.4.2 and its appendix).
// Linear values, written to non-sRGB targets (the guide rejects sRGB albedo formats).

#include "MaterialLighting.hlsli"

// The appendix's specular albedo: the average specular reflectivity for a view direction (Ray Tracing Gems ch. 32), with
// alpha the GGX alpha (perceptual roughness squared) and NoV the cosine between the shading normal and the view. Constants
// and arithmetic as printed in the guide's appendix.
float3 RrEnvBRDFApprox2(float3 specularColor, float alpha, float NoV)
{
    NoV = abs(NoV);
    float4 X;
    X.x = 1.0f;
    X.y = NoV;
    X.z = NoV * NoV;
    X.w = NoV * X.z;
    float4 Y;
    Y.x = 1.0f;
    Y.y = alpha;
    Y.z = alpha * alpha;
    Y.w = alpha * Y.z;
    float2x2 M1 = float2x2(0.99044f, -1.28514f, 1.29678f, -0.755907f);
    float3x3 M2 = float3x3(1.0f, 2.92338f, 59.4188f, 20.3225f, -27.0302f, 222.592f, 121.563f, 626.13f, 316.627f);
    float2x2 M3 = float2x2(0.0365463f, 3.32707f, 9.0632f, -9.04756f);
    float3x3 M4 = float3x3(1.0f, 3.59685f, -1.36772f, 9.04401f, -16.3174f, 9.22949f, 5.56589f, 19.7886f, -20.2123f);
    float bias = dot(mul(M1, X.xy), Y.xy) * rcp(dot(mul(M2, X.xyw), Y.xyw));
    float scale = dot(mul(M3, X.xy), Y.xy) * rcp(dot(mul(M4, X.xzw), Y.xyw));
    // The guide's own correction for a specular reflectance of 0.
    bias *= saturate(specularColor.g * 50.0f);
    return mad(specularColor, max(0.0f, scale), max(0.0f, bias));
}

// The diffuse reflectance and specular F0 from the G-buffer's linear base colour and metalness: the OpenPBR material's
// guide reflectances (OpenPBRMaterialReflectanceGuide, the same ones NRD's NrdPathTraceMaterialAlbedo uses), without
// any of NRD's demodulation floors.
void RrMaterialReflectance(GPUMaterial material, float3 baseColor, float metallic, out float3 diffuseAlbedo,
    out float3 specularF0)
{
    OpenPBRMaterialReflectanceGuide(material, baseColor, metallic, diffuseAlbedo, specularF0);
}

#endif
