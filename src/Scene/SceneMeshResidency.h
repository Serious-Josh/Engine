#pragma once

#include <optional>
#include <string_view>

class Mesh;

enum class SceneMeshResidency { Empty, Pending, Resident };

// Classification only: reference validity and registry availability still belong
// to scene preparation. Pending means not resident, not necessarily actively loading.
// An empty authored assignment with resident geometry is inconsistent.
inline std::optional<SceneMeshResidency> GetSceneMeshResidency(
    std::string_view authoredMeshReference, const Mesh* residentMesh) noexcept
{
    if(authoredMeshReference.empty())
    {
        if(residentMesh)
        {
            return std::nullopt;
        }

        return SceneMeshResidency::Empty;
    }

    return residentMesh ? SceneMeshResidency::Resident : SceneMeshResidency::Pending;
}
