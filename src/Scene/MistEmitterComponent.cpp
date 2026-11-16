#include "Scene/MistEmitterComponent.h"

#include <algorithm>
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

bool VectorInRange(const Vector3& value, float minimum, float maximum)
{
    return InRange(value.x, minimum, maximum) && InRange(value.y, minimum, maximum) && InRange(value.z, minimum, maximum);
}
}

const char* MistEmitterShapeName(MistEmitterShape shape) noexcept
{
    switch(shape)
    {
    case MistEmitterShape::Sphere:
        return "Sphere";
    case MistEmitterShape::Box:
        return "Box";
    }

    return nullptr;
}

bool TryParseMistEmitterShape(std::string_view text, MistEmitterShape& shape) noexcept
{
    for(const MistEmitterShape candidate : {MistEmitterShape::Sphere, MistEmitterShape::Box})
    {
        if(text == MistEmitterShapeName(candidate))
        {
            shape = candidate;
            return true;
        }
    }

    return false;
}

const char* MistEmitterModeName(MistEmitterMode mode) noexcept
{
    switch(mode)
    {
    case MistEmitterMode::Legacy:
        return "Legacy";
    case MistEmitterMode::Point:
        return "Point";
    case MistEmitterMode::Volume:
        return "Volume";
    }

    return nullptr;
}

bool TryParseMistEmitterMode(std::string_view text, MistEmitterMode& mode) noexcept
{
    for(const MistEmitterMode candidate : {MistEmitterMode::Legacy, MistEmitterMode::Point, MistEmitterMode::Volume})
    {
        if(text == MistEmitterModeName(candidate))
        {
            mode = candidate;
            return true;
        }
    }

    return false;
}

float MistEmitterPointFootprint(const MistEmitterComponent& emitter) noexcept
{
    return (std::max)(emitter.radius, MistEmitterComponent::MinimumPointFootprint);
}

float MistEmitterJetReach(const MistEmitterComponent& emitter) noexcept
{
    return (std::max)(MistEmitterPointFootprint(emitter), MistEmitterComponent::JetMinimumReach);
}

bool MistEmitterJetNeedsProjectionWarning(const MistEmitterComponent& emitter, bool pressureProjection) noexcept
{
    return !pressureProjection && emitter.enabled && emitter.mode == MistEmitterMode::Point && emitter.jetSpeed > 0.0f;
}

std::uint64_t MistEmitterTag(std::string_view objectID) noexcept
{
    // FNV-1a over a prefix and the object's ID, so an object's emitter and motor never share a tag.
    std::uint64_t hash = 14695981039346656037ull;

    for(const char character : std::string_view("mist-emitter:"))
    {
        hash = (hash ^ static_cast<unsigned char>(character)) * 1099511628211ull;
    }

    for(const char character : objectID)
    {
        hash = (hash ^ static_cast<unsigned char>(character)) * 1099511628211ull;
    }

    return hash == 0 ? 1 : hash;
}

bool ValidateMistEmitterComponent(const MistEmitterComponent& emitter, std::string* error)
{
    using Limits = MistEmitterComponent;

    if(MistEmitterShapeName(emitter.shape) == nullptr)
    {
        return Fail(error, "Mist Emitter shape must be Sphere or Box.");
    }

    if(!VectorInRange(emitter.emissionOrigin, -Limits::MaximumEmissionOffset, Limits::MaximumEmissionOffset))
    {
        return Fail(error, "Mist Emitter emission origin must be finite and within +/-256 m on each axis.");
    }

    if(!VectorInRange(emitter.emissionRotation, -Limits::MaximumEmissionRotation, Limits::MaximumEmissionRotation))
    {
        return Fail(error, "Mist Emitter emission rotation must be finite and within +/-360 degrees on each axis.");
    }

    if(!InRange(emitter.radius, Limits::MinimumSize, Limits::MaximumSize) ||
        !VectorInRange(emitter.halfExtents, Limits::MinimumSize, Limits::MaximumSize))
    {
        return Fail(error, "Mist Emitter radius and half extents must be finite, from 0.05 m to 256 m.");
    }

    if(!InRange(emitter.rate, 0.0f, Limits::MaximumRate))
    {
        return Fail(error, "Mist Emitter rate must be finite, from 0 to 1000 per second.");
    }

    if(!VectorInRange(emitter.color, 0.0f, 1.0f))
    {
        return Fail(error, "Mist Emitter colour components must be from 0 to 1.");
    }

    if(!InRange(emitter.density, 0.0f, Limits::MaximumDensity))
    {
        return Fail(error, "Mist Emitter density must be finite, from 0 to 100.");
    }

    if(!InRange(emitter.temperature, Limits::MinimumTemperature, Limits::MaximumTemperature))
    {
        return Fail(error, "Mist Emitter temperature must be finite, from -200 K to +1000 K relative to the ambient.");
    }

    if(MistEmitterModeName(emitter.mode) == nullptr)
    {
        return Fail(error, "Mist Emitter mode must be Legacy, Point or Volume.");
    }

    if(!InRange(emitter.emissionRate, 0.0f, Limits::MaximumEmissionRate))
    {
        return Fail(error, "Mist Emitter emission rate must be finite, from 0 to 1000 density x m^3 per second.");
    }

    if(!InRange(emitter.jetSpeed, 0.0f, Limits::MaximumJetSpeed))
    {
        return Fail(error, "Mist Emitter jet speed must be finite, from 0 to 30 m/s.");
    }

    if(!InRange(emitter.temperatureResponseRate, Limits::MinimumTemperatureResponseRate, Limits::MaximumTemperatureResponseRate))
    {
        return Fail(error, "Mist Emitter temperature response rate must be finite, from 0.1 to 120 per second.");
    }

    if(!InRange(emitter.targetDensity, 0.0f, Limits::MaximumTargetDensity) ||
        !InRange(emitter.replenishmentRate, 0.0f, Limits::MaximumReplenishmentRate))
    {
        return Fail(error, "Mist Emitter target density must be from 0 to 100 and replenishment rate from 0 to 60 per second.");
    }

    if(!InRange(emitter.fillHeight, Limits::MinimumFillHeight, Limits::MaximumFillHeight) ||
        !InRange(emitter.edgeSoftness, 0.0f, Limits::MaximumEdgeSoftness))
    {
        return Fail(error, "Mist Emitter fill height must be from 0.05 to 512 m and edge softness from 0 to 64 m.");
    }

    if(!InRange(emitter.volumeTemperatureRate, 0.0f, Limits::MaximumVolumeTemperatureRate))
    {
        return Fail(error, "Mist Emitter volume temperature response must be finite, from 0 to 60 per second.");
    }

    // The Point limit applies only in Point mode, so a large Legacy sphere stays valid; the Inspector reports it on switching.
    if(emitter.mode == MistEmitterMode::Point && emitter.radius > Limits::MaximumPointRadius)
    {
        return Fail(error, "A Point Mist Emitter's radius must be at most 2 m.");
    }

    return true;
}
