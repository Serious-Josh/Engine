#pragma once

#include "Scene/Transform.h"
#include <map>
#include <string>

class Mesh;
struct Material;

struct RenderObject
{
    Mesh* mesh = nullptr;

    Material* material = nullptr;

    Transform transform;
    Transform previousTransform;

    // Effective borrowed materials for every mesh slot. Empty maps retain legacy fallback behavior.
    std::map<std::string, Material*> slotMaterials;

    Material* GetSlotMaterial(const std::string& slotID) const noexcept
    {
        const auto found = slotMaterials.find(slotID);
        return found == slotMaterials.end() ? material : found->second;
    }
};