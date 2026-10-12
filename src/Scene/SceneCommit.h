#pragma once

#include "Scene/PreparedScene.h"
#include "Scene/Scene.h"

#include <string>
#include <unordered_map>

struct SceneObjectMapping
{
    std::unordered_map<std::string, RenderObjectID> idToRuntimeIndex;

    // Borrowed identities captured at commit; never dereferenced by capture.
    std::vector<RenderObject> committedObjects;
    std::uint64_t structureRevision = 0;
    const Scene* scene = nullptr;
};

// Replaces the runtime scene contents with prepared data.
// Call only when the scene is not actively being used by the renderer.
bool CommitPreparedScene(
    const PreparedScene& prepared,
    Scene& scene,
    SceneObjectMapping& mapping,
    std::string* errorMessage = nullptr);