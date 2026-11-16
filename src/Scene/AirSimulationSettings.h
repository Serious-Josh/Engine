#pragma once

// R1.1: the scene's AIR simulation configuration, stored in the scene (SceneDocument::airSimulation), cooked, and resolved by the Engine
// for every frame through ResolveAirSimulation (SceneDocument.h). Editor-independent: the packaged runtime reads the same settings. A
// scene without the block uses these defaults, which are the Editor's earlier session defaults (AIR off). Viewing, diagnostics, pause
// and rendering quality are not simulation configuration and stay Editor session state. See Documentation/Runtime/
// RuntimeAirConfigurationR1_1.md.

#include "Math/Vector3.h"

#include <cmath>
#include <string>

struct AirSimulationSettings
{
    // AIR.4F.1c: the default damping, and the preset for buoyant scenes (default damping slows convection by about a third and delays
    // a cold plume's arrival at the floor from about 10 s to 20 s; Documentation/Air/AirBuoyancyImplementation4F1b.md, section 5).
    static constexpr float DefaultDampingRate = 0.5f;
    static constexpr float ConvectionDampingRate = 0.1f;

    // The ranges the Air window has always offered; values outside them are refused, never clamped.
    static constexpr float MaximumAmbientSpeed = 50.0f;
    static constexpr float MaximumDampingRate = 20.0f;
    static constexpr float MaximumMotorSpeedLimit = 200.0f;
    static constexpr float MaximumMistDissipationRate = 10.0f;
    static constexpr float MaximumBuoyancyGravity = 30.0f;

    bool enabled = false;
    // AIR.4E.4 (experimental): the pressure projection after each air step.
    bool pressureProjection = false;
    // AIR.4F.1b (experimental): temperature-driven buoyancy, applied by the pressure projection, so in effect only while it is on.
    // Gravity is its magnitude in m/s^2, along -y.
    bool buoyancy = false;
    float buoyancyGravity = 9.81f;
    // AIR.4E.5b.1: the most fixed air steps one frame may run (1 or 2).
    int maximumStepsPerFrame = 2;
    // m/s; each component within +/- MaximumAmbientSpeed.
    Vector3 ambientVelocity{0.0f, 0.0f, 0.0f};
    // 1/s: air relaxes to the ambient wind by exp(-rate * t).
    float dampingRate = DefaultDampingRate;
    float maximumMotorSpeed = 40.0f;
    // 1/s: mist fades by exp(-rate * t).
    float mistDissipationRate = 0.25f;
    // R1.5: moving-solid displacement. On, the overlay carves the participating bodies (MovingSolidParticipation.h) out of the air
    // and the pressure solver takes its warm path (local rebuilds, bounded recovery), which is validated only with the overlay. Off by
    // default and for every scene saved before schema 27 (payload 37), so existing scenes keep their pressure behaviour.
    bool movingSolids = false;

    bool operator==(const AirSimulationSettings& other) const
    {
        return enabled == other.enabled && pressureProjection == other.pressureProjection && buoyancy == other.buoyancy &&
            movingSolids == other.movingSolids &&
            buoyancyGravity == other.buoyancyGravity && maximumStepsPerFrame == other.maximumStepsPerFrame &&
            ambientVelocity.x == other.ambientVelocity.x && ambientVelocity.y == other.ambientVelocity.y &&
            ambientVelocity.z == other.ambientVelocity.z && dampingRate == other.dampingRate &&
            maximumMotorSpeed == other.maximumMotorSpeed && mistDissipationRate == other.mistDissipationRate;
    }
};

// Every value finite and inside its range, as ValidateAirEnvironmentSettings: invalid values are refused, never clamped.
inline bool ValidateAirSimulationSettings(const AirSimulationSettings& settings, std::string* error = nullptr)
{
    const auto fail = [&](const char* message)
    {
        if(error != nullptr)
        {
            *error = message;
        }

        return false;
    };
    const auto inRange = [](float value, float minimum, float maximum)
    {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    };

    if(settings.maximumStepsPerFrame != 1 && settings.maximumStepsPerFrame != 2)
    {
        return fail("Maximum AIR steps per frame must be 1 or 2.");
    }

    const float ambientLimit = AirSimulationSettings::MaximumAmbientSpeed;

    if(!inRange(settings.ambientVelocity.x, -ambientLimit, ambientLimit) || !inRange(settings.ambientVelocity.y, -ambientLimit, ambientLimit) ||
        !inRange(settings.ambientVelocity.z, -ambientLimit, ambientLimit))
    {
        return fail("Each ambient wind component must be finite, from -50 to 50 m/s.");
    }

    if(!inRange(settings.dampingRate, 0.0f, AirSimulationSettings::MaximumDampingRate))
    {
        return fail("Damping must be finite, from 0 to 20 per second.");
    }

    if(!inRange(settings.maximumMotorSpeed, 0.0f, AirSimulationSettings::MaximumMotorSpeedLimit))
    {
        return fail("The motor speed cap must be finite, from 0 to 200 m/s.");
    }

    if(!inRange(settings.mistDissipationRate, 0.0f, AirSimulationSettings::MaximumMistDissipationRate))
    {
        return fail("Mist dissipation must be finite, from 0 to 10 per second.");
    }

    if(!inRange(settings.buoyancyGravity, 0.0f, AirSimulationSettings::MaximumBuoyancyGravity))
    {
        return fail("Buoyancy gravity must be finite, from 0 to 30 m/s^2.");
    }

    return true;
}
