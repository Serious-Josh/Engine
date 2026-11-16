#pragma once

// R1.5: the scene's moving-solid participants, resolved from authored data alone. One shared rule for every host: the Editor's
// physics sessions (PhysicsSimulationController::ApplyMovingSolidParticipationPolicy, on the document the session registered) and the
// packaged runtime's request-level resolution (Engine::ResolvePackagedAirConfiguration, on the cooked document) both call it, so a
// file scene, its archive activation and its packaged build resolve the same bodies.

#include "Physics/MovingSolidParticipation.h"
#include "Scene/AirSimulationSettings.h"
#include "Scene/SceneDocument.h"

#include <algorithm>
#include <string>
#include <vector>

// The object IDs of the bodies with a PhysicsComponent that ResolveMovingSolidParticipation admits, in object-ID order. Empty while
// the scene's moving solids are off (`simulation.enabled && simulation.movingSolids`). Character Controllers are not listed: they
// have no PhysicsComponent and always participate while the feature is on.
inline std::vector<std::string> ResolveMovingSolidParticipants(const SceneDocument& document, const AirSimulationSettings& simulation)
{
    std::vector<std::string> participants;
    const bool sceneEnabled = simulation.enabled && simulation.movingSolids;

    if(!sceneEnabled)
    {
        return participants;
    }

    for(const SceneObjectDocument& object : document.objects)
    {
        if(object.physics && Physics::ResolveMovingSolidParticipation(true, *object.physics, object.transform.scale).participates)
        {
            participants.push_back(object.id);
        }
    }

    std::sort(participants.begin(), participants.end());
    return participants;
}
