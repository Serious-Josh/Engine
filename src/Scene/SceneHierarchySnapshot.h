#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

struct SceneHierarchyEntry
{
    std::string objectID;
    std::string name;
    // Empty-mesh instances still have runtime entries. Null means unavailable,
    // never an index synthesized from the document position.
    std::optional<std::size_t> runtimeIndex;
    std::string parentID;
};

struct SceneHierarchySnapshot
{
    std::string sceneName;
    std::vector<SceneHierarchyEntry> entries;
};
