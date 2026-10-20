#include "Renderer/Rtxdi/RtxdiLightBridge.h"
#include "Renderer/FrameLighting.h"

#include <cmath>

namespace
{
    // PathTraceSunHasEnergy.
    bool SunHasEnergy(const RtxdiSunSource& sun)
    {
        bool anyPositive = false;

        for(const float component : sun.irradiance)
        {
            if(!std::isfinite(component))
            {
                return false;
            }

            anyPositive = anyPositive || component > 0.0f;
        }

        return anyPositive;
    }

    // PathTraceEnvironmentSamplingEnabled.
    bool EnvironmentSamplingEnabled(const RtxdiEnvironmentSource& environment)
    {
        return environment.width != 0 && environment.height != 0 && std::isfinite(environment.totalWeight) &&
            environment.totalWeight > 0.0f && environment.conditionalCDFIndex != 0xFFFFFFFFu &&
            environment.marginalCDFIndex != 0xFFFFFFFFu;
    }

    bool SameKey(const RtxdiLightIdentity& left, const RtxdiLightIdentity& right)
    {
        return left.kind == right.kind && left.objectID == right.objectID;
    }

    // The only index in `set` carrying the key of `identity`, or -1 when none or several do.
    std::int32_t FindUniqueKey(const RtxdiLightSet& set, const RtxdiLightIdentity& identity)
    {
        std::int32_t found = -1;

        for(std::size_t index = 0; index < set.identities.size(); ++index)
        {
            if(SameKey(set.identities[index], identity))
            {
                if(found >= 0)
                {
                    return -1;
                }

                found = static_cast<std::int32_t>(index);
            }
        }

        return found;
    }
}

RtxdiSunSource MakeRtxdiSunSource(const DirectionalLight& sun)
{
    // DirectionalLight::direction is the direction light travels.
    const Vector3 directionToSun = sun.direction.Normalized() * -1.0f;
    const Vector3 irradiance = sun.color * sun.intensity;

    RtxdiSunSource source;
    source.directionToSun[0] = directionToSun.x;
    source.directionToSun[1] = directionToSun.y;
    source.directionToSun[2] = directionToSun.z;
    source.cosAngularRadius = sun.angularRadius > 0.0f ? std::cos(sun.angularRadius) : 1.0f;
    source.irradiance[0] = irradiance.x;
    source.irradiance[1] = irradiance.y;
    source.irradiance[2] = irradiance.z;
    return source;
}

bool BuildRtxdiLightSet(const FrameLighting& lighting, const RtxdiEnvironmentSource& environment,
    RtxdiLightSet& set)
{
    RtxdiLightSet candidate;
    candidate.records.reserve(RtxdiLightSet::Capacity);
    candidate.identities.reserve(RtxdiLightSet::Capacity);

    const auto addTableLights = [&](bool local)
    {
        for(std::uint32_t index = 0; index < lighting.table.lightCount; ++index)
        {
            const GPULight& light = lighting.table.lights[index];
            const bool isLocal = light.type != static_cast<std::uint32_t>(RtxdiLightKind::Directional);

            if(isLocal != local)
            {
                continue;
            }

            RtxdiLightIdentity identity;
            identity.kind = static_cast<RtxdiLightKind>(light.type);
            identity.objectID = lighting.tableLightIDs[index];
            identity.tableLight = light;

            candidate.records.push_back({light.type, index});
            candidate.identities.push_back(std::move(identity));
        }
    };

    for(std::uint32_t index = 0; index < lighting.table.lightCount; ++index)
    {
        if(lighting.table.lights[index].type > static_cast<std::uint32_t>(RtxdiLightKind::Directional))
        {
            return false;
        }
    }

    addTableLights(true);
    const auto localCount = static_cast<std::uint32_t>(candidate.records.size());
    addTableLights(false);

    const RtxdiSunSource sun = MakeRtxdiSunSource(lighting.sun);

    if(SunHasEnergy(sun))
    {
        RtxdiLightIdentity identity;
        identity.kind = RtxdiLightKind::Sun;
        identity.objectID = lighting.sunObjectID;
        identity.sun = sun;

        candidate.records.push_back({static_cast<std::uint32_t>(RtxdiLightKind::Sun), 0});
        candidate.identities.push_back(std::move(identity));
    }

    if(EnvironmentSamplingEnabled(environment))
    {
        RtxdiLightIdentity identity;
        identity.kind = RtxdiLightKind::Environment;
        identity.environment = environment;

        candidate.records.push_back({static_cast<std::uint32_t>(RtxdiLightKind::Environment), 0});
        candidate.identities.push_back(std::move(identity));
    }

    const auto totalCount = static_cast<std::uint32_t>(candidate.records.size());

    candidate.bufferParameters.localLightBufferRegion.firstLightIndex = 0;
    candidate.bufferParameters.localLightBufferRegion.numLights = localCount;
    candidate.bufferParameters.infiniteLightBufferRegion.firstLightIndex = localCount;
    candidate.bufferParameters.infiniteLightBufferRegion.numLights = totalCount - localCount;
    // The HDRI is an infinite light; RTXDI's environment region (and its presampling) stays unused.
    candidate.bufferParameters.environmentLightParams.lightPresent = 0;
    candidate.bufferParameters.environmentLightParams.lightIndex = RTXDI_INVALID_LIGHT_INDEX;

    set = std::move(candidate);
    return true;
}

void BuildRtxdiLightTranslation(const RtxdiLightSet& previous, const RtxdiLightSet& current,
    std::vector<std::int32_t>& currentToPrevious, std::vector<std::int32_t>& previousToCurrent)
{
    currentToPrevious.assign(current.identities.size(), -1);
    previousToCurrent.assign(previous.identities.size(), -1);

    for(std::size_t currentIndex = 0; currentIndex < current.identities.size(); ++currentIndex)
    {
        const RtxdiLightIdentity& identity = current.identities[currentIndex];

        if(FindUniqueKey(current, identity) != static_cast<std::int32_t>(currentIndex))
        {
            continue;
        }

        const std::int32_t previousIndex = FindUniqueKey(previous, identity);

        if(previousIndex < 0 || !(previous.identities[previousIndex] == identity))
        {
            continue;
        }

        currentToPrevious[currentIndex] = previousIndex;
        previousToCurrent[previousIndex] = static_cast<std::int32_t>(currentIndex);
    }
}
