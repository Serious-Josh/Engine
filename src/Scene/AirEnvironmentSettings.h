#pragma once

// AIR.4F F4.5.1b: the scene's air environment, authored in the Air window and stored in the scene (SceneDocument::airEnvironment). The
// air field stores each piece's temperature as ΔT, kelvin above (or below) the ambient here, so changing the ambient keeps every ΔT:
// absolute air temperatures shift with it. A scene without the block uses these defaults. See Documentation/Air/AirArchitecture.md,
// AIR.4F F4.5.1b.

#include <cmath>
#include <string>

struct AirEnvironmentSettings
{
    // The ambient's range: -50 to 100 degrees Celsius, so the lowest ΔT the field holds (-200 K) leaves a positive absolute temperature.
    static constexpr float MinimumAmbientTemperature = 223.15f;
    static constexpr float MaximumAmbientTemperature = 373.15f;
    // The relaxation time's limit; 0 turns relaxation off.
    static constexpr float MaximumRelaxationTime = 3600.0f;

    // Kelvin: 20 degrees Celsius.
    float ambientTemperature = 293.15f;
    // Seconds: the exponential time constant of ΔT's return to the ambient (ΔT x exp(-t / time)); 0: no relaxation.
    float relaxationTime = 30.0f;

    bool operator==(const AirEnvironmentSettings&) const = default;
};

// Every value finite and inside its range. Unlike the mist's appearance, invalid environment values are refused, never clamped.
inline bool ValidateAirEnvironmentSettings(const AirEnvironmentSettings& settings, std::string* error = nullptr)
{
    const auto fail = [&](const char* message)
    {
        if(error != nullptr)
        {
            *error = message;
        }

        return false;
    };

    if(!std::isfinite(settings.ambientTemperature) || settings.ambientTemperature < AirEnvironmentSettings::MinimumAmbientTemperature ||
        settings.ambientTemperature > AirEnvironmentSettings::MaximumAmbientTemperature)
    {
        return fail("Ambient temperature must be finite, from 223.15 K to 373.15 K (-50 to 100 degrees Celsius).");
    }

    if(!std::isfinite(settings.relaxationTime) || settings.relaxationTime < 0.0f ||
        settings.relaxationTime > AirEnvironmentSettings::MaximumRelaxationTime)
    {
        return fail("Thermal relaxation time must be finite, from 0 (off) to 3600 s.");
    }

    return true;
}
