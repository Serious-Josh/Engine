#include "Scene/FileSceneRuntime.h"

#include <algorithm>

bool FileSceneRuntime::SetObjectCharacterController(const SceneObjectMetadata& selection,
    const std::optional<Physics::CharacterControllerComponent>& expected,
    const std::optional<Physics::CharacterControllerComponent>& replacement, std::string* errorMessage)
{
    const auto fail = [&](const char* message)
    {
        if(errorMessage)
        {
            *errorMessage = message;
        }

        return false;
    };

    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(m_archiveScene || HasPreparedScene() || m_materialAssignment || m_activePresentationAssociation)
    {
        return fail("Character Controller authoring requires an idle editable scene.");
    }

    if(replacement && !Physics::ValidateCharacterControllerComponent(*replacement, errorMessage))
    {
        return false;
    }

    const auto current = GetObjectMetadata(selection.objectID, errorMessage);

    if(!current)
    {
        return false;
    }

    if(current->sceneID != selection.sceneID || current->runtimeIdentity != selection.runtimeIdentity ||
        current->structureRevision != selection.structureRevision || current->runtimeIndex != selection.runtimeIndex)
    {
        return fail("Character Controller target belongs to a stale scene selection.");
    }

    const auto& objects = m_fileScene.GetDocument().objects;
    const auto found = std::find_if(objects.begin(), objects.end(),
        [&](const SceneObjectDocument& object) { return object.id == selection.objectID; });

    if(found == objects.end() || found->characterController != expected)
    {
        return fail("Character Controller changed since inspection; edit rejected.");
    }

    SceneObjectDocument candidate = *found;
    candidate.characterController = replacement;
    candidate.transform = m_scene.GetRenderObjects()[current->runtimeIndex].transform;

    if(!ValidateSceneObjectComponents(candidate, errorMessage))
    {
        return false;
    }

    if(expected == replacement)
    {
        return true;
    }

    const size_t index = static_cast<size_t>(found - objects.begin());
    // All guards precede dirty marking. Geometry, physics and presentation stay untouched.
    m_fileScene.EditDocument().objects[index].characterController = replacement;
    return true;
}

