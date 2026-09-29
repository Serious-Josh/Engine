#pragma once

#include "Math/Matrix4.h"

struct CameraConstants
{
    Matrix4 view;
    Matrix4 projection;
};

struct ObjectConstants
{
    Matrix4 model;
    Matrix4 normalMatrix;
};

struct ReconstructionConstants
{
    Matrix4 inverseViewProjection;
};

static_assert(sizeof(Matrix4) == 64);
static_assert(sizeof(CameraConstants) == 128);
static_assert(sizeof(ObjectConstants) == 128);
static_assert(sizeof(ReconstructionConstants) == 64);