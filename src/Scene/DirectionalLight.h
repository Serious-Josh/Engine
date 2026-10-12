#pragma once

#include "Math/Vector3.h"
#include "Math/Math.h"

struct DirectionalLight
{
    Vector3 direction{0.5f, -0.5f, 1.0f};

    float intensity = 1.0f;

    Vector3 color{1.0f, 1.0f, 1.0f};

    float ambientStrength = 0.15f;

    float angularRadius = ToRadians(0.27f);
};
