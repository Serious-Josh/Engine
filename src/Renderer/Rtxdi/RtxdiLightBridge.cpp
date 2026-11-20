#include "Renderer/Rtxdi/RtxdiLightBridge.h"
#include "Renderer/FrameLighting.h"

#include "../../../assets/Shaders/RtxdiEnvironmentConfig.hlsli"

#include <algorithm>
#include <cmath>
#include <iterator>

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

    // PT.2B: a spot light's samples survive only radiance changes. A moved, rotated or reshaped cone made reused samples near
    // its steep edge high-variance (measured: up to 1.7x the error of dropping them), the D.2C.2R cone-edge mechanism.
    bool SameSpotCone(const GPULight& previous, const GPULight& current)
    {
        GPULight previousCone = previous;
        GPULight currentCone = current;
        std::fill(std::begin(previousCone.radiance), std::end(previousCone.radiance), 0.0f);
        std::fill(std::begin(currentCone.radiance), std::end(currentCone.radiance), 0.0f);
        return previousCone == currentCone;
    }

    RtxdiLightRecord MakeSunRecord(const RtxdiSunSource& sun)
    {
        RtxdiLightRecord record{};
        record.type = static_cast<std::uint32_t>(RtxdiLightKind::Sun);

        for(int axis = 0; axis < 3; ++axis)
        {
            record.direction[axis] = sun.directionToSun[axis];
            record.radiance[axis] = sun.irradiance[axis];
        }

        record.cosOuterCone = sun.cosAngularRadius;
        return record;
    }
}

RtxdiEnvironmentStrategy CompiledRtxdiEnvironmentStrategy() noexcept
{
    static_assert(RTXDI_ENVIRONMENT_STRATEGY_INFINITE_LIGHT ==
        static_cast<int>(RtxdiEnvironmentStrategy::InfiniteLight));
    static_assert(RTXDI_ENVIRONMENT_STRATEGY_RESERVOIR == static_cast<int>(RtxdiEnvironmentStrategy::Reservoir));
    static_assert(RTXDI_ENVIRONMENT_STRATEGY_SEPARATE_MIS == static_cast<int>(RtxdiEnvironmentStrategy::SeparateMis));
    return static_cast<RtxdiEnvironmentStrategy>(RTXDI_ENVIRONMENT_STRATEGY);
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
    RtxdiEnvironmentStrategy strategy, RtxdiLightSet& set)
{
    RtxdiLightSet candidate;
    // Every table light plus the sun and the HDRI (many-light M2: no fixed capacity).
    candidate.records.reserve(std::size_t{lighting.table.lightCount} + 2);
    candidate.identities.reserve(std::size_t{lighting.table.lightCount} + 2);

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

            candidate.records.push_back(light);
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

        candidate.records.push_back(MakeSunRecord(sun));
        candidate.identities.push_back(std::move(identity));
    }

    const auto infiniteEnd = static_cast<std::uint32_t>(candidate.records.size());
    const bool environmentLight = strategy != RtxdiEnvironmentStrategy::SeparateMis &&
        EnvironmentSamplingEnabled(environment);

    if(environmentLight)
    {
        RtxdiLightIdentity identity;
        identity.kind = RtxdiLightKind::Environment;
        identity.environment = environment;

        RtxdiLightRecord record{};
        record.type = static_cast<std::uint32_t>(RtxdiLightKind::Environment);
        candidate.records.push_back(record);
        candidate.identities.push_back(std::move(identity));
    }

    const auto totalCount = static_cast<std::uint32_t>(candidate.records.size());
    const bool environmentRegion = environmentLight && strategy == RtxdiEnvironmentStrategy::Reservoir;

    candidate.bufferParameters.localLightBufferRegion.firstLightIndex = 0;
    candidate.bufferParameters.localLightBufferRegion.numLights = localCount;
    candidate.bufferParameters.infiniteLightBufferRegion.firstLightIndex = localCount;
    candidate.bufferParameters.infiniteLightBufferRegion.numLights = (environmentRegion ? infiniteEnd : totalCount) -
        localCount;
    // Reservoir: the HDRI is RTXDI's environment light, the index after the infinite region. Otherwise RTXDI's
    // environment region stays unused.
    candidate.bufferParameters.environmentLightParams.lightPresent = environmentRegion ? 1u : 0u;
    candidate.bufferParameters.environmentLightParams.lightIndex = environmentRegion ? infiniteEnd :
        RTXDI_INVALID_LIGHT_INDEX;

    set = std::move(candidate);
    return true;
}

void BuildRtxdiLightTranslation(const RtxdiLightSet& previous, const RtxdiLightSet& current,
    bool preserveThroughLightChanges, std::vector<std::int32_t>& currentToPrevious,
    std::vector<std::int32_t>& previousToCurrent)
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

        if(previousIndex < 0)
        {
            continue;
        }

        const RtxdiLightIdentity& previousIdentity = previous.identities[previousIndex];
        const bool wholeIdentityRequired = !preserveThroughLightChanges ||
            identity.kind == RtxdiLightKind::Environment;

        if(wholeIdentityRequired && !(previousIdentity == identity))
        {
            continue;
        }

        if(identity.kind == RtxdiLightKind::Spot && !SameSpotCone(previousIdentity.tableLight, identity.tableLight))
        {
            continue;
        }

        currentToPrevious[currentIndex] = previousIndex;
        previousToCurrent[previousIndex] = static_cast<std::int32_t>(currentIndex);
    }
}
