#pragma once

// AIR.4F.2d.3: the scene's environmental haze, authored in the Scene Inspector and stored in the scene (SceneDocument::environmentalHaze)
// and in cooked scenes. A uniform atmosphere independent of the AIR simulation: it fills the whole rendered world, beyond the AIR window,
// up to geometry and the sky. Rendered by the fog pass (EditorMist.hlsl) with the scene's mist lighting settings. A scene without the
// block has no haze and no near clearance. See Documentation/Air/AirEnvironmentalHaze4F2d3.md.
//
// Kept apart from MistRenderSettings and AirEnvironmentSettings so it can move to a future World Level unchanged: the renderer reads it
// through the fog request only.

#include <array>
#include <cmath>
#include <string>

struct EnvironmentalHazeSettings
{
    static constexpr float MinimumAttenuationDistance = 1.0f;
    static constexpr float MaximumAttenuationDistance = 100000.0f;
    static constexpr float MaximumNearDistance = 1000.0f;
    static constexpr float MinimumNearTransition = 0.1f;
    static constexpr float MaximumNearTransition = 1000.0f;
    static constexpr float MaximumAmbient = 4.0f;
    static constexpr float MaximumHeight = 100000.0f;
    static constexpr float MaximumHeightTransition = 1000.0f;

    bool enabled = false;
    // Metres: haze alone lets e^(-d / D) of the background through over a distance d (37% at D). Its extinction is 1 / D.
    float attenuationDistance = 100.0f;
    // The haze's scattering colour (its albedo per channel, 0 to 1), multiplied by the scene's scattering tint and strength like mist.
    std::array<float, 3> color{0.9f, 0.9f, 0.9f};
    // Metres: the haze is clear up to this distance from the camera, then reaches full strength over nearTransition metres (a
    // smoothstep in its extinction). 0 turns the clearance off. AIR.4F.2d.3b: the haze's own; the AIR mist has its own clearance
    // (MistRenderSettings::nearDistance). Before 2d.3b one shared clearance served both; loading such a scene gives each a copy.
    float nearDistance = 0.0f;
    float nearTransition = 5.0f;
    // AIR.4F.2d.3a: the environment light the haze scatters, as a fraction of the environment's mean radiance (the sky and HDRI the
    // scene is lit by; 1: the haze sees the whole environment unoccluded, 0: it scatters only the sun and Lights). Above 1 it adds
    // environment light the scene does not otherwise have. Independent of the AIR mist's Ambient. Scenes saved before it read 0.
    float ambient = 1.0f;
    // AIR.4F.2d.3b: optional world-space height bounds (absolute Y, metres). With them, the haze is at full density from baseHeight to
    // maxHeight and fades to nothing over heightTransition below baseHeight and above maxHeight (a smoothstep in its extinction; 0: hard
    // edges). The layer's inside is never thinned, however thin the layer. Off: uniform everywhere, as before.
    bool heightBounds = false;
    float baseHeight = -10.0f;
    float maxHeight = 15.0f;
    float heightTransition = 3.0f;

    bool operator==(const EnvironmentalHazeSettings&) const = default;
};

// Every value finite and inside its range. Like the air environment, invalid values are refused, never clamped.
inline bool ValidateEnvironmentalHazeSettings(const EnvironmentalHazeSettings& settings, std::string* error = nullptr)
{
    const auto fail = [&](const char* message)
    {
        if(error != nullptr)
        {
            *error = message;
        }

        return false;
    };

    const auto within = [](float value, float minimum, float maximum)
    {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    };

    if(!within(settings.attenuationDistance, EnvironmentalHazeSettings::MinimumAttenuationDistance,
        EnvironmentalHazeSettings::MaximumAttenuationDistance))
    {
        return fail("Fog attenuation distance must be finite, from 1 m to 100 km.");
    }

    for(const float channel : settings.color)
    {
        if(!within(channel, 0.0f, 1.0f))
        {
            return fail("Haze colour channels must be from 0 to 1.");
        }
    }

    if(!within(settings.nearDistance, 0.0f, EnvironmentalHazeSettings::MaximumNearDistance) ||
        !within(settings.nearTransition, EnvironmentalHazeSettings::MinimumNearTransition, EnvironmentalHazeSettings::MaximumNearTransition))
    {
        return fail("Near fog distance must be from 0 to 1000 m and its transition from 0.1 to 1000 m.");
    }

    if(!within(settings.ambient, 0.0f, EnvironmentalHazeSettings::MaximumAmbient))
    {
        return fail("Haze ambient contribution must be from 0 to 4.");
    }

    // The heights are checked even while the bounds are off, so turning them on never meets a stored invalid layer.
    const float maximumHeight = EnvironmentalHazeSettings::MaximumHeight;

    if(!within(settings.baseHeight, -maximumHeight, maximumHeight) || !within(settings.maxHeight, -maximumHeight, maximumHeight))
    {
        return fail("Haze base and max heights must be finite, within 100 km of 0.");
    }

    if(!(settings.maxHeight > settings.baseHeight))
    {
        return fail("Haze max height must be above its base height.");
    }

    if(!within(settings.heightTransition, 0.0f, EnvironmentalHazeSettings::MaximumHeightTransition))
    {
        return fail("Haze height transition must be from 0 to 1000 m.");
    }

    return true;
}
