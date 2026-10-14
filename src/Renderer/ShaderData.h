#pragma once

#include "Math/Matrix4.h"
#include <cstddef>
#include <cstdint>

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
    Matrix4 viewProjection;
    Matrix4 previousViewProjection;
    Matrix4 view;
    Matrix4 previousView;
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
};

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
    float ambientStrength;
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
static_assert(sizeof(CameraConstants) == 256);
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
static_assert(sizeof(ReflectionConstants) == 160);
static_assert(sizeof(RayTracingInstanceData) == 48);
static_assert(offsetof(RayTracingInstanceData, triangleSlotBufferIndex) == 32);
static_assert(offsetof(RayTracingInstanceData, slotMaterialBufferIndex) == 40);
static_assert(sizeof(GIConstants) == 112);
static_assert(sizeof(AOConstants) == 96);
