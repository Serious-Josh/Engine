#include "Scene/SceneEnvironmentResolution.h"

#include <algorithm>

namespace
{
    // Equality of optional settings whose type has operator==; absent equals absent only.
    template<typename Settings>
    bool Same(const std::optional<Settings>& left, const std::optional<Settings>& right)
    {
        return left.has_value() == right.has_value() && (!left || *left == *right);
    }

    bool SameVector(const Vector3& left, const Vector3& right)
    {
        return left.x == right.x && left.y == right.y && left.z == right.z;
    }
}

EffectiveSceneEnvironment ResolveSceneEnvironment(const SceneDocument& scene, const SceneDocument* world, const std::string& worldProblem)
{
    EffectiveSceneEnvironment result;

    if(scene.kind == SceneKind::World)
    {
        result.source = EffectiveSceneEnvironment::Source::World;
        result.worldName = scene.name;
        result.environmentReference = scene.environmentReference;
        result.lightingSettings = scene.lightingSettings;
        result.environmentalHaze = scene.environmentalHaze;
        result.mistRendering = scene.mistRendering;
        result.airEnvironment = scene.airEnvironment;
        result.airSimulation = scene.airSimulation;
        return result;
    }

    // The World provides inherited values only when it is a World that lists this Local Level.
    const SceneDocument* owner = nullptr;

    if(scene.owningWorld.empty())
    {
        result.source = EffectiveSceneEnvironment::Source::Unassigned;
    }
    else if(!world)
    {
        result.source = EffectiveSceneEnvironment::Source::InvalidWorld;
        result.worldProblem = worldProblem.empty() ? "The owning World " + scene.owningWorld + " could not be read." : worldProblem;
    }
    else if(world->kind != SceneKind::World || world->id != scene.owningWorld)
    {
        result.source = EffectiveSceneEnvironment::Source::InvalidWorld;
        result.worldProblem = "The owning World " + scene.owningWorld + " is not a World with that identity.";
    }
    else if(std::none_of(world->members.begin(), world->members.end(), [&](const SceneMembership& member)
        {
            return member.level == scene.id;
        }))
    {
        result.source = EffectiveSceneEnvironment::Source::InvalidWorld;
        result.worldName = world->name;
        result.worldProblem = "World '" + world->name + "' does not list this Local Level.";
    }
    else
    {
        result.source = EffectiveSceneEnvironment::Source::AssignedWorld;
        result.worldName = world->name;
        owner = world;
    }

    // Disabled groups inherit from the valid World, or from the Engine scene defaults.
    const SceneDocument defaults;
    const SceneDocument& inherited = owner ? *owner : defaults;
    const auto& overrides = scene.environmentOverrides;

    result.environmentReference = overrides.sky ? scene.environmentReference : inherited.environmentReference;
    result.lightingSettings.environmentLuminanceScale = overrides.sky ? scene.lightingSettings.environmentLuminanceScale :
        inherited.lightingSettings.environmentLuminanceScale;
    result.lightingSettings.referenceEV100 = overrides.exposure ? scene.lightingSettings.referenceEV100 :
        inherited.lightingSettings.referenceEV100;
    result.environmentalHaze = overrides.haze ? scene.environmentalHaze : inherited.environmentalHaze;
    result.mistRendering = overrides.mist ? scene.mistRendering : inherited.mistRendering;
    result.airEnvironment = overrides.airEnvironment ? scene.airEnvironment : inherited.airEnvironment;

    // AIR simulation: the World's configuration when assigned, the Local Level's own otherwise; wind as its own group.
    std::optional<AirSimulationSettings> simulation = owner ? owner->airSimulation : scene.airSimulation;
    const Vector3 defaultWind = AirSimulationSettings{}.ambientVelocity;
    Vector3 wind = defaultWind;

    if(overrides.wind)
    {
        wind = scene.airSimulation.value_or(AirSimulationSettings{}).ambientVelocity;
    }
    else if(owner)
    {
        wind = owner->airSimulation.value_or(AirSimulationSettings{}).ambientVelocity;
    }

    if(simulation || !SameVector(wind, defaultWind))
    {
        AirSimulationSettings settings = simulation.value_or(AirSimulationSettings{});
        settings.ambientVelocity = wind;
        simulation = settings;
    }

    result.airSimulation = simulation;
    return result;
}

void StoreEnvironment(SceneDocument& local, const EffectiveSceneEnvironment& environment)
{
    local.environmentReference = environment.environmentReference;
    local.lightingSettings = environment.lightingSettings;
    local.environmentalHaze = environment.environmentalHaze;
    local.mistRendering = environment.mistRendering;
    local.airEnvironment = environment.airEnvironment;
    local.airSimulation = environment.airSimulation;
}

std::string DescribeEnvironmentDifferences(const EffectiveSceneEnvironment& before, const EffectiveSceneEnvironment& after)
{
    std::string groups;
    const auto add = [&](bool differs, const char* name)
    {
        if(differs)
        {
            groups += groups.empty() ? name : std::string(", ") + name;
        }
    };

    add(before.environmentReference != after.environmentReference ||
        before.lightingSettings.environmentLuminanceScale != after.lightingSettings.environmentLuminanceScale, "sky");
    add(before.lightingSettings.referenceEV100 != after.lightingSettings.referenceEV100, "exposure reference");
    add(!Same(before.environmentalHaze, after.environmentalHaze), "haze");
    add(!Same(before.mistRendering, after.mistRendering), "mist appearance");
    add(!Same(before.airEnvironment, after.airEnvironment), "AIR environment");
    const AirSimulationSettings beforeSimulation = before.airSimulation.value_or(AirSimulationSettings{});
    const AirSimulationSettings afterSimulation = after.airSimulation.value_or(AirSimulationSettings{});
    add(!SameVector(beforeSimulation.ambientVelocity, afterSimulation.ambientVelocity), "wind");
    AirSimulationSettings beforeRest = beforeSimulation;
    AirSimulationSettings afterRest = afterSimulation;
    beforeRest.ambientVelocity = afterRest.ambientVelocity;
    add(!(beforeRest == afterRest), "AIR simulation");
    return groups;
}
