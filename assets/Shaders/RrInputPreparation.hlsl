// RR.1B DLSS Ray Reconstruction input preparation (RrInputPreparationPass). One compute pass at the render extent, after
// the path tracer's signal extraction and the DLSS input preparation (DlssInputPreparation.hlsl), producing what RR
// evaluation (RR.1C, Renderer::RecordRayReconstructionEvaluation) consumes besides depth and the ordinary motion vectors. Nothing here changes lighting: it reads the
// extraction, the G-buffer (after PSR.2's in-place replacement) and the path tracer's presentation, and writes its own
// targets only.
//
// Outputs (render extent):
//   colour            RGBA16F  this frame's radiance: extraction diffuse + specular on surface pixels (the extraction is
//                              the frame's own samples, the specular part being the frame mean minus the diffuse one, so
//                              the sum is the frame mean with nothing counted twice); the path tracer's presentation on
//                              background pixels (the environment, as presented today). No clamp, no normalization.
//                              RR.1C: times colorScale (1 except in the exposure experiment's PreExposed mode).
//   diffuse albedo    RGBA8    linear (RrMaterialReflectance); background 0.5 (RR guide 3.4.1)
//   specular albedo   RGBA8    RrEnvBRDFApprox2(F0, r^2, N.V); background 0 (RR guide 3.4.2, appendix)
//   normal+roughness  RGBA16F  the world-space shading normal the path tracer shaded with (both normals facing the viewer,
//                              then the view-horizon correction, as NRD's preparation); alpha the perceptual roughness
//                              (the G-buffer's, which the BSDF squares to alpha; the guide's appendix squares it too).
//                              Background (0, 0, 0, 0) (RR guide 3.4.3, 3.4.4)
//   specular motion   RG16F    DLSS convention as DlssInputPreparation.hlsl: previous = current + vector, render pixels,
//                              jitter excluded. See RrSpecularMotion.
//   throughput        RGBA16F  diagnostics only (UINT_MAX: not written): rgb the PSR mirror throughput applied to the
//                              albedos, a the PSR chain length (0 ordinary, 1, 2; -1 a replaced pixel without PSR bindings)
//   second mirror     RGBA32F  diagnostics only (RR.1C): a two-mirror pixel's F_B (rgb) and B's material index +
//                              |N.V| / 2 (a); (1, 1, 1, -1) elsewhere
//
// PSR.2 (RR_INPUT_PSR, the RrInputPreparationPsr.hlsl variant; b1, t0, s0 as NRD's PSR preparation): a replaced pixel's
// G-buffer holds the virtual surface (B or, for a two-mirror chain, D). Its normal, roughness and depth are used as they
// are; its albedos are B's (or D's) times the mirror chain's throughput: the first mirror's delta weight F_A, recovered
// exactly as NRD's PSR preparation recovers it (PsrDenoise.hlsli) but without NRD's 0.02 demodulation floor, and (RR.1C)
// for a two-mirror chain also the second mirror's F_B, which the path tracer's radiance at D carries too. F_B is retraced
// exactly as the path tracer retraces the chain's first segment (RrPsrChain.hlsli; t1 the TLAS, s1 the path tracer's
// material sampler). NRD's guides keep F_A only. Each factor applies once. The specular motion of a replaced pixel is its
// G-buffer motion, already the virtual image's.

#include "RrGuides.hlsli"
#include "ShadingNormal.hlsli"

struct RrInputPreparationConstants
{
    // Render (jittered) inverse view-projection, world space: reconstructs the rasterized sample of each pixel centre.
    row_major float4x4 inverseViewProjection;
    // Unjittered current and previous view-projections, world space (the motion-vector convention).
    row_major float4x4 viewProjection;
    row_major float4x4 previousViewProjection;
    float3 cameraPosition;
    uint width;
    uint height;
    uint depthIndex;
    uint materialIndex;
    uint surfaceIndex;
    uint geometricNormalIndex;
    uint gBufferMotionIndex;
    uint materialBufferIndex;
    uint diffuseExtractionIndex;
    uint specularExtractionIndex;
    uint presentationIndex;
    uint motionIndex;
    uint colorOutputIndex;
    uint diffuseAlbedoOutputIndex;
    uint specularAlbedoOutputIndex;
    uint normalRoughnessOutputIndex;
    uint specularMotionOutputIndex;
    uint throughputOutputIndex;
    // Perceptual roughness below which a surface's reflection is treated as specular enough for virtual-image motion.
    float specularMotionRoughnessThreshold;
    // RR.1C: the noisy colour's scale (1 in production) and the second-mirror diagnostic UAV (UINT_MAX: not written).
    float colorScale;
    uint secondMirrorOutputIndex;
};

ConstantBuffer<RrInputPreparationConstants> constants : register(b0);

#ifdef RR_INPUT_PSR
#define PSR_DENOISE_CONSTANTS_REGISTER b1
#define PSR_DENOISE_TRANSFORMS_REGISTER t0
#define PSR_DENOISE_SAMPLER_REGISTER s0
#include "PsrDenoise.hlsli"
#define RR_PSR_CHAIN_SCENE_REGISTER t1
#define RR_PSR_CHAIN_SAMPLER_REGISTER s1
#include "RrPsrChain.hlsli"

// PsrDenoiseThroughput without its floor: the mirror chain's throughput of a replaced pixel, F_A, times F_B for a two-mirror
// chain (chain 2), or false where the first mirror cannot be recovered (the path tracer then has no replaced surface there
// either). secondMirror: F_B and B's material index + N.V / 2, (1, 1, 1, -1) when there is no second mirror.
bool RrPsrThroughput(uint2 pixel, uint chain, out float3 throughput, out float4 secondMirror)
{
    throughput = float3(1.0f, 1.0f, 1.0f);
    secondMirror = float4(1.0f, 1.0f, 1.0f, -1.0f);
    Texture2D<uint2> identityTexture = ResourceDescriptorHeap[psrDenoise.identityIndex];
    uint2 identity = identityTexture.Load(int3(pixel, 0));

    if(!PsrIdentityEligible(identity.x))
    {
        return false;
    }

    PsrMirrorCamera mirrorCamera;
    mirrorCamera.camera.inverseCameraRelativeViewProjection = psrDenoise.inverseCameraRelativeViewProjection;
    mirrorCamera.camera.cameraPosition = psrDenoise.cameraPosition;
    mirrorCamera.camera.resolution = psrDenoise.resolution;
    mirrorCamera.cameraRelativeViewProjection = psrDenoise.cameraRelativeViewProjection;
    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[psrDenoise.materialBufferIndex];
    PsrRecoveredMirror mirror;

    if(!PsrRecoverMirror(pixel, identity, mirrorCamera, psrDenoiseTransforms, psrDenoise.transformCount,
        psrDenoise.instanceDataIndex, materials, psrDenoise.materialTableIndex, psrDenoise.materialAverageTableIndex,
        psrDenoise.materialTableCosineMapping, psrDenoiseMirrorSampler, psrDenoise.textureMipBias, true, mirror))
    {
        return false;
    }

    float3 diffuseShare;
    bool specularStrategy;
    PathTraceBSDFSample sample = PathTraceSampleBSDFLobes(mirror.vertex.surface, float3(0.0f, 0.0f, 0.0f), diffuseShare,
        specularStrategy);

    if(!sample.valid || !sample.delta || !all(isfinite(sample.weight)))
    {
        return false;
    }

    throughput = max(sample.weight, 0.0f);

    // RR.1C: a two-mirror chain's radiance at D carries B's delta weight as well. A chain whose first segment does not
    // reach a pure mirror keeps F_A (the path tracer's chain would not exist there).
    float3 secondWeight;
    uint secondMaterial;
    float secondNormalDotView;

    if(chain == 2u && RrPsrSecondMirror(mirror, sample, secondWeight, secondMaterial, secondNormalDotView))
    {
        throughput *= secondWeight;
        secondMirror = float4(secondWeight, float(secondMaterial) + 0.5f * saturate(secondNormalDotView));
    }

    return true;
}
#else
static const float PATH_TRACE_PRIMARY_MAXIMUM_MATERIALS = 2048.0f;
#endif

static const float RR_HALF_MAXIMUM = 65504.0f;

float2 RrClipToUV(float4 clip)
{
    float2 ndc = clip.xy / clip.w;
    return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
}

// The reflection-aware motion of an ordinary (not replaced) glossy pixel, false when it cannot be estimated reliably.
// The specular continuation's hit lies hitDistance beyond the surface along the reflected ray; for a planar reflector its
// mirror image lies the same distance beyond the surface along the camera ray. That virtual point is projected through the
// unjittered current and previous view-projections (as RTXPT's ComputeSpecularMotionVector, without its reflector-motion
// term: Engine's G-buffer holds screen-space, not world-space, motion). Exact for a static planar reflector of static
// geometry under camera motion; an approximation for curved or normal-mapped reflectors, moving reflectors and moving
// reflected geometry (RR.1B report, section 8).
bool RrSpecularMotion(float3 worldPosition, float3 view, float roughness, float hitDistance, float2 size,
    out float2 motion)
{
    motion = float2(0.0f, 0.0f);

    // -1: no sample took the specular lobe; 65504: every specular continuation missed (sky); also non-finite.
    if(!(roughness < constants.specularMotionRoughnessThreshold) || !isfinite(hitDistance) || hitDistance <= 1.0e-3f ||
        hitDistance >= RR_HALF_MAXIMUM)
    {
        return false;
    }

    float3 virtualPoint = worldPosition - view * hitDistance;
    float4 currentClip = mul(constants.viewProjection, float4(virtualPoint, 1.0f));
    float4 previousClip = mul(constants.previousViewProjection, float4(virtualPoint, 1.0f));

    if(!(currentClip.w > 1.0e-6f) || !(previousClip.w > 1.0e-6f))
    {
        return false;
    }

    float2 candidate = (RrClipToUV(previousClip) - RrClipToUV(currentClip)) * size;

    if(!all(isfinite(candidate)))
    {
        return false;
    }

    motion = candidate;
    return true;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThread : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThread.xy;

    if(pixel.x >= constants.width || pixel.y >= constants.height)
    {
        return;
    }

    const float2 size = float2(constants.width, constants.height);
    Texture2D<float> depthTexture = ResourceDescriptorHeap[constants.depthIndex];
    Texture2D<float4> presentation = ResourceDescriptorHeap[constants.presentationIndex];
    Texture2D<float4> diffuseExtraction = ResourceDescriptorHeap[constants.diffuseExtractionIndex];
    Texture2D<float4> specularExtraction = ResourceDescriptorHeap[constants.specularExtractionIndex];
    Texture2D<float2> motionTexture = ResourceDescriptorHeap[constants.motionIndex];
    RWTexture2D<float4> colorOutput = ResourceDescriptorHeap[constants.colorOutputIndex];
    RWTexture2D<float4> diffuseAlbedoOutput = ResourceDescriptorHeap[constants.diffuseAlbedoOutputIndex];
    RWTexture2D<float4> specularAlbedoOutput = ResourceDescriptorHeap[constants.specularAlbedoOutputIndex];
    RWTexture2D<float4> normalRoughnessOutput = ResourceDescriptorHeap[constants.normalRoughnessOutputIndex];
    RWTexture2D<float2> specularMotionOutput = ResourceDescriptorHeap[constants.specularMotionOutputIndex];

    const float depth = depthTexture.Load(int3(pixel, 0));
    const float2 surfaceMotion = motionTexture.Load(int3(pixel, 0));
    const float4 specular = specularExtraction.Load(int3(pixel, 0));

    // Background defaults (RR guide 3.4.1-3.4.4), and the defaults of any surface pixel the guides cannot describe.
    float3 diffuseAlbedo = float3(0.5f, 0.5f, 0.5f);
    float3 specularAlbedo = float3(0.0f, 0.0f, 0.0f);
    float4 normalRoughness = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float2 specularMotion = surfaceMotion;
    float4 throughputDiagnostic = float4(1.0f, 1.0f, 1.0f, 0.0f);
    float4 secondMirrorDiagnostic = float4(1.0f, 1.0f, 1.0f, -1.0f);
    const bool surfacePixel = isfinite(depth) && depth >= 0.0f && depth < 1.0f;
    float3 color = (surfacePixel ? diffuseExtraction.Load(int3(pixel, 0)).rgb + specular.rgb :
        presentation.Load(int3(pixel, 0)).rgb) * constants.colorScale;

    if(surfacePixel)
    {
        Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[constants.surfaceIndex];
        Texture2D<float4> materialTexture = ResourceDescriptorHeap[constants.materialIndex];
        Texture2D<float4> geometricNormalTexture = ResourceDescriptorHeap[constants.geometricNormalIndex];
        StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[constants.materialBufferIndex];
        const float4 surface = surfaceTexture.Load(int3(pixel, 0));
        // The material target is sRGB: the SRV read returns linear base colour (and roughness, stored linearly).
        const float4 materialData = materialTexture.Load(int3(pixel, 0));
        const float4 geometricNormalData = geometricNormalTexture.Load(int3(pixel, 0));

        // The pixel centre's rasterized sample (the G-buffer is rasterized with the jittered projection).
        const float2 uv = (float2(pixel) + 0.5f) / size;
        float4 worldPosition = mul(constants.inverseViewProjection, float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f));
        uint materialCount;
        uint materialStride;
        materials.GetDimensions(materialCount, materialStride);
        float materialIndex = round(surface.w);
        // A replaced pixel's word is -(i + 1): the virtual surface's material i.
        const bool replaced = materialIndex < 0.0f;
        materialIndex = replaced ? -materialIndex - 1.0f : materialIndex;

        const bool decodable = all(isfinite(worldPosition)) && abs(worldPosition.w) > 1.0e-8f && all(isfinite(surface)) &&
            all(isfinite(materialData)) && dot(surface.xyz, surface.xyz) > 1.0e-8f && materialIndex >= 0.0f &&
            materialIndex < float(materialCount) && materialIndex < PATH_TRACE_PRIMARY_MAXIMUM_MATERIALS;

        if(decodable && materials[uint(materialIndex)].model <= 2u)
        {
            worldPosition /= worldPosition.w;
            const GPUMaterial material = materials[uint(materialIndex)];
            const float3 toCamera = constants.cameraPosition - worldPosition.xyz;
            const float3 view = toCamera * rsqrt(max(dot(toCamera, toCamera), 1.0e-30f));
            float3 normal = normalize(surface.xyz);
            const float geometricLengthSquared = dot(geometricNormalData.xyz, geometricNormalData.xyz);

            // The normal the path tracer shaded with: both normals face the viewer, then the view-horizon correction.
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

            const float roughness = saturate(materialData.a);
            float3 specularF0;
            RrMaterialReflectance(material, max(materialData.rgb, 0.0f), saturate(geometricNormalData.w), diffuseAlbedo,
                specularF0);
            specularAlbedo = RrEnvBRDFApprox2(specularF0, roughness * roughness, dot(normal, view));
            normalRoughness = float4(normal, roughness);

            if(replaced)
            {
                // The virtual surface's motion is already in the ordinary vectors (PsrInPlaceReplace.hlsl): no second
                // reflection.
                float3 throughput = float3(1.0f, 1.0f, 1.0f);
                throughputDiagnostic.a = -1.0f;
#ifdef RR_INPUT_PSR
                // The chain length is the replacement pass's motion.w (1, or 2 for a two-mirror chain).
                Texture2D<float4> gBufferMotion = ResourceDescriptorHeap[constants.gBufferMotionIndex];
                const uint chain = gBufferMotion.Load(int3(pixel, 0)).w == 2.0f ? 2u : 1u;

                if(RrPsrThroughput(pixel, chain, throughput, secondMirrorDiagnostic))
                {
                    throughputDiagnostic.a = float(chain);
                }
#endif
                diffuseAlbedo *= throughput;
                specularAlbedo *= throughput;
                throughputDiagnostic.rgb = throughput;
            }
            else
            {
                float2 motion;

                if(RrSpecularMotion(worldPosition.xyz, view, roughness, specular.a, size, motion))
                {
                    specularMotion = motion;
                }
            }
        }
    }

    colorOutput[pixel] = float4(color, 1.0f);
    diffuseAlbedoOutput[pixel] = float4(saturate(diffuseAlbedo), 1.0f);
    specularAlbedoOutput[pixel] = float4(saturate(specularAlbedo), 1.0f);
    normalRoughnessOutput[pixel] = normalRoughness;
    specularMotionOutput[pixel] = all(isfinite(specularMotion)) ? specularMotion : float2(0.0f, 0.0f);

    if(constants.throughputOutputIndex != 0xFFFFFFFFu)
    {
        RWTexture2D<float4> throughputOutput = ResourceDescriptorHeap[constants.throughputOutputIndex];
        throughputOutput[pixel] = throughputDiagnostic;
    }

    if(constants.secondMirrorOutputIndex != 0xFFFFFFFFu)
    {
        RWTexture2D<float4> secondMirrorOutput = ResourceDescriptorHeap[constants.secondMirrorOutputIndex];
        secondMirrorOutput[pixel] = secondMirrorDiagnostic;
    }
}
