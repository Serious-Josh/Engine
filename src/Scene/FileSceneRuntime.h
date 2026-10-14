#pragma once

#include "Scene/FileScene.h"
#include "Scene/Scene.h"
#include "Scene/SceneCommit.h"
#include "Scene/PreparedScene.h"
#include "Scene/SceneHierarchySnapshot.h"
#include "Scene/MeshResidency.h"

#include <optional>
#include <memory>
#include <string>
#include <map>
#include "assets/AssetID.h"

// Copied document metadata. Identity fields are internal selection checks, not UI labels.
struct SceneObjectMetadata
{
    std::string name;
    std::string meshReference;
    std::string materialReference;
    std::string objectID;
    std::string sceneID;

    RenderObjectID runtimeIndex = 0;

    std::uint64_t runtimeIdentity = 0;
    std::uint64_t structureRevision = 0;

    // Copied presentation metadata; never borrows scene-owned material state.
    std::map<std::string, std::string> materialSlotBindings;

    std::uint64_t bindingRevision = 0;

    // Derived from an already validated object mapping; no mutable residency field.
    std::optional<SceneMeshResidency> GetMeshResidency(const RenderObject& runtime) const noexcept
    {
        return GetSceneMeshResidency(meshReference, runtime.mesh);
    }
};

// History restores the exact saved value, including legacy untrimmed names.
enum class SceneObjectNamePolicy { TrimmedInput, ExactHistory };
bool NormalizeSceneObjectName(std::string& name, std::string* errorMessage = nullptr);

struct SceneObjectOrderIdentity
{
    std::string sceneID;
    std::uint64_t runtimeIdentity = 0;
    std::uint64_t structureRevision = 0;
};

struct MaterialAssignmentToken
{
    std::uint64_t runtimeIdentity = 0;
    std::uint64_t transaction = 0;

    bool noOp = false;
    bool operator==(const MaterialAssignmentToken&) const = default;
};

class FileSceneRuntime
{
public:
    FileSceneRuntime();
    bool HasMaterialAssignment() const noexcept
    {
        return m_materialAssignment.has_value();
    }

    std::uint64_t GetRuntimeIdentity() const noexcept
    {
        return m_instanceIdentity;
    }

    FileSceneRuntime(const FileSceneRuntime&) = delete;
    FileSceneRuntime& operator=(const FileSceneRuntime&) = delete;

    // Builds an inactive candidate without changing this runtime. Existing live
    // asset borrows are preserved; caller retains their catalog through GPU cleanup.
    std::unique_ptr<FileSceneRuntime> PrepareObjectCreation(
        SceneAssetResolver& resolver, std::string meshReference, const Transform& transform,
        std::string materialReference, std::string name, std::string& objectID,
        std::string* errorMessage = nullptr) const;

    // Removes one instance from an inactive captured document; surviving asset borrows
    // and live transforms are copied without reloading or destroying catalog assets.
    bool CaptureObjectDeletionSnapshot(std::string_view objectID, SceneObjectDocument& object,
        size_t& documentIndex, std::string* errorMessage = nullptr) const;
    std::unique_ptr<FileSceneRuntime> PrepareObjectDeletion(std::string_view objectID,
        std::string* errorMessage = nullptr, FileSceneAssetCatalog* catalog = nullptr) const;

    std::unique_ptr<FileSceneRuntime> PrepareObjectRestoration(SceneAssetResolver& resolver,
        const SceneObjectDocument& restored, size_t insertionIndex, std::string* errorMessage = nullptr) const;

    const FileScene& GetFileScene() noexcept
    {
        return m_fileScene;
    }

    const FileScene& GetFileScene() const noexcept
    {
        return m_fileScene;
    }

    Scene& GetScene() noexcept
    {
        return m_scene;
    }

    const Scene& GetScene() const noexcept
    {
        return m_scene;
    }

    const SceneObjectMapping& GetObjectMapping() const noexcept
    {
        return m_objectMapping;
    }

    std::optional<SceneObjectMetadata> GetObjectMetadata(
        RenderObjectID runtimeIndex,
        std::string* errorMessage = nullptr) const;
    std::optional<SceneObjectMetadata> GetObjectMetadata(
        std::string_view objectID, std::string* errorMessage = nullptr) const;

    bool ReorderObjects(const SceneObjectOrderIdentity& identity,
        const std::vector<std::string>& expectedOrder, const std::vector<std::string>& replacementOrder,
        std::string* errorMessage = nullptr);
    bool RenameObject(const SceneObjectMetadata& selection, std::string_view expectedName,
        std::string name, std::string* errorMessage = nullptr,
        SceneObjectNamePolicy policy = SceneObjectNamePolicy::TrimmedInput);

    // Ordered, copied metadata; validates the document and mapping once.
    bool BuildHierarchySnapshot(SceneHierarchySnapshot& snapshot, std::string* errorMessage = nullptr) const;

    // CPU preparation only; one outstanding assignment per runtime.
    std::optional<MaterialAssignmentToken> PrepareMaterialAssignment(
        const SceneObjectMetadata& selection, std::string materialReference,
        std::string* errorMessage = nullptr);
    // CPU staging only. Caller resolves/retains effectiveMaterial before preparing.
    // No material ID clears the explicit override and must use the existing fallback pointer.
    std::optional<MaterialAssignmentToken> PrepareMaterialSlotAssignment(
        const SceneObjectMetadata& selection, std::string slotID, const std::optional<AssetID>& materialID,
        Material& effectiveMaterial, std::string* errorMessage = nullptr, FileSceneAssetCatalog* catalog = nullptr);
    Material* GetPreparedMaterialAssignmentMaterial(const MaterialAssignmentToken& token) const noexcept;
    // Resident targets require checked renderer release. Pending targets may
    // commit at the exclusive pre-recording boundary with already-published
    // material identities; they never mutate shader-visible tables.
    // Caller owns and retains BOTH materials through cleanup, rollback and GPU completion.
    bool CommitMaterialAssignment(const MaterialAssignmentToken& token, Material& retainedSceneMaterial,
        std::string* errorMessage = nullptr);
    // Same Renderer/GPU safety precondition as commit; never destroys either material.
    // Rejects intervening edits rather than overwriting them. Finish only after Renderer success.
    bool RollbackMaterialAssignment(const MaterialAssignmentToken& token, std::string* errorMessage = nullptr);
    bool FinishMaterialAssignment(const MaterialAssignmentToken& token, std::string* errorMessage = nullptr);
    bool CancelMaterialAssignment(const MaterialAssignmentToken& token, std::string* errorMessage = nullptr);

    bool Prepare(
        SceneAssetResolver& resolver,
        std::string* errorMessage = nullptr);

    bool Commit(std::string* errorMessage = nullptr);

    // Explicit inactive-runtime path. Requires the catalog's fallback texture
    // policy; FileBackedDemoScene configures it for this mode.
    bool PrepareMetadata(FileSceneAssetCatalog& catalog, std::string* errorMessage = nullptr);
    bool CommitMetadata(FileSceneAssetCatalog& catalog, std::string* errorMessage = nullptr);

    std::optional<MeshResidencyToken> PrepareMeshResidency(const SceneObjectMetadata& selection,
        FileSceneAssetCatalog& catalog, const MeshResidencyCandidate& candidate, std::string* errorMessage = nullptr);
    void CancelMeshResidency() noexcept;
    std::uint64_t GetResidencyRevision() const noexcept { return m_residencyRevision; }

    bool HasPreparedScene() const noexcept
    {
        return m_preparedScene.has_value() || m_preparedMetadata.has_value();
    }

    void DiscardPreparedScene() noexcept
    {
        CancelMeshResidency();
        m_preparedScene.reset();
        m_preparedMetadata.reset();
    }

    bool New(
        std::string sceneID,
        std::string sceneName = "Untitled Scene");

    bool Open(
        const std::filesystem::path& path,
        std::string* errorMessage = nullptr);

    SceneDocument& EditDocument() noexcept;

    // Returns false when live state cannot be represented safely.
    // On failure, dirty is unchanged.
    bool CheckUnsavedChanges(
        bool& dirty,
        std::string* errorMessage = nullptr) const;

    bool Save(std::string* errorMessage = nullptr);

    bool SaveAs(
        const std::filesystem::path& path,
        std::string* errorMessage = nullptr);

private:
    // No coordinator exists in S.5B. Only the future matching renderer/DXR
    // transaction may call this at the main-thread pre-recording boundary.
    friend class MeshResidencyPublication;
    std::optional<SceneObjectMetadata> ValidatePreparedMeshResidencyIdentity(const MeshResidencyToken& token,
        const FileSceneAssetCatalog& catalog, std::string* errorMessage) const;
    MeshResidencySourceLock ValidatePreparedMeshResidency(const MeshResidencyToken& token,
        FileSceneAssetCatalog& catalog, std::string* errorMessage) const;
    // Accepted transaction only: caller retains the freshly validated source
    // pin and proves matching renderer readiness, epochs and revision capacity.
    void ApplyPreparedMeshResidency() noexcept;
    bool CommitMeshResidency(const MeshResidencyToken& token, FileSceneAssetCatalog& catalog,
        std::string* errorMessage = nullptr);

    struct MeshResidencyPreparation
    {
        MeshResidencyToken token;
        SceneObjectMetadata selection;
        MeshResidencyCandidate candidate;

        FileSceneAssetCatalog* catalog = nullptr;
        Material* fallback = nullptr;

        std::map<std::string, Material*> originalSlots;
        std::map<std::string, Material*> runtimeSlots;
        std::map<std::string, Material*> committedSlots;
    };
    std::optional<MeshResidencyPreparation> m_meshResidency;

    std::uint64_t m_nextMeshResidency = 1;
    std::uint64_t m_residencyRevision = 0;

    struct MaterialAssignment
    {
        MaterialAssignmentToken token;

        SceneObjectMetadata selection;

        FileScene original;
        FileScene candidate;

        SceneDocument liveBefore;

        std::map<std::string, std::string> slotBindings;
        std::map<std::string, Material*> originalSlots;
        std::map<std::string, Material*> stagedRuntimeSlots;
        std::map<std::string, Material*> stagedCommittedSlots;
        std::map<std::string, Material*> expectedAppliedSlots;
        std::optional<MeshResidencyCandidate> pendingSlotSource;
        FileSceneAssetCatalog* pendingSlotCatalog = nullptr;

        std::optional<std::string> slotID;
        Mesh* expectedMesh = nullptr;
        Material* stagedMaterial = nullptr;

        Material* previousMaterial = nullptr;
        Material* appliedMaterial = nullptr;

        bool applied = false;
    };
    std::optional<MaterialAssignment> m_materialAssignment;

    std::uint64_t m_nextMaterialAssignment = 1;
    const std::uint64_t m_instanceIdentity;
    FileScene m_fileScene;

    Scene m_scene;

    SceneObjectMapping m_objectMapping;

    std::optional<PreparedScene> m_preparedScene;
    std::optional<PreparedMetadataScene> m_preparedMetadata;
};
