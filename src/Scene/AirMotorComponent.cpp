#include "Scene/AirMotorComponent.h"
#include "Core/Log.h"

#include <cmath>
#include <string>

namespace
{
bool Fail(std::string* error, const char* message)
{
    if(error)
    {
        *error = message;
    }

    return false;
}

bool InRange(float value, float minimum, float maximum)
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}
}

const char* AirMotorTypeName(AirMotorType type) noexcept
{
    switch(type)
    {
    case AirMotorType::Directional:
        return "Directional";
    case AirMotorType::Omni:
        return "Omni";
    case AirMotorType::Vortex:
        return "Vortex";
    case AirMotorType::Moving:
        return "Moving";
    case AirMotorType::Cylinder:
        return "Cylinder";
    }

    return nullptr;
}

bool TryParseAirMotorType(std::string_view text, AirMotorType& type) noexcept
{
    for(const AirMotorType candidate : {AirMotorType::Directional, AirMotorType::Omni, AirMotorType::Vortex, AirMotorType::Moving,
        AirMotorType::Cylinder})
    {
        if(text == AirMotorTypeName(candidate))
        {
            type = candidate;
            return true;
        }
    }

    return false;
}

std::uint64_t AirMotorTag(std::string_view objectID) noexcept
{
    // FNV-1a over the object's ID.
    std::uint64_t hash = 14695981039346656037ull;

    for(const char character : objectID)
    {
        hash = (hash ^ static_cast<unsigned char>(character)) * 1099511628211ull;
    }

    return hash == 0 ? 1 : hash;
}

bool ValidateAirMotorComponent(const AirMotorComponent& motor, std::string* error)
{
    using Limits = AirMotorComponent;

    if(AirMotorTypeName(motor.type) == nullptr)
    {
        return Fail(error, "Air Motor type must be Directional, Omni, Vortex, Moving or Cylinder.");
    }

    if(!InRange(motor.strength, -Limits::MaximumStrength, Limits::MaximumStrength))
    {
        return Fail(error, "Air Motor strength must be finite and within +/-1000 m/s^2.");
    }

    if(!InRange(motor.falloff, 0.0f, 1.0f))
    {
        return Fail(error, "Air Motor falloff must be between 0 and 1.");
    }

    const bool sizes = InRange(motor.halfExtents.x, Limits::MinimumSize, Limits::MaximumSize) &&
        InRange(motor.halfExtents.y, Limits::MinimumSize, Limits::MaximumSize) &&
        InRange(motor.halfExtents.z, Limits::MinimumSize, Limits::MaximumSize) && InRange(motor.radius, Limits::MinimumSize, Limits::MaximumSize) &&
        InRange(motor.length, Limits::MinimumSize, Limits::MaximumSize * 2.0f);

    if(!sizes)
    {
        return Fail(error, "Air Motor sizes must be finite, from 0.05 m to 256 m (length to 512 m).");
    }

    if(!InRange(motor.inwardPull, -Limits::MaximumInwardPull, Limits::MaximumInwardPull))
    {
        return Fail(error, "Air Motor inward pull must be finite and within +/-1000 m/s^2.");
    }

    if(!InRange(motor.coupling, 0.0f, Limits::MaximumCoupling))
    {
        return Fail(error, "Air Motor coupling must be between 0 and 60 per second.");
    }

    const bool origin = InRange(motor.emissionOrigin.x, -Limits::MaximumEmissionOffset, Limits::MaximumEmissionOffset) &&
        InRange(motor.emissionOrigin.y, -Limits::MaximumEmissionOffset, Limits::MaximumEmissionOffset) &&
        InRange(motor.emissionOrigin.z, -Limits::MaximumEmissionOffset, Limits::MaximumEmissionOffset);

    if(!origin)
    {
        return Fail(error, "Air Motor emission origin must be finite and within +/-256 m on each axis.");
    }

    const Vector3& semiAxes = motor.emissionSemiAxes;
    const bool semiAxesInRange = InRange(semiAxes.x, 0.0f, Limits::MaximumSize) && InRange(semiAxes.y, 0.0f, Limits::MaximumSize) &&
        InRange(semiAxes.z, 0.0f, Limits::MaximumSize);

    if(!semiAxesInRange)
    {
        return Fail(error, "Air Motor emission semi-axes must be finite, from 0 to 256 m.");
    }

    const Vector3& rotation = motor.emissionRotation;
    const bool rotationInRange = InRange(rotation.x, -Limits::MaximumEmissionRotation, Limits::MaximumEmissionRotation) &&
        InRange(rotation.y, -Limits::MaximumEmissionRotation, Limits::MaximumEmissionRotation) &&
        InRange(rotation.z, -Limits::MaximumEmissionRotation, Limits::MaximumEmissionRotation);

    if(!rotationInRange)
    {
        return Fail(error, "Air Motor emission rotation must be finite and within +/-360 degrees on each axis.");
    }

    // The emitter's shape must be one the solver can sample: an ellipsoid (or exactly a point) for an Omni, an ellipse for a Vortex.
    // Semi-axes below the minimum size would make the surface normal or ring tangent ill-conditioned.
    if(motor.type == AirMotorType::Omni && !IsPointOmni(motor))
    {
        const bool ellipsoid = semiAxes.x >= Limits::MinimumSize && semiAxes.y >= Limits::MinimumSize && semiAxes.z >= Limits::MinimumSize;

        if(!ellipsoid)
        {
            return Fail(error, "An Omni emission ellipsoid needs every semi-axis from 0.05 m to 256 m, or all three 0 for a point Omni.");
        }
    }

    if(motor.type == AirMotorType::Vortex && (semiAxes.x < Limits::MinimumSize || semiAxes.y < Limits::MinimumSize))
    {
        return Fail(error, "A Vortex emission ring needs semi-axes X and Y from 0.05 m to 256 m.");
    }

    return true;
}

bool IsPointOmni(const AirMotorComponent& motor) noexcept
{
    return motor.type == AirMotorType::Omni && motor.emissionSemiAxes.x == 0.0f && motor.emissionSemiAxes.y == 0.0f &&
        motor.emissionSemiAxes.z == 0.0f;
}

bool MigrateLegacyAirMotorEmission(AirMotorComponent& motor, float legacyEmissionRadius)
{
    const AirMotorComponent defaults;
    motor.emissionRotation = defaults.emissionRotation;

    if(motor.type == AirMotorType::Omni)
    {
        motor.emissionSemiAxes = {0.0f, 0.0f, 0.0f};

        // A point Omni is exactly the new point Omni; an emission ball has no surface equivalent.
        if(legacyEmissionRadius == 0.0f)
        {
            return false;
        }

        motor.enabled = false;
        return true;
    }

    motor.emissionSemiAxes = defaults.emissionSemiAxes;

    if(motor.type == AirMotorType::Vortex)
    {
        // The old emission cylinder has no ring equivalent; the default ring is a placeholder, never enabled automatically.
        motor.enabled = false;
        return true;
    }

    return false;
}

void LogAirMotorReconfiguration(std::string_view objectName, std::string_view objectID, AirMotorType type, std::string_view source)
{
    const std::string shape = type == AirMotorType::Vortex ? "emission cylinder has no ring equivalent" :
        "emission ball has no surface equivalent";
    Log::Warning("Air Motor on object '" + std::string(objectName) + "' (" + std::string(objectID) + ", " + std::string(source) + "): its " +
        AirMotorTypeName(type) + " " + shape + " in the AIR.4D D2.1 emission model, so it was disabled. Set its emission semi-axes, "
        "rotation and influence distance by hand, then enable it; it stays disabled until then, including after saving.");
}
