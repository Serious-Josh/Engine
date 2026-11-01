#pragma once

#include "Math/Matrix4.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

struct MaterialConstants
{
    unsigned int textureIndex;
    float roughness;
    unsigned int materialIndex;
    unsigned int materialBufferIndex;

    float baseColor[3];
    unsigned int roughnessTextureIndex;
};

struct CameraConstants
{
    // Rasterization: jittered while UPSCALING.2B camera jitter is active, otherwise unjittered.
    Matrix4 viewProjection;
    // Motion vectors: always unjittered (previous and current).
    Matrix4 previousViewProjection;
    Matrix4 view;
    Matrix4 previousView;
    Matrix4 unjitteredViewProjection;
    // UPSCALING.3D: material texture mip bias (Triangle.hlsl); non-zero only on DLSS frames.
    float textureMipBias = 0.0f;
    // PSR.2: non-zero only on Path Tracing frames with in-place replacement enabled (Triangle.hlsl marks PSR-eligible
    // mirrors in identity.x bit 31), with the path tracer's camera for the decode it evaluates.
    std::uint32_t psrEligibility = 0;
    std::uint32_t psrResolution[2] = {};
    Matrix4 psrInverseCameraRelativeViewProjection;
    float psrCameraPosition[3] = {};
    float psrPadding = 0.0f;
};

struct ObjectConstants
{
    Matrix4 model;
    Matrix4 previousModel;
    Matrix4 normalMatrix;
};

struct ReconstructionConstants
{
    Matrix4 inverseViewProjection;

    float cameraPosition[3];
    unsigned int geometricNormalIndex;
    uint32_t motionIndex;
    uint32_t padding[3];
};

struct ShadowConstants
{
    Matrix4 inverseViewProjection;
    Matrix4 previousViewProjection;
    Matrix4 previousInverseViewProjection;

    float lightDirection[3];
    unsigned int depthIndex;

    unsigned int surfaceIndex;
    float lightAngularRadius;
    unsigned int frameIndex;
    unsigned int historyIndex;

    unsigned int previousDepthIndex;
    unsigned int outputDepthIndex;
    unsigned int previousHistoryLengthIndex;
    unsigned int outputHistoryLengthIndex;

    unsigned int accumulatedFrameCount;
    unsigned int geometricNormalIndex;
    float padding[2];
};

struct ReflectionConstants
{
    Matrix4 inverseViewProjection;

    float cameraPosition[3];
    unsigned int depthIndex;

    unsigned int materialIndex;
    unsigned int surfaceIndex;
    unsigned int instanceDataIndex;
    unsigned int materialBufferIndex;

    unsigned int environmentIndex;
    unsigned int environmentCubemapIndex;
    unsigned int diffuseIrradianceIndex;
    unsigned int ggxMultiscatterIrradianceIndex;

    unsigned int brdfIntegrationLUTIndex;
    unsigned int ggxAverageAlbedoLUTIndex;
    unsigned int frameIndex;
    unsigned int importanceConditionalCDFIndex;

    unsigned int importanceMarginalCDFIndex;
    unsigned int importanceWidth;
    unsigned int importanceHeight;
    float importanceTotalWeight;

    unsigned int geometricNormalIndex;
    unsigned int padding[3];
};

struct GIConstants
{
    Matrix4 inverseViewProjection;

    unsigned int depthIndex = 0;
    unsigned int surfaceIndex = 0;
    unsigned int frameIndex = 0;
    unsigned int instanceDataIndex = 0;

    unsigned int materialBufferIndex = 0;
    unsigned int environmentIndex = 0;
    unsigned int diffuseIrradianceIndex = 0;
    unsigned int diffuseIrradianceArrayIndex = 0;

    unsigned int geometricNormalIndex = 0;
    unsigned int fullResolution[2];
    unsigned int signedCorrectionIndex = 0xFFFFFFFFu;

    // Appended fields preserve the historical cooked GI constant-buffer prefix.
    unsigned int importanceConditionalCDFIndex = 0xFFFFFFFFu;
    unsigned int importanceMarginalCDFIndex = 0xFFFFFFFFu;
    unsigned int importanceWidth = 0;
    unsigned int importanceHeight = 0;
    float importanceTotalWeight = 0.0f;
    unsigned int importancePadding[3]{};
};

// RTXPT.1 reference path tracer, root CBV b0 of PathTraceReference.hlsl. Layout matches the
// PathTraceConstants cbuffer field for field (16-byte rows).
struct PathTraceReferenceConstants
{
    Matrix4 inverseViewProjection;

    std::uint32_t resolution[2]{};
    std::uint32_t accumulatedSamples = 0;
    std::uint32_t samplesPerFrame = 1;

    std::uint32_t maxPathDepth = 1;
    std::uint32_t russianRouletteDepth = 1;
    std::uint32_t instanceDataIndex = 0;
    std::uint32_t materialBufferIndex = 0;

    std::uint32_t environmentTextureIndex = 0;
    std::uint32_t environmentConditionalCDFIndex = 0xFFFFFFFFu;
    std::uint32_t environmentMarginalCDFIndex = 0xFFFFFFFFu;
    std::uint32_t environmentWidth = 0;

    std::uint32_t environmentHeight = 0;
    float environmentTotalWeight = 0.0f;
    // RTXPT.1A.2 bundled GGX single-scatter complement tables (MaterialTableResources SRVs).
    std::uint32_t materialTableIndex = 0;
    std::uint32_t materialAverageTableIndex = 0;

    float sunDirectionToLight[3]{0.0f, 1.0f, 0.0f};
    float sunCosAngularRadius = 1.0f;

    float sunIrradiance[3]{};
    std::uint32_t randomSeed = 0;

    std::uint32_t materialTableCosineMapping = 0;
    // RTXPT D.1B.2 signal extraction (former padding): raw UAV indices read only by the extraction ray generations.
    std::uint32_t diffuseOutputIndex = 0;
    std::uint32_t specularOutputIndex = 0;
    std::uint32_t splitDirectDiffuseIndex = 0;

    // RTXPT.2 R3, appended after the RTXPT.1 layout: the raster-primary G-buffer reconstruction
    // (PathTracePrimarySurface.hlsli). The reference entry point does not read them.
    Matrix4 inverseCameraRelativeViewProjection;

    float cameraPosition[3]{};
    std::uint32_t gBufferDepthIndex = 0;

    std::uint32_t gBufferMaterialIndex = 0;
    std::uint32_t gBufferSurfaceIndex = 0;
    std::uint32_t gBufferGeometricNormalIndex = 0;
    // RTXPT.3 ReSTIR DI: the per-sample vertex-0 direct radiance raw UAV. Read only by the
    // split direct and indirect entry points.
    std::uint32_t splitDirectRadianceIndex = 0;

    // NRD.RELAX.2, appended: the raw UAV of the R32F raw primary diffuse hit distance the extraction ray generations
    // write when requested (PathTraceReferencePass::EnsureRawDiffuseHitDistance); UINT32_MAX writes nothing.
    std::uint32_t rawDiffuseHitDistanceIndex = 0xFFFFFFFFu;
    // Reference-only optional raw UAV; reuses padding, preserving b0 size/prefix.
    std::uint32_t referenceRayCountIndex = 0xFFFFFFFFu;
    // PSR.1a, in the first padding word: the raw UAV of the replacement records, read only by the PSR libraries
    // (PathTraceReferencePsr*.hlsl); every other library still declares the word as padding.
    std::uint32_t psrRecordIndex = 0;
    std::uint32_t rawDiffuseHitDistancePadding = 0;
};

static_assert(sizeof(PathTraceReferenceConstants) == 64 + 7 * 16 + 64 + 3 * 16);
static_assert(offsetof(PathTraceReferenceConstants, sunDirectionToLight) == 64 + 4 * 16);
static_assert(offsetof(PathTraceReferenceConstants, inverseCameraRelativeViewProjection) == 64 + 7 * 16);
static_assert(offsetof(PathTraceReferenceConstants, rawDiffuseHitDistanceIndex) == 272);
static_assert(offsetof(PathTraceReferenceConstants, referenceRayCountIndex) == 276);
static_assert(offsetof(PathTraceReferenceConstants, psrRecordIndex) == 280);

// PSR.1b: b3 of the PSR guide pass (PathTracePsrGuideConstants, PathTraceReference.hlsl).
struct PathTracePsrGuideConstants
{
    Matrix4 viewProjection;
    Matrix4 previousViewProjection;
    float viewDepthRow[4]{};
    float previousViewDepthRow[4]{};
    std::uint32_t guideIndex = 0;
    std::uint32_t historyValid = 0;
    std::uint32_t transformCount = 0;
    std::uint32_t identityIndex = 0;
};

static_assert(sizeof(PathTracePsrGuideConstants) == 2 * 64 + 3 * 16);

// PSR.2 Stage 3A: b4 of the PSR.2 lighting pair (PathTracePsrGBufferConstants, PathTraceReference.hlsl): the replacement
// pass's camera (its PsrInPlaceReplacePass::Constants values) and identity target, and its transform count.
struct PathTracePsrGBufferConstants
{
    Matrix4 cameraRelativeViewProjection;
    Matrix4 inverseCameraRelativeViewProjection;
    float cameraPosition[3]{};
    std::uint32_t identityIndex = 0;
    std::uint32_t transformCount = 0;
    // [0] the verification probe's UAV; [1] the geometry pass's texture mip bias as a float's bits (SetTextureMipBias).
    std::uint32_t padding[3]{};

    void SetTextureMipBias(float bias)
    {
        std::memcpy(&padding[1], &bias, sizeof(bias));
    }
};

static_assert(sizeof(PathTracePsrGBufferConstants) == 2 * 64 + 2 * 16);

// PSR.2 Stage 3B: the denoisers' PSR variants' constants (PsrDenoiseConstantsData, PsrDenoise.hlsli): the replacement
// pass's camera and identity target, and the path tracer's instance data, materials and material tables.
struct PsrDenoiseConstants
{
    Matrix4 cameraRelativeViewProjection;
    Matrix4 inverseCameraRelativeViewProjection;
    float cameraPosition[3]{};
    std::uint32_t identityIndex = 0;
    std::uint32_t transformCount = 0;
    std::uint32_t instanceDataIndex = 0;
    std::uint32_t materialBufferIndex = 0;
    std::uint32_t materialTableIndex = 0;
    std::uint32_t materialAverageTableIndex = 0;
    std::uint32_t materialTableCosineMapping = 0;
    std::uint32_t resolution[2]{};
    // The geometry pass's texture mip bias (PsrRecoverMirror).
    float textureMipBias = 0.0f;
    std::uint32_t padding[3]{};
};

static_assert(sizeof(PsrDenoiseConstants) == 2 * 64 + 4 * 16);

// PSR.1b: one render object's (TLAS InstanceID's) current-to-previous world transform, rows of a 3x4 matrix
// (PathTracePsrTransform).
struct PathTracePsrTransform
{
    float rows[3][4]{};
    std::uint32_t valid = 0;
    std::uint32_t padding[3]{};
};

static_assert(sizeof(PathTracePsrTransform) == 64);

struct AOConstants
{
    Matrix4 inverseViewProjection;

    unsigned int depthIndex = 0;
    unsigned int surfaceIndex = 0;
    unsigned int frameIndex = 0;
    unsigned int padding0 = 0;

    float maxDistance = 1.5f;
    float originBias = 0.01f;
    float rayTMin = 0.001f;
    float padding1 = 0.0f;
};

struct DirectionalLightConstants
{
    float direction[3];
    float intensity;

    float color[3];
    float padding;
};

// LIGHT.1 deferred light record; identical layout to GPULight in DeferredLighting.hlsl.
// type: 0 Point, 1 Spot, 2 Directional (LightType). direction is the direction light travels.
// radiance is the light's value in GPU render units, built by BuildFrameLighting (PhotometricUnits.h): luminous
// intensity (Point/Spot, cd) or illuminance (Directional, lx) times the unit-luminance tint and the scene
// pre-scale. Cone cosines are used by Spot only. |range| is the Point/Spot influence radius (also the culling
// bound); its sign is the attenuation mode: positive fades to zero approaching it, negative cuts off hard there.
struct GPULight
{
    float position[3];
    std::uint32_t type;

    float direction[3];
    float range;

    float radiance[3];
    float cosOuterCone;

    float cosInnerCone;
    float padding[3];

    bool operator==(const GPULight&) const = default;
};

// Root constant buffer b3 of deferred lighting and b2 of RTGI. Only lightCount records are
// read; the table is never partially filled with stale values.
struct SceneLightConstants
{
    static constexpr std::uint32_t Capacity = 64;

    std::uint32_t lightCount;
    std::uint32_t padding[3];

    GPULight lights[Capacity];
};

struct GPUMaterial
{
    unsigned int model;
    float roughness;
    float ior;
    float metallic;

    float baseColor[3];
    unsigned int textureIndex;

    float conductorN[3];
    unsigned int roughnessTextureIndex;

    float conductorK[3];
    unsigned int metallicTextureIndex;

    float averageConductorFresnel[3];
    unsigned int normalTextureIndex;
    unsigned int armTextureIndex;
    unsigned int preferARM;
    uint32_t aoTextureIndex;

    uint32_t surfaceDetailMode;
    uint32_t heightTextureIndex;
    float heightScale;
    float heightBias;
};

struct RayTracingInstanceData
{
    unsigned int vertexBufferIndex;
    unsigned int indexBufferIndex;
    unsigned int materialIndex;
    unsigned int vertexStride;

    unsigned int normalOffset;
    unsigned int uvOffset;
    unsigned int indexed;
    unsigned int tangentOffset;

    unsigned int triangleSlotBufferIndex;
    unsigned int triangleSlotOffset;
    unsigned int slotMaterialBufferIndex;
    unsigned int slotMaterialOffset;
};

static_assert(sizeof(MaterialConstants) == 32);
static_assert(sizeof(Matrix4) == 64);
static_assert(sizeof(CameraConstants) == 416);
static_assert(sizeof(ObjectConstants) == 192);
static_assert(sizeof(ReconstructionConstants) == 96);
static_assert(offsetof(ReconstructionConstants, inverseViewProjection) == 0);
static_assert(offsetof(ReconstructionConstants, cameraPosition) == 64);
static_assert(offsetof(ReconstructionConstants, geometricNormalIndex) == 76);
static_assert(offsetof(ReconstructionConstants, motionIndex) == 80);
static_assert(offsetof(ReconstructionConstants, padding) == 84);
static_assert(sizeof(GPUMaterial) == 108);
static_assert(offsetof(GPUMaterial, model) == 0);
static_assert(offsetof(GPUMaterial, roughness) == 4);
static_assert(offsetof(GPUMaterial, ior) == 8);
static_assert(offsetof(GPUMaterial, metallic) == 12);
static_assert(offsetof(GPUMaterial, baseColor) == 16);
static_assert(offsetof(GPUMaterial, textureIndex) == 28);
static_assert(offsetof(GPUMaterial, conductorN) == 32);
static_assert(offsetof(GPUMaterial, roughnessTextureIndex) == 44);
static_assert(offsetof(GPUMaterial, conductorK) == 48);
static_assert(offsetof(GPUMaterial, metallicTextureIndex) == 60);
static_assert(offsetof(GPUMaterial, averageConductorFresnel) == 64);
static_assert(offsetof(GPUMaterial, normalTextureIndex) == 76);
static_assert(offsetof(GPUMaterial, armTextureIndex) == 80);
static_assert(offsetof(GPUMaterial, preferARM) == 84);
static_assert(offsetof(GPUMaterial, aoTextureIndex) == 88);
static_assert(offsetof(GPUMaterial, surfaceDetailMode) == 92);
static_assert(offsetof(GPUMaterial, heightTextureIndex) == 96);
static_assert(offsetof(GPUMaterial, heightScale) == 100);
static_assert(offsetof(GPUMaterial, heightBias) == 104);
static_assert(sizeof(ShadowConstants) == 256);
static_assert(sizeof(GPULight) == 64);
static_assert(offsetof(GPULight, direction) == 16);
static_assert(offsetof(GPULight, radiance) == 32);
static_assert(offsetof(GPULight, cosInnerCone) == 48);
static_assert(sizeof(SceneLightConstants) == 16 + 64 * SceneLightConstants::Capacity);
static_assert(offsetof(SceneLightConstants, lights) == 16);
static_assert(sizeof(ReflectionConstants) == 160);
static_assert(sizeof(RayTracingInstanceData) == 48);
static_assert(offsetof(RayTracingInstanceData, triangleSlotBufferIndex) == 32);
static_assert(offsetof(RayTracingInstanceData, slotMaterialBufferIndex) == 40);
static_assert(offsetof(GIConstants, importanceConditionalCDFIndex) == 112);
static_assert(sizeof(GIConstants) == 144);
static_assert(sizeof(AOConstants) == 96);
