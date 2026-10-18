#include "Scene/SceneFileCreation.h"

#include "Project/ProjectPaths.h"
#include "Scene/SceneSerializer.h"

#include <objbase.h>

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

        GUID id{};

        if (FAILED(CoCreateGuid(&id)))
        {
            return Fail(errorMessage, "Failed to generate a new scene ID.");
        }

        // Encode all UUID bytes; no clock or process-local counter is used as identity.
        const auto* bytes = reinterpret_cast<const unsigned char*>(&id);
        constexpr char hex[] = "0123456789abcdef";
        SceneDocument document;
        document.id = "scene-";

        for (std::size_t index = 0; index < sizeof(id); ++index)
        {
            document.id += hex[bytes[index] >> 4];
            document.id += hex[bytes[index] & 15];
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
