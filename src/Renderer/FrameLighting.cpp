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

    // Packs one resolved Light into GPU render units (PhotometricUnits.h): the unit-luminance tint times the luminous
    // intensity (Point: flux / 4 pi; Spot: flux over its emission solid angle) or illuminance (Directional), times
    // the scene pre-scale. Computed in double; the GPU applies it once.
    bool PackLight(const ResolvedSceneLight& light, double renderScale, GPULight& record)
    {
        double photometric = light.intensity;

        if(light.type == LightType::Point)
        {
            photometric = Photometry::PointIntensity(light.intensity);
        }
        else if(light.type == LightType::Spot)
        {
            photometric = Photometry::SpotAxisIntensity(light.intensity, light.innerConeAngle, light.outerConeAngle);
        }

        const Vector3 tint = Photometry::UnitLuminanceColor(light.color);
        const double scale = photometric * renderScale;
        const Vector3 radiance{float(tint.x * scale), float(tint.y * scale), float(tint.z * scale)};
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
        // The sign carries the attenuation mode (positive: fade to zero at range; negative: hard cutoff at -range).
        record.range = light.rangeAttenuation ? light.range : -light.range;
        record.radiance[0] = radiance.x;
        record.radiance[1] = radiance.y;
        record.radiance[2] = radiance.z;
        record.cosOuterCone = cosOuter;
        record.cosInnerCone = cosInner;
        return true;
    }

    // A sun in GPU render units: unit-luminance tint, pre-scaled illuminance.
    bool ConvertSun(DirectionalLight& sun, double renderScale)
    {
        if(!IsFinite(sun.direction) || !IsFinite(sun.color) || !std::isfinite(sun.intensity) || sun.intensity < 0.0f ||
            !std::isfinite(sun.angularRadius) || sun.angularRadius < 0.0f)
        {
            return false;
        }

        sun.color = Photometry::UnitLuminanceColor(sun.color);
        sun.intensity = float(double(sun.intensity) * renderScale);
        return std::isfinite(sun.intensity);
    }
}

bool BuildFrameLighting(const DirectionalLight& legacySun, const SceneLightingSnapshot* snapshot,
    FrameLighting& lighting, std::string* error, const SceneLightingSettings* proceduralSettings)
{
    FrameLighting candidate;
    candidate.sun = legacySun;
    candidate.table.lightCount = 0;
    const SceneLightingSettings settings = snapshot ? snapshot->lightingSettings :
        (proceduralSettings ? *proceduralSettings : SceneLightingSettings{});

    if(!ValidateSceneLightingSettings(settings))
    {
        return Fail(error, "Scene photometric calibration must be finite and in range.");
    }

    const double renderScale = Photometry::RenderScale(settings.referenceEV100);

    if(!snapshot)
    {
        if(!ConvertSun(candidate.sun, renderScale))
        {
            return Fail(error, "The procedural sun is not finite.");
        }

        lighting = candidate;
        return true;
    }

    if(snapshot->additionalLights.size() > SceneLightConstants::Capacity)
    {
        return Fail(error, "Scene lighting exceeds the GPU light table capacity.");
    }

    // Captured file scenes are authoritative even when empty. Only explicit procedural-demo
    // callers may opt into their compatibility source; file Scene storage is already discarded.
    if(snapshot->suppressLegacyDirectional)
    {
        // A captured scene never uses the procedural-demo compatibility source.
        candidate.sun.angularRadius = 0.0f;
        candidate.sun.color = {0.0f, 0.0f, 0.0f};
        candidate.sun.intensity = 0.0f;
        candidate.sun.direction = {0.0f, -1.0f, 0.0f};

        if(snapshot->sunDirectional)
        {
            const ResolvedSceneLight& owner = *snapshot->sunDirectional;
            candidate.sun.direction = owner.direction;
            candidate.sun.color = owner.color;
            candidate.sun.intensity = owner.intensity;
            candidate.sun.angularRadius = owner.angularRadius;
            candidate.sunObjectID = owner.objectID;
        }
    }

    if(!ConvertSun(candidate.sun, renderScale))
    {
        return Fail(error, "The shadow-owner Directional Light is not finite.");
    }

    for(const ResolvedSceneLight& light : snapshot->additionalLights)
    {
        if(!PackLight(light, renderScale, candidate.table.lights[candidate.table.lightCount]))
        {
            return Fail(error, "A scene Light is not finite; lighting was not published.");
        }

        candidate.tableLightIDs[candidate.table.lightCount] = light.objectID;
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
