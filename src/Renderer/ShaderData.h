#pragma once

#include "Math/Matrix4.h"

struct MaterialConstants
{
    unsigned int textureIndex;
    float roughness;
    unsigned int materialIndex;
    float padding;

    float baseColor[3];
    float padding2;
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
    float padding;
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
    float padding[3];
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
    unsigned int padding1 = 0;
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
    float padding0;

    float conductorK[3];
    float padding1;

    float averageConductorFresnel[3];
    float padding2;
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
    unsigned int padding;
};

static_assert(sizeof(MaterialConstants) == 32);
static_assert(sizeof(Matrix4) == 64);
static_assert(sizeof(CameraConstants) == 256);
static_assert(sizeof(ObjectConstants) == 192);
static_assert(sizeof(ReconstructionConstants) == 80);
static_assert(sizeof(GPUMaterial) == 80);
static_assert(sizeof(ShadowConstants) == 256);
static_assert(sizeof(ReflectionConstants) == 144);
static_assert(sizeof(RayTracingInstanceData) == 32);
static_assert(sizeof(GIConstants) == 96);