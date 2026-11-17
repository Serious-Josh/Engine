#pragma once

#include "Scene/CompositeWorld.h"

#include <filesystem>
#include <string>

class ProjectPaths;
class ProjectAssetRegistry;

namespace ProjectCompositeWorld
{
    // W4: reads the World at `worldPath` (absolute, inside the project) and every member through the registry, checks project-wide
    // membership (no member is listed by another World), and builds the composite (BuildCompositeWorld). Reads only; never writes a
    // file. Fails with a description and leaves `composite` unchanged.
    bool Load(const ProjectPaths& paths, const ProjectAssetRegistry& registry, const std::filesystem::path& worldPath,
        CompositeWorld& composite, std::string& error);
}
