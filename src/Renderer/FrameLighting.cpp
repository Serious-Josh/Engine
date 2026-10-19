#include "Renderer/FrameLighting.h"

#include <cmath>

namespace
{
    bool IsFinite(const Vector3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool Fail(std::string* error, const char* message)
    {
        if(error)
        {
            *error = message;
        }

        return false;
    }

    // Packs one resolved Light. Radiance is color * intensity, applied once on the GPU.
    bool PackLight(const ResolvedSceneLight& light, GPULight& record)
    {
        const Vector3 radiance = light.color * light.intensity;
        const float cosOuter = std::cos(light.outerConeAngle);
        const float cosInner = std::cos(light.innerConeAngle);
        const bool finite = IsFinite(light.position) && IsFinite(light.direction) && IsFinite(radiance) &&
            std::isfinite(light.range) && std::isfinite(cosOuter) && std::isfinite(cosInner);

        if(!finite || !(light.direction.Length() > 0.5f))
        {
            return false;
        }

        record = {};
        record.position[0] = light.position.x;
        record.position[1] = light.position.y;
        record.position[2] = light.position.z;
        record.type = static_cast<std::uint32_t>(light.type);
        record.direction[0] = light.direction.x;
        record.direction[1] = light.direction.y;
        record.direction[2] = light.direction.z;
        record.range = light.range;
        record.radiance[0] = radiance.x;
        record.radiance[1] = radiance.y;
        record.radiance[2] = radiance.z;
        record.cosOuterCone = cosOuter;
        record.cosInnerCone = cosInner;
        return true;
    }
}

bool BuildFrameLighting(const DirectionalLight& legacySun, const SceneLightingSnapshot* snapshot,
    FrameLighting& lighting, std::string* error)
{
    FrameLighting candidate;
    candidate.sun = legacySun;
    candidate.table.lightCount = 0;

    if(!snapshot)
    {
        lighting = candidate;
        return true;
    }

    if(snapshot->additionalLights.size() > SceneLightConstants::Capacity)
    {
        return Fail(error, "Scene lighting exceeds the GPU light table capacity.");
    }

    if(snapshot->suppressLegacyDirectional)
    {
        // Never both: once Directional Light components exist the legacy sun contributes nothing.
        candidate.sun.color = {0.0f, 0.0f, 0.0f};
        candidate.sun.intensity = 0.0f;
        candidate.sun.direction = {0.0f, -1.0f, 0.0f};

        if(snapshot->sunDirectional)
        {
            const ResolvedSceneLight& owner = *snapshot->sunDirectional;

            if(!IsFinite(owner.direction) || !IsFinite(owner.color) || !std::isfinite(owner.intensity))
            {
                return Fail(error, "The shadow-owner Directional Light is not finite.");
            }

            candidate.sun.direction = owner.direction;
            candidate.sun.color = owner.color;
            candidate.sun.intensity = owner.intensity;
        }
    }

    for(const ResolvedSceneLight& light : snapshot->additionalLights)
    {
        if(!PackLight(light, candidate.table.lights[candidate.table.lightCount]))
        {
            return Fail(error, "A scene Light is not finite; lighting was not published.");
        }

        ++candidate.table.lightCount;
    }

    lighting = candidate;
    return true;
}

bool SameLocalLightInputs(const SceneLightConstants& left, const SceneLightConstants& right)
{
    std::uint32_t leftIndex = 0;
    std::uint32_t rightIndex = 0;
    const auto skipDirectionals = [](const SceneLightConstants& table, std::uint32_t& index)
    {
        while(index < table.lightCount &&
            table.lights[index].type == static_cast<std::uint32_t>(LightType::Directional))
        {
            ++index;
        }
    };

    for(;;)
    {
        skipDirectionals(left, leftIndex);
        skipDirectionals(right, rightIndex);

        if(leftIndex == left.lightCount || rightIndex == right.lightCount)
        {
            return leftIndex == left.lightCount && rightIndex == right.lightCount;
        }

        if(!(left.lights[leftIndex] == right.lights[rightIndex]))
        {
            return false;
        }

        ++leftIndex;
        ++rightIndex;
    }
}
