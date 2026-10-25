// NRD.3 path-traced NRD signal preparation and bypass reconstruction (diagnostics only; no NRD denoiser runs).
//
// CSPrepare turns this frame's RTXPT D.1B.2 extraction outputs (raw, material-modulated diffuse and specular radiance,
// combined direct + indirect, with primary hit distances in alpha) and the G-buffer into NRD v4.17.3 REBLUR inputs:
//   IN_NORMAL_ROUGHNESS  NRD_FrontEnd_PackNormalAndRoughness(the path tracer's viewer-facing, horizon-corrected shading
//                        normal, G-buffer roughness, material ID),
//                        R10G10B10A2 (NRD_NORMAL_ENCODING 2, NRD_ROUGHNESS_ENCODING 1, the library's build)
//   IN_VIEWZ             linear view-space depth (+Z forward), R32F; 2 * denoisingRange + 1 where there is no surface
//   IN_DIFF/SPEC_RADIANCE_HITDIST
//                        REBLUR_FrontEnd_PackRadianceAndNormHitDist(L / (factor * scale), normalized hit distance), RGBA16F
//   IN_MV                the G-buffer motion, copied unchanged (RGBA16F) for every pixel: NRD's integration binds its
//                        resources as unordered-access views, which the G-buffer render target does not allow (NRD.4B).
//                        motionVectorScale (-1, -1, 1) still applies (see the NRD.3 report).
//
// Demodulation (NRD's "material demodulation"): L_diffuse / diffuseFactor and L_specular / specularFactor with
// NRD_MaterialFactors of this pixel's G-buffer surface. CSResolve recomputes the same factors with the same function
// from the same texel and multiplies them back, so NRD(L / k) * k returns L wherever the denoiser passes a signal
// through; the factors' accuracy affects filtering quality only, never energy.
//
// Hit distance: -1 (no sample took that lobe) packs as 0, "no data". Diffuse (NRD.4A): the extraction already stores
// REBLUR's estimator, the mean of per-sample normalized distances (PathTraceReference.hlsl), which packs unchanged.
// Specular: PATH_TRACE_HALF_MAXIMUM (every such continuation missed) packs as 1; a hit packs as
// REBLUR_FrontEnd_GetNormHitDist(hitT, viewZ, A B C, roughness).
//
// Radiance scale (NRD.4A): demodulated radiance is divided by radianceScale before packing, so it stays inside fp16
// (REBLUR's input and history format) whenever the raw signal does: the extraction clamps at 65504 and the factors
// are at least NRD_MATERIAL_FACTOR_MIN_SCALE (0.02), so a scale of 64 or more can never clip.
//
// CSResolveDenoised (NRD.4B) is the production resolve: REBLUR's outputs, unpacked and remodulated with the same
// factors and scale, summed into linear HDR radiance on surface pixels of the path-traced presentation. Background
// pixels keep the path tracer's own value, as with the Engine denoiser.
//
// CSResolve is the NRD bypass: it unpacks CSPrepare's own outputs as if NRD had returned them unchanged
// (REBLUR_BackEnd_UnpackRadianceAndNormHitDist), remodulates, and writes the reconstructed raw signals in the extraction
// layout (radiance, hit distance) at RGBA32F, so their comparison with the extraction measures only the NRD formats.

#include "MaterialLighting.hlsli"
#include "ShadingNormal.hlsli"

#define NRD_NORMAL_ENCODING 2
#define NRD_ROUGHNESS_ENCODING 1
#include "NRD/NRD.hlsli"

struct NrdPathTracePrepareConstants
{
    row_major float4x4 inverseViewProjection;
    row_major float4x4 view;

    float3 cameraPosition;
    float denoisingRange;

    uint2 resolution;
    uint depthIndex;
    uint materialIndex;

    uint surfaceIndex;
    uint geometricNormalIndex;
    uint materialBufferIndex;
    uint rawDiffuseIndex;

    uint rawSpecularIndex;
    uint normalRoughnessIndex;
    uint viewZIndex;
    uint diffuseIndex;

    uint specularIndex;
    uint reconstructedDiffuseIndex;
    uint reconstructedSpecularIndex;
    float hitDistanceA;

    float hitDistanceB;
    float hitDistanceC;
    // NRD.4A: the packed radiance is L / (factor * radianceScale), unpacked as * factor * radianceScale (a power of two,
    // so both are exact). See NrdPathTracePreparePass.h.
    float radianceScale;
    // NRD.4B: the G-buffer motion (SRV) and its IN_MV copy (UAV).
    uint motionIndex;

    // NRD.4B production resolve: REBLUR's diffuse and specular outputs (SRVs) and the path-traced presentation (UAV).
    uint denoisedDiffuseIndex;
    uint denoisedSpecularIndex;
    uint presentationIndex;
    uint motionOutputIndex;

    // NRD.4C diagnostics: 0 both components, 1 diffuse only, 2 specular only (CSResolveDenoised).
    uint resolveComponents;
    // NRD.4D diagnostics: the specular hit-distance alpha (NrdPathTracePreparePass.h); 0 in production.
    uint specularHitDistanceMode;
    uint2 padding;
};

ConstantBuffer<NrdPathTracePrepareConstants> constants : register(b0);

static const float NRD_PATH_TRACE_HALF_MAXIMUM = 65504.0f;
// PathTraceBSDF.hlsli: PATH_TRACE_DELTA_ROUGHNESS, at or below which the single-scatter lobe is a delta mirror.
static const float NRD_PATH_TRACE_DELTA_ROUGHNESS = 0.001f;

// The primary surface as NRD sees it, and the demodulation factors of its two signals.
struct NrdPathTraceSurface
{
    bool valid;
    float viewZ;
    float3 normal;
    float roughness;
    // 0 dielectric, 1 conductor or metallic (NRD's materialID; 2 bits in IN_NORMAL_ROUGHNESS).
    float materialID;
    float3 diffuseFactor;
    float3 specularFactor;
};

// The diffuse albedo and Schlick F0 of the path tracer's material models (PathTraceDiffuseAlbedo, PathTraceSchlickF0;
// conductors use their average Fresnel), from the G-buffer's resolved base colour and metallic.
void NrdPathTraceMaterialAlbedo(GPUMaterial material, float3 baseColor, float metallic, out float3 diffuseAlbedo,
    out float3 specularF0)
{
    if(material.model == 0u)
    {
        diffuseAlbedo = baseColor * (1.0f - metallic);
        specularF0 = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallic);
        return;
    }

    if(material.model == 1u)
    {
        float iorDifference = material.ior - 1.0f;
        float iorSum = material.ior + 1.0f;
        float dielectricF0 = (iorDifference * iorDifference) / max(iorSum * iorSum, 1.0e-6f);
        diffuseAlbedo = baseColor;
        specularF0 = float3(dielectricF0, dielectricF0, dielectricF0);
        return;
    }

    diffuseAlbedo = float3(0.0f, 0.0f, 0.0f);
    specularF0 = saturate(material.averageConductorFresnel);
}

// Shared by CSPrepare and CSResolve, so demodulation and remodulation use the same factors.
NrdPathTraceSurface NrdPathTraceLoadSurface(uint2 pixel)
{
    NrdPathTraceSurface result = (NrdPathTraceSurface)0;
    result.normal = float3(0.0f, 0.0f, 1.0f);
    result.roughness = 1.0f;
    result.diffuseFactor = float3(1.0f, 1.0f, 1.0f);
    result.specularFactor = float3(1.0f, 1.0f, 1.0f);

    Texture2D<float> depthTexture = ResourceDescriptorHeap[constants.depthIndex];
    float depth = depthTexture.Load(int3(pixel, 0));

    if(!isfinite(depth) || depth < 0.0f || depth >= 1.0f)
    {
        return result;
    }

    // The G-buffer is rasterized with the jittered projection, so the render inverse puts the pixel centre on the
    // rasterized surface.
    float2 uv = (float2(pixel) + 0.5f) / float2(constants.resolution);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 worldPosition = mul(constants.inverseViewProjection, float4(ndc, depth, 1.0f));

    if(!all(isfinite(worldPosition)) || abs(worldPosition.w) < 1.0e-8f)
    {
        return result;
    }

    worldPosition /= worldPosition.w;
    float viewZ = mul(constants.view, worldPosition).z;

    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[constants.surfaceIndex];
    Texture2D<float4> materialTexture = ResourceDescriptorHeap[constants.materialIndex];
    Texture2D<float4> geometricNormalTexture = ResourceDescriptorHeap[constants.geometricNormalIndex];
    float4 surface = surfaceTexture.Load(int3(pixel, 0));
    float4 materialData = materialTexture.Load(int3(pixel, 0));
    float4 geometricNormalData = geometricNormalTexture.Load(int3(pixel, 0));
    float metallic = saturate(geometricNormalData.w);

    if(!isfinite(viewZ) || viewZ <= 0.0f || !all(isfinite(surface)) || !all(isfinite(materialData)) ||
        dot(surface.xyz, surface.xyz) < 1.0e-8f)
    {
        return result;
    }

    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[constants.materialBufferIndex];
    uint materialCount;
    uint materialStride;
    materials.GetDimensions(materialCount, materialStride);
    float materialIndex = round(surface.w);

    if(materialIndex < 0.0f || materialIndex >= float(materialCount))
    {
        return result;
    }

    GPUMaterial material = materials[uint(materialIndex)];

    if(material.model > 2u)
    {
        return result;
    }

    float3 normal = normalize(surface.xyz);
    float roughness = saturate(materialData.a);
    float3 diffuseAlbedo;
    float3 specularF0;
    NrdPathTraceMaterialAlbedo(material, max(materialData.rgb, 0.0f), metallic, diffuseAlbedo, specularF0);

    float3 toCamera = constants.cameraPosition - worldPosition.xyz;
    float3 view = toCamera * rsqrt(max(dot(toCamera, toCamera), 1.0e-30f));

    // FOUNDATION.5: the normal the path tracer shaded the signal with (PathTraceDecodePrimaryVertex, then
    // PathTraceFinalizeSurface): both normals face the viewer, then the view-horizon correction. Without a usable
    // geometric normal the path tracer has no surface either, and the G-buffer normal is kept.
    float geometricLengthSquared = dot(geometricNormalData.xyz, geometricNormalData.xyz);

    if(all(isfinite(geometricNormalData.xyz)) && geometricLengthSquared > 1.0e-8f)
    {
        float3 geometricNormal = geometricNormalData.xyz * rsqrt(geometricLengthSquared);

        if(dot(geometricNormal, view) < 0.0f)
        {
            geometricNormal = -geometricNormal;
            normal = -normal;
        }

        normal = CorrectShadingNormalViewHorizon(normal, geometricNormal, view);
    }

    result.valid = true;
    result.viewZ = viewZ;
    result.normal = normal;
    // A delta mirror is roughness 0 for NRD, as for the path tracer.
    result.roughness = roughness <= NRD_PATH_TRACE_DELTA_ROUGHNESS ? 0.0f : roughness;
    result.materialID = material.model == 2u || metallic >= 0.5f ? 1.0f : 0.0f;
    NRD_MaterialFactors(normal, view, diffuseAlbedo, specularF0, result.roughness, result.diffuseFactor,
        result.specularFactor);
    return result;
}

// The packed RGBA16F inputs, rounded to the nearest fp16 value before the store. The typed store's float32-to-float16
// conversion may truncate toward zero (D3D allows it), and on this GPU it and f32tof16 both do: through the round trip
// that measured a systematic -3.5e-4 luminance bias. f32tof16's value and the next fp16 value away from zero bracket
// the input; the nearer one is kept, and an fp16-representable value stores exactly.
float4 NrdPathTraceRoundToHalf(float4 value)
{
    uint4 truncatedBits = f32tof16(value);
    float4 truncated = f16tof32(truncatedBits);
    float4 next = f16tof32(truncatedBits + 1u);
    return select(abs(value - next) < abs(value - truncated), next, truncated);
}

float3 NrdPathTraceHitDistanceParameters()
{
    return float3(constants.hitDistanceA, constants.hitDistanceB, constants.hitDistanceC);
}

// The extraction's hit distance (-1 none, PATH_TRACE_HALF_MAXIMUM all missed, else the nearest hit) as REBLUR's.
float NrdPathTraceNormalizedHitDistance(float hitDistance, float viewZ, float roughness)
{
    if(!(hitDistance >= 0.0f))
    {
        return 0.0f;
    }

    if(hitDistance >= NRD_PATH_TRACE_HALF_MAXIMUM)
    {
        return 1.0f;
    }

    return REBLUR_FrontEnd_GetNormHitDist(hitDistance, viewZ, NrdPathTraceHitDistanceParameters(), roughness);
}

// The inverse where one exists: 0 is "no data" (-1), 1 is "at or beyond the normalization distance" (reported as
// PATH_TRACE_HALF_MAXIMUM).
float NrdPathTraceDenormalizedHitDistance(float normalizedHitDistance, float viewZ, float roughness)
{
    if(!(normalizedHitDistance > 0.0f))
    {
        return -1.0f;
    }

    if(normalizedHitDistance >= 1.0f)
    {
        return NRD_PATH_TRACE_HALF_MAXIMUM;
    }

    return normalizedHitDistance * _REBLUR_GetHitDistanceNormalization(viewZ, NrdPathTraceHitDistanceParameters(),
        roughness);
}

[numthreads(8, 8, 1)]
void CSPrepare(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;

    if(any(pixel >= constants.resolution))
    {
        return;
    }

    RWTexture2D<float4> normalRoughnessOutput = ResourceDescriptorHeap[constants.normalRoughnessIndex];
    RWTexture2D<float> viewZOutput = ResourceDescriptorHeap[constants.viewZIndex];
    RWTexture2D<float4> diffuseOutput = ResourceDescriptorHeap[constants.diffuseIndex];
    RWTexture2D<float4> specularOutput = ResourceDescriptorHeap[constants.specularIndex];
    Texture2D<float4> motion = ResourceDescriptorHeap[constants.motionIndex];
    RWTexture2D<float4> motionOutput = ResourceDescriptorHeap[constants.motionOutputIndex];
    motionOutput[pixel] = motion.Load(int3(pixel, 0));
    NrdPathTraceSurface surface = NrdPathTraceLoadSurface(pixel);

    // No surface (background, unusable G-buffer data): outside the denoising range, no radiance, no hit distance.
    if(!surface.valid)
    {
        normalRoughnessOutput[pixel] = NRD_FrontEnd_PackNormalAndRoughness(float3(0.0f, 0.0f, 1.0f), 1.0f, 0.0f);
        viewZOutput[pixel] = max(constants.denoisingRange, 0.0f) * 2.0f + 1.0f;
        diffuseOutput[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        specularOutput[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    Texture2D<float4> rawDiffuse = ResourceDescriptorHeap[constants.rawDiffuseIndex];
    Texture2D<float4> rawSpecular = ResourceDescriptorHeap[constants.rawSpecularIndex];
    float4 diffuse = rawDiffuse.Load(int3(pixel, 0));
    float4 specular = rawSpecular.Load(int3(pixel, 0));

    float diffuseHitDistance = diffuse.a < 0.0f ? 0.0f : diffuse.a;
    float specularHitDistance = NrdPathTraceNormalizedHitDistance(specular.a, surface.viewZ, surface.roughness);

    if(constants.specularHitDistanceMode == 1u)
    {
        specularHitDistance = 1.0f;
    }
    else if(constants.specularHitDistanceMode == 2u)
    {
        specularHitDistance = 0.1f;
    }
    else if(constants.specularHitDistanceMode == 3u && specularHitDistance <= 0.0f)
    {
        specularHitDistance = 1.0f;
    }

    normalRoughnessOutput[pixel] = NRD_FrontEnd_PackNormalAndRoughness(surface.normal, surface.roughness,
        surface.materialID);
    viewZOutput[pixel] = surface.viewZ;
    diffuseOutput[pixel] = NrdPathTraceRoundToHalf(REBLUR_FrontEnd_PackRadianceAndNormHitDist(
        diffuse.rgb / (surface.diffuseFactor * constants.radianceScale), diffuseHitDistance, true));
    specularOutput[pixel] = NrdPathTraceRoundToHalf(REBLUR_FrontEnd_PackRadianceAndNormHitDist(
        specular.rgb / (surface.specularFactor * constants.radianceScale), specularHitDistance, true));
}

[numthreads(8, 8, 1)]
void CSResolve(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;

    if(any(pixel >= constants.resolution))
    {
        return;
    }

    RWTexture2D<float4> reconstructedDiffuse = ResourceDescriptorHeap[constants.reconstructedDiffuseIndex];
    RWTexture2D<float4> reconstructedSpecular = ResourceDescriptorHeap[constants.reconstructedSpecularIndex];
    NrdPathTraceSurface surface = NrdPathTraceLoadSurface(pixel);

    // The extraction's layout for a pixel without a surface: no radiance, no hit distance. A production resolve keeps
    // the path tracer's own value there, as the Engine denoiser does.
    if(!surface.valid)
    {
        reconstructedDiffuse[pixel] = float4(0.0f, 0.0f, 0.0f, -1.0f);
        reconstructedSpecular[pixel] = float4(0.0f, 0.0f, 0.0f, -1.0f);
        return;
    }

    // The bypass: NRD's outputs are taken to be its inputs.
    Texture2D<float4> diffuseInput = ResourceDescriptorHeap[constants.diffuseIndex];
    Texture2D<float4> specularInput = ResourceDescriptorHeap[constants.specularIndex];
    float4 diffuse = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(diffuseInput.Load(int3(pixel, 0)));
    float4 specular = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(specularInput.Load(int3(pixel, 0)));

    // The diffuse hit distance comes back in the extraction's own (normalized) form.
    reconstructedDiffuse[pixel] = float4(diffuse.rgb * (surface.diffuseFactor * constants.radianceScale),
        diffuse.a > 0.0f ? diffuse.a : -1.0f);
    reconstructedSpecular[pixel] = float4(specular.rgb * (surface.specularFactor * constants.radianceScale),
        NrdPathTraceDenormalizedHitDistance(specular.a, surface.viewZ, surface.roughness));
}

[numthreads(8, 8, 1)]
void CSResolveDenoised(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;

    if(any(pixel >= constants.resolution))
    {
        return;
    }

    NrdPathTraceSurface surface = NrdPathTraceLoadSurface(pixel);

    if(!surface.valid)
    {
        return;
    }

    Texture2D<float4> denoisedDiffuse = ResourceDescriptorHeap[constants.denoisedDiffuseIndex];
    Texture2D<float4> denoisedSpecular = ResourceDescriptorHeap[constants.denoisedSpecularIndex];
    RWTexture2D<float4> presentation = ResourceDescriptorHeap[constants.presentationIndex];
    float3 diffuse = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(denoisedDiffuse.Load(int3(pixel, 0))).rgb;
    float3 specular = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(denoisedSpecular.Load(int3(pixel, 0))).rgb;
    float diffuseWeight = constants.resolveComponents == 2 ? 0.0f : 1.0f;
    float specularWeight = constants.resolveComponents == 1 ? 0.0f : 1.0f;
    float3 radiance = (diffuse * surface.diffuseFactor * diffuseWeight + specular * surface.specularFactor *
        specularWeight) * constants.radianceScale;

    // A non-finite output never reaches the image; the path tracer's value stays.
    if(all(isfinite(radiance)))
    {
        presentation[pixel] = float4(min(max(radiance, 0.0f), NRD_PATH_TRACE_HALF_MAXIMUM), 1.0f);
    }
}
