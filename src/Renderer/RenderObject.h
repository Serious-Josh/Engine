#pragma once

#include "Scene/Transform.h"
#include <map>
#include <string>

class Mesh;
struct Material;

struct RenderObject
{
    // Resident GPU geometry only. A null pointer does not describe the authored
    // assignment, which file-backed scenes retain in SceneObjectDocument.
    Mesh* mesh = nullptr;

    Material* material = nullptr;

    Transform transform;
    Transform previousTransform;

    // Effective runtime borrows, not authored overrides. The document retains
    // overrides independently of geometry/metadata availability.
    // Pending objects may retain explicit overrides only, not a complete slot table.
    // Empty maps retain legacy fallback behavior.
    std::map<std::string, Material*> slotMaterials;

    Material* GetSlotMaterial(const std::string& slotID) const noexcept
    {
        const auto found = slotMaterials.find(slotID);
        return found == slotMaterials.end() ? material : found->second;
    }
};
