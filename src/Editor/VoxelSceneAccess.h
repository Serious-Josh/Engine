#pragma once

#include "DemoContent/IDemoScene.h"
#include "Engine/VoxelScene.h"
#include "Scene/FileSceneRuntime.h"

#include <memory>
#include <optional>
#include <vector>

// VX.4C.1: how the Editor's voxel capture reads the live scene: authored world matrices (Scene::ComputeAuthoredWorlds, so a physics
// pose never stands in for the authored transform) and CPU meshes from the scene's catalog (GetMeshGeometry, which also serves the
// built-in sphere and triangle). Borrowed references; valid while the runtime and scene are, which Capture's single call guarantees.
namespace VoxelSceneAccess
{
inline VoxelScene::SceneAccess Make(const FileSceneRuntime& runtime, const IDemoScene& scene)
{
    VoxelScene::SceneAccess access;
    // Computed once per access, on first use.
    auto authored = std::make_shared<std::optional<std::vector<Matrix4>>>();

    access.world = [&runtime, authored](const SceneObjectDocument& object) -> std::optional<Matrix4> {
        if(!authored->has_value())
        {
            std::vector<Matrix4> worlds;
            authored->emplace(runtime.GetScene().ComputeAuthoredWorlds(worlds) ? std::move(worlds) : std::vector<Matrix4>{});
        }

        const auto& mapping = runtime.GetObjectMapping().idToRuntimeIndex;
        const auto found = mapping.find(object.id);

        if(found == mapping.end() || found->second >= (*authored)->size())
        {
            return std::nullopt;
        }

        return (**authored)[found->second];
    };

    access.mesh = [&runtime, &scene](const SceneObjectDocument& object) -> const ImportedMesh* {
        const auto& mapping = runtime.GetObjectMapping().idToRuntimeIndex;
        const auto& objects = runtime.GetScene().GetRenderObjects();
        const auto found = mapping.find(object.id);

        if(found == mapping.end() || found->second >= objects.size() || !objects[found->second].mesh)
        {
            return nullptr;
        }

        return scene.GetMeshGeometry(*objects[found->second].mesh);
    };

    return access;
}
}
