#pragma once

#include "DemoContent/FileBackedDemoScene.h"
#include "Engine/ProbeVolumes.h"
#include "Scene/FileSceneRuntime.h"

#include <memory>
#include <optional>
#include <vector>

// How the Editor's Probe Volume and reflection-bake code read the live scene: world transforms and meshes from the runtime that
// presents the document. Borrowed references; the result is valid while both are.
namespace ProbeSceneAccess
{
    inline Audio::AcousticTransform ToTransform(const Matrix4& world)
    {
        Audio::AcousticTransform transform{};

        for(std::size_t row = 0; row < 4; ++row)
        {
            for(std::size_t column = 0; column < 4; ++column)
            {
                transform[row][column] = world.m[row][column];
            }
        }

        return transform;
    }

    inline ProbeVolumes::SceneAccess MakeSceneAccess(const FileSceneRuntime& runtime, const IDemoScene& scene)
    {
        ProbeVolumes::SceneAccess access;
        // Authored worlds, not the presented ones: during Play a physics body is presented through its pose, whose rounding differs from
        // the authored transform in the last bits, and validity must judge what was generated and baked. Computed once per access, on first use.
        auto authored = std::make_shared<std::optional<std::vector<Matrix4>>>();
        access.world = [&runtime, authored](const SceneObjectDocument& object) -> std::optional<Audio::AcousticTransform>
        {
            const auto& mapping = runtime.GetObjectMapping().idToRuntimeIndex;
            const auto found = mapping.find(object.id);

            if(!authored->has_value())
            {
                std::vector<Matrix4> worlds;
                authored->emplace(runtime.GetScene().ComputeAuthoredWorlds(worlds) ? std::move(worlds) : std::vector<Matrix4>{});
            }

            if(found == mapping.end() || found->second >= (*authored)->size())
            {
                return std::nullopt;
            }

            return ToTransform((**authored)[found->second]);
        };
        access.mesh = [&runtime, &scene](const SceneObjectDocument& object) -> const ImportedMesh*
        {
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
