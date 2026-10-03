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
};

struct ObjectConstants
{
    Matrix4 model;
    Matrix4 normalMatrix;
};

struct ReconstructionConstants
{
    Matrix4 inverseViewProjection;

    float cameraPosition[3];
    float padding;
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

    float conductorN[3];
    float padding0;

    float conductorK[3];
    float padding1;

    float averageConductorFresnel[3];
    float padding2;
};

static_assert(sizeof(MaterialConstants) == 32);
static_assert(sizeof(Matrix4) == 64);
static_assert(sizeof(CameraConstants) == 64);
static_assert(sizeof(ObjectConstants) == 128);
static_assert(sizeof(ReconstructionConstants) == 80);
static_assert(sizeof(GPUMaterial) == 64);