#pragma once

#include "Scene/SceneDocument.h"

#include <optional>
#include <string>

// W3: the one resolution of a scene's effective environment (Engine_World_Local_Level_Architecture §5.1-5.2), shared by standalone
// Local Level preview now and by composite Worlds (W4) later. It reads documents only; it never changes them.
//
//   Local Level, per group: its own stored values where its override is enabled; otherwise its World's values when the World is
//   valid; otherwise the Engine scene defaults (SceneDocument's member defaults).
//   AIR simulation: a valid World's configuration, with the Local Level's wind when its wind override is enabled; without a valid
//   World, the Local Level's own configuration, with the default wind unless its wind override is enabled.
//   World: its own values.
struct EffectiveSceneEnvironment
{
    enum class Source
    {
        // The document is a World, previewing itself.
        World,
        // A Local Level with no owner.
        Unassigned,
        // A Local Level whose owning World was read and is valid.
        AssignedWorld,
        // A Local Level whose owner is missing, unreadable, not a World or does not list it: resolved as if unassigned.
        InvalidWorld
    };

    Source source = Source::Unassigned;
    // The owning World's name when it was read; the problem when it is invalid.
    std::string worldName;
    std::string worldProblem;

    std::string environmentReference;
    SceneLightingSettings lightingSettings;
    std::optional<EnvironmentalHazeSettings> environmentalHaze;
    std::optional<MistRenderSettings> mistRendering;
    std::optional<AirEnvironmentSettings> airEnvironment;
    std::optional<AirSimulationSettings> airSimulation;

    bool operator==(const EffectiveSceneEnvironment&) const = default;
};

// `world` is the owning World's document when the caller could read it (null when the scene is unassigned or the World could not be
// read; `worldProblem` then explains why for an assigned Local Level). The World is valid only when it is a World that lists the
// Local Level.
EffectiveSceneEnvironment ResolveSceneEnvironment(const SceneDocument& scene, const SceneDocument* world,
    const std::string& worldProblem = {});

// Writes an environment's values into a document's environment fields: a Local Level's own values when it is unassigned (keeping
// its effective environment), a World's values on Import Environment and World environment edits. Overrides are not changed.
void StoreEnvironment(SceneDocument& local, const EffectiveSceneEnvironment& environment);

// The groups whose effective values differ between two resolutions (empty when none): for assignment warnings.
std::string DescribeEnvironmentDifferences(const EffectiveSceneEnvironment& before, const EffectiveSceneEnvironment& after);
