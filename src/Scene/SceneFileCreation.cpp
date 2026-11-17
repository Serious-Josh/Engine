#include "Scene/SceneFileCreation.h"

#include "Project/ProjectPaths.h"
#include "Scene/SceneSerializer.h"
#include "assets/AssetID.h"

#include <cstdint>
#include <limits>
#include <utility>

namespace
{
    bool Fail(
        std::string* errorMessage,
        const std::string& message)
    {
        if (errorMessage)
        {
            *errorMessage = message;
        }

        return false;
    }

    // W1.1: a canonical GUID, so the scene can be written in schema 28 and its id is its registry asset identity.
    bool GenerateSceneID(
        std::string& sceneID,
        std::string* errorMessage)
    {
        const auto id = AssetID::Generate();

        if (!id)
        {
            return Fail(errorMessage, "Failed to generate a new scene ID.");
        }

        sceneID = id->ToString();
        return true;
    }
}

bool CreateCleanSceneDocument(
    SceneDocument& document,
    std::string* errorMessage)
{
    SceneDocument candidate;

    if (!GenerateSceneID(candidate.id, errorMessage))
    {
        return false;
    }

    // Camera, light, environment and schema version keep SceneDocument's defaults, the same
    // values every new scene file starts from. No objects, so no starter sphere.
    candidate.name = "Untitled Scene";
    // W3: a new Local Level inherits its environment (from its World, or the Engine defaults) until a group is overridden.
    candidate.environmentOverrides = SceneEnvironmentOverrides::None();
    document = std::move(candidate);
    return true;
}

bool CreateNewSceneFile(
    const ProjectPaths& project,
    std::filesystem::path& createdPath,
    std::string* errorMessage)
{
    try
    {
        if (!project.EnsureDirectories(errorMessage))
        {
            return false;
        }

        SceneDocument document;
        // W3: new Local Levels start with every environment group inherited.
        document.environmentOverrides = SceneEnvironmentOverrides::None();

        if (!GenerateSceneID(document.id, errorMessage))
        {
            return false;
        }

        SceneObjectDocument sphere;
        // One starter object per new UUID-backed scene gives it a distinct,
        // persistent identity without tying identity to the chosen filename.
        sphere.id = "object-" + document.id;
        sphere.name = "Sphere";
        sphere.meshReference = "builtin:sphere";
        sphere.materialReference = "builtin:default";

        // SceneObjectDocument supplies the identity transform by default.
        document.objects.push_back(std::move(sphere));

        for (std::uint64_t suffix = 0;; ++suffix)
        {
            document.name = suffix == 0
                ? "Untitled Scene"
                : "Untitled Scene (" + std::to_string(suffix) + ")";
            auto candidatePath = project.GetScenesDirectory() / (document.name + ".scene");
            const auto result = CreateSceneDocument(document, candidatePath, errorMessage);

            if (result == CreateSceneDocumentResult::Created)
            {
                createdPath = std::move(candidatePath);
                return true;
            }

            if (result == CreateSceneDocumentResult::Error)
            {
                return false;
            }

            if (suffix == (std::numeric_limits<std::uint64_t>::max)())
            {
                return Fail(errorMessage, "No available new-scene filename suffix.");
            }
        }
    }
    catch (const std::exception& exception)
    {
        return Fail(errorMessage, exception.what());
    }
}
