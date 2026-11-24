#include "Renderer/FrameLighting.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

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

    // Many-light M2: the table grows with the scene. The only bound is the 32-bit light index shaders use.
    if(snapshot->additionalLights.size() > UINT32_MAX)
    {
        return Fail(error, "Scene lighting exceeds 32-bit light indexing.");
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

    candidate.table.lights.reserve(snapshot->additionalLights.size());
    candidate.tableLightIDs.reserve(snapshot->additionalLights.size());

    for(const ResolvedSceneLight& light : snapshot->additionalLights)
    {
        GPULight record{};

        if(!PackLight(light, renderScale, record))
        {
            return Fail(error, "A scene Light is not finite; lighting was not published.");
        }

        candidate.table.lights.push_back(record);
        candidate.tableLightIDs.push_back(light.objectID);
        ++candidate.table.lightCount;
    }

    lighting = candidate;
    return true;
}

namespace
{
    // The largest channel's change relative to the larger of the two values' largest channel (0 when both are black).
    float RelativeRadianceChange(const float (&left)[3], const float (&right)[3])
    {
        float scale = 0.0f;
        float change = 0.0f;

        for(int channel = 0; channel < 3; ++channel)
        {
            scale = std::max(scale, std::max(std::abs(left[channel]), std::abs(right[channel])));
            change = std::max(change, std::abs(left[channel] - right[channel]));
        }

        return scale > 0.0f ? change / scale : 0.0f;
    }

    float RelativeChange(float left, float right)
    {
        const float scale = std::max(std::abs(left), std::abs(right));
        return scale > 0.0f ? std::abs(left - right) / scale : 0.0f;
    }

    float Dot3(const float (&left)[3], const float (&right)[3])
    {
        return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
    }

    float Length3(const float (&value)[3])
    {
        return std::sqrt(Dot3(value, value));
    }

    // Whether two direction vectors (any length) are more than `cosine` apart; a zero vector compares as unchanged.
    bool Turned(const float (&left)[3], const float (&right)[3], float cosine)
    {
        const float lengths = Length3(left) * Length3(right);
        return lengths > 0.0f && Dot3(left, right) < cosine * lengths;
    }

    bool UniqueIDs(const std::vector<std::string>& ids)
    {
        std::unordered_map<std::string, int> seen;

        for(const std::string& id : ids)
        {
            if(id.empty() || !seen.emplace(id, 0).second)
            {
                return false;
            }
        }

        return true;
    }
}

LightingDiscontinuities DetectLightingDiscontinuities(const FrameLighting& previous, const FrameLighting& current,
    float relativeThreshold, float directionalDegrees, std::size_t maxRegions)
{
    LightingDiscontinuities result;
    const float directionalCosine = std::cos(directionalDegrees * 3.14159265358979f / 180.0f);
    // A local light's own turn (a spot swinging round) counts only when it is large: its cone sweeps new surfaces.
    const float localCosine = std::cos(30.0f * 3.14159265358979f / 180.0f);

    // The sun (render units: colour times intensity) and its direction.
    {
        const float before[3] = {previous.sun.color.x * previous.sun.intensity, previous.sun.color.y * previous.sun.intensity,
            previous.sun.color.z * previous.sun.intensity};
        const float after[3] = {current.sun.color.x * current.sun.intensity, current.sun.color.y * current.sun.intensity,
            current.sun.color.z * current.sun.intensity};
        const float beforeDirection[3] = {previous.sun.direction.x, previous.sun.direction.y, previous.sun.direction.z};
        const float afterDirection[3] = {current.sun.direction.x, current.sun.direction.y, current.sun.direction.z};
        const bool lit = Length3(before) > 0.0f || Length3(after) > 0.0f;

        if(RelativeRadianceChange(before, after) > relativeThreshold ||
            (lit && Turned(beforeDirection, afterDirection, directionalCosine)))
        {
            result.global = true;
            return result;
        }
    }

    const std::size_t previousCount = std::min<std::size_t>(previous.table.lightCount, previous.table.lights.size());
    const std::size_t currentCount = std::min<std::size_t>(current.table.lightCount, current.table.lights.size());
    const bool byID = previous.tableLightIDs.size() == previousCount && current.tableLightIDs.size() == currentCount &&
        UniqueIDs(previous.tableLightIDs) && UniqueIDs(current.tableLightIDs);

    if(!byID && previousCount != currentCount)
    {
        result.global = true;
        return result;
    }

    const auto addRegion = [&](const GPULight& light)
    {
        const float radius = std::abs(light.range);

        if(light.type == static_cast<std::uint32_t>(LightType::Directional) || !(radius > 0.0f) || !std::isfinite(radius))
        {
            result.global = true;
            return;
        }

        result.regions.push_back({light.position[0], light.position[1], light.position[2], radius});
    };

    std::unordered_map<std::string, std::size_t> previousIndex;

    if(byID)
    {
        previousIndex.reserve(previousCount);

        for(std::size_t index = 0; index < previousCount; ++index)
        {
            previousIndex.emplace(previous.tableLightIDs[index], index);
        }
    }

    std::vector<bool> matched(previousCount, false);

    for(std::size_t index = 0; index < currentCount && !result.global; ++index)
    {
        const GPULight& light = current.table.lights[index];
        std::size_t match = index;

        if(byID)
        {
            const auto found = previousIndex.find(current.tableLightIDs[index]);

            if(found == previousIndex.end())
            {
                addRegion(light);
                continue;
            }

            match = found->second;
        }

        matched[match] = true;
        const GPULight& before = previous.table.lights[match];
        const bool directional = light.type == static_cast<std::uint32_t>(LightType::Directional) ||
            before.type == static_cast<std::uint32_t>(LightType::Directional);
        bool changed = light.type != before.type || RelativeRadianceChange(before.radiance, light.radiance) > relativeThreshold;

        if(directional)
        {
            const bool lit = Length3(before.radiance) > 0.0f || Length3(light.radiance) > 0.0f;

            if(changed || (lit && Turned(before.direction, light.direction, directionalCosine)))
            {
                result.global = true;
            }

            continue;
        }

        const float moved[3] = {light.position[0] - before.position[0], light.position[1] - before.position[1],
            light.position[2] - before.position[2]};
        changed = changed || RelativeChange(std::abs(before.range), std::abs(light.range)) > relativeThreshold ||
            Length3(moved) > 0.5f * std::max(std::abs(before.range), std::abs(light.range));

        if(light.type == static_cast<std::uint32_t>(LightType::Spot))
        {
            changed = changed || std::abs(light.cosOuterCone - before.cosOuterCone) > 0.05f ||
                std::abs(light.cosInnerCone - before.cosInnerCone) > 0.05f ||
                (Length3(light.radiance) > 0.0f && Turned(before.direction, light.direction, localCosine));
        }

        if(changed)
        {
            addRegion(light);

            // A light that also moved or changed range leaves stale light where it was.
            if(Length3(moved) > 0.0f || before.range != light.range)
            {
                addRegion(before);
            }
        }
    }

    for(std::size_t index = 0; index < previousCount && !result.global; ++index)
    {
        if(!matched[index])
        {
            addRegion(previous.table.lights[index]);
        }
    }

    if(result.global || result.regions.size() > maxRegions)
    {
        result.global = true;
        result.regions.clear();
    }

    return result;
}

bool SameLocalLightInputs(const SceneLightTable& left, const SceneLightTable& right)
{
    std::uint32_t leftIndex = 0;
    std::uint32_t rightIndex = 0;
    const auto skipDirectionals = [](const SceneLightTable& table, std::uint32_t& index)
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
