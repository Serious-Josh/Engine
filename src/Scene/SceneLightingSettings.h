#pragma once

// Scene-owned ambient settings, independent of the presence or energy of any light.
struct SceneLightingSettings
{
    float ambientStrength = 0.15f;
    bool operator==(const SceneLightingSettings&) const = default;
};
