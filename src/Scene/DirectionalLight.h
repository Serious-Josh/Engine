#pragma once

#include "Math/Vector3.h"
#include "Math/Math.h"

// A sun. As authored by a procedural demo scene (Scene::GetDirectionalLight), intensity is illuminance in lux and
// color a [0, 1] tint, like a Directional LightComponent. As FrameLighting::sun it is in GPU render units instead:
// BuildFrameLighting stores the unit-luminance tint and the pre-scaled illuminance (PhotometricUnits.h), so every
// renderer consumer forms color * intensity identically.
struct DirectionalLight
{
    Vector3 direction{0.5f, -0.5f, 1.0f};

    float intensity = 2000.0f;

    Vector3 color{1.0f, 1.0f, 1.0f};

    float angularRadius = ToRadians(0.27f);
};
