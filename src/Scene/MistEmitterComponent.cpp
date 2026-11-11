#include "Scene/MistEmitterComponent.h"

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

    return true;
}
