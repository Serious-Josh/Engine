#pragma once

#include "Scene/PreparedScene.h"
#include "Scene/Scene.h"
#include "Scene/SceneMeshResidency.h"

#include <string>
#include <unordered_map>

struct SceneObjectMapping
{
    std::unordered_map<std::string, RenderObjectID> idToRuntimeIndex;

    // Borrowed residency/material identities captured at commit; never dereferenced
    // by capture. Future residency transactions must update these coherently with
    // the live object, after renderer/GPU safety is established.
    std::vector<RenderObject> committedObjects;
    // Per-object binding epochs; residency never changes the structural revision.
    std::vector<std::uint64_t> bindingRevisions;
    std::uint64_t structureRevision = 0;
    const Scene* scene = nullptr;
};

// Authored references/overrides belong to the document, including while pending.
// Do not infer an empty assignment from a null runtime mesh, or silently accept
// an uncommitted pointer/slot-map change as a residency transition.
inline bool MatchesCommittedSceneObjectBindings(const SceneObjectDocument& authored,
    const RenderObject& runtime, const RenderObject& committed)
{
    if(runtime.mesh != committed.mesh || runtime.material != committed.material ||
        runtime.slotMaterials != committed.slotMaterials || runtime.materialsDeferred != committed.materialsDeferred)
    {
        return false;
    }

    const auto residency = GetSceneMeshResidency(authored.meshReference, runtime.mesh);

    if(!residency)
    {
        return false;
    }

    return *residency != SceneMeshResidency::Empty || runtime.slotMaterials.empty();
}

// Replaces the runtime scene contents with prepared data.
// Call only when the scene is not actively being used by the renderer.
bool CommitPreparedScene(
    const PreparedScene& prepared,
    Scene& scene,
    SceneObjectMapping& mapping,
    std::string* errorMessage = nullptr);

// Separate pending-only transaction. Same inactive scene / checked GPU-safety
// precondition as full commit. Use FileSceneRuntime::CommitMetadata for catalog
// revalidation. No geometry upload or renderer publication occurs here.
bool CommitPreparedMetadataScene(const PreparedMetadataScene& prepared, Scene& scene,
    SceneObjectMapping& mapping, std::string* errorMessage = nullptr);

// Editor-only replacement: validates each prepared binding against the authored
// document. Resident borrows must come from retained catalog geometry; pending
// slots contain exactly authored overrides. Same inactive/GPU-safe precondition.
bool CommitPreparedEditorScene(const PreparedScene& prepared, const SceneDocument& document,
    Scene& scene, SceneObjectMapping& mapping, std::string* errorMessage = nullptr);
