#pragma once

#include "Scene/FileScene.h"
#include "Renderer/EditorColliderOverlay.h"
#include "Scene/Scene.h"
#include "Scene/SceneCommit.h"
#include "Scene/PreparedScene.h"
#include "Scene/SceneHierarchySnapshot.h"
#include "Scene/SceneHierarchy.h"
#include "Scene/MeshResidency.h"

#include <optional>
#include <memory>
#include <span>
#include <string>
#include <map>
#include <thread>
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

namespace CookedScene { struct ArchiveScene; }
class Renderer;

enum class PhysicsPresentationReadiness { Ready, Retry, Stale, Failed };

// Identifies one accepted presentation, rather than just a compatible scene.
// Replacements issue a new revision, so an old owner cannot clear newer poses.
class PhysicsPresentationLease
{
    friend class FileSceneRuntime;
    std::uint64_t runtimeIdentity = 0;
    std::uint64_t rendererLifetime = 0;
    std::uint64_t revision = 0;
};

// Opaque, value-owned registration evidence. Capture with the exact document
// used to create a session; matching stable-ID strings alone are insufficient.
class PhysicsPresentationAssociation
{
    friend class FileSceneRuntime;
#if defined(ENGINE_HAS_VERIFICATION)
    friend class PresentationTransformVerification;
#endif
    std::uint64_t runtimeIdentity = 0;
    std::uint64_t structureRevision = 0;
    std::uint64_t rendererLifetime = 0;
    std::uint64_t materialBindingEpoch = 0;
    std::uint64_t materialTableRevision = 0;
    std::vector<SceneObjectMetadata> objects;
    std::vector<std::optional<Physics::PhysicsComponent>> components;
    std::vector<std::optional<Physics::TriggerColliderComponent>> triggerComponents;
    std::vector<std::optional<Physics::CharacterControllerComponent>> controllerComponents;
};

class FileSceneRuntime
{
public:
    FileSceneRuntime();
    std::optional<PhysicsPresentationAssociation> AssociatePhysicsPresentation(
        Renderer& renderer, const SceneDocument& registeredDocument, std::string& error) const;
    // Synchronous prepare/accept at the owning main-thread idle boundary.
    // Repeated apply replaces the complete snapshot. Failure preserves active state.
    bool ApplyPhysicsPresentation(Renderer& renderer, const PhysicsPresentationAssociation& association,
        const Physics::RuntimePoseSnapshot& snapshot, std::string& error,
        PhysicsPresentationLease* acceptedLease = nullptr);
    bool ClearPhysicsPresentation(Renderer& renderer, std::string& error);
    PhysicsPresentationReadiness CheckPhysicsPresentation(Renderer& renderer,
        const PhysicsPresentationAssociation& association, const PhysicsPresentationLease& lease, std::string& error) const;
    PhysicsPresentationReadiness ClearOwnedPhysicsPresentation(Renderer& renderer,
        const PhysicsPresentationLease& lease, std::string& error);
    bool HasMaterialAssignment() const noexcept
    {
        return m_materialAssignment.has_value();
    }

    std::uint64_t GetRuntimeIdentity() const noexcept
    {
        return m_instanceIdentity;
    }

    std::uint64_t GetHierarchyRevision() const noexcept
    {
        return m_hierarchy.GetRevision();
    }

    // Graph-dependent consumers call this before publishing hierarchy worlds. Fails when the
    // Scene's installed topology no longer matches this runtime's validated hierarchy state or
    // the current document's IDs/parents (for example after a raw EditDocument change).
    bool ValidateHierarchyAssociation(std::string* errorMessage = nullptr) const;

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
    // Character Controller diagnostics (CM.9) add ground/contact markers only when a snapshot
    // describes exactly the pose currently presented for that object; otherwise only capsules.
    bool CaptureEditorColliderOverlay(const Renderer&, EditorColliderOverlay&, std::string& error,
        std::span<const Physics::CharacterControllerDiagnostics> controllerDiagnostics = {}) const;
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

    // Stable-ID presentation read using the existing selection/binding identity.
    // Rejects stale reload, membership, index, and binding associations.
    std::optional<PresentationTransform> GetObjectPresentation(const SceneObjectMetadata& identity,
        std::string* errorMessage = nullptr) const;

    // Authored topology/order/local-only commit: preserves all runtime slots and GPU bindings.
    bool CommitHierarchyEdit(const SceneDocument& expected, SceneDocument replacement,
        std::string* errorMessage);
    bool ReorderObjects(const SceneObjectOrderIdentity& identity,
        const std::vector<std::string>& expectedOrder, const std::vector<std::string>& replacementOrder,
        std::string* errorMessage = nullptr);
    bool SetObjectPhysics(const SceneObjectMetadata& selection,
        const std::optional<Physics::PhysicsComponent>& expected,
        const std::optional<Physics::PhysicsComponent>& replacement, std::string* errorMessage = nullptr);
    bool SetObjectCharacterController(const SceneObjectMetadata& selection,
        const std::optional<Physics::CharacterControllerComponent>& expected,
        const std::optional<Physics::CharacterControllerComponent>& replacement, std::string* errorMessage = nullptr);
    bool SetObjectTriggerCollider(const SceneObjectMetadata& selection,
        const std::optional<Physics::TriggerColliderComponent>& expected,
        const std::optional<Physics::TriggerColliderComponent>& replacement, std::string* errorMessage = nullptr);
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
    bool LoadArchive(std::shared_ptr<const CookedScene::ArchiveScene> source, std::string* errorMessage = nullptr);
    bool IsArchiveActivation() const noexcept { return bool(m_archiveScene); }
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

    // Loose, editable document that has never been saved: no disk read and no path. Save
    // fails until SaveAs supplies a path; SaveAs then makes it an ordinary file scene.
    bool LoadUnsavedDocument(
        SceneDocument document,
        std::string* errorMessage = nullptr);

    SceneDocument& EditDocument();

    // Returns false when live state cannot be represented safely.
    // On failure, dirty is unchanged.
    bool CheckUnsavedChanges(
        bool& dirty,
        std::string* errorMessage = nullptr) const;

    bool Save(std::string* errorMessage = nullptr);

    bool SaveAs(
        const std::filesystem::path& path,
        std::string* errorMessage = nullptr,
        bool replaceExisting = true);

private:
    bool OwnsPhysicsPresentation(const Renderer& renderer, const PhysicsPresentationLease& lease) const noexcept;
    bool ValidatePhysicsPresentationAssociation(const Renderer& renderer,
        const PhysicsPresentationAssociation& association, std::string& error) const;
    PhysicsPresentationReadiness GetPhysicsPresentationBoundaryReadiness(const Renderer& renderer,
        std::string& error, bool clearing) const;
    std::uint64_t m_physicsPresentationRevision = 0;
    bool ValidatePhysicsPresentationBoundary(const Renderer& renderer, std::string& error, bool clearing = false) const;
    bool PreparePhysicsPresentation(const Physics::RuntimePoseSnapshot& snapshot, std::vector<std::optional<Physics::Pose>>& poses,
        std::string& error) const;
    const std::thread::id m_ownerThread = std::this_thread::get_id();
    std::optional<PhysicsPresentationAssociation> m_activePresentationAssociation;
    // Checked binding-only commits preserve stable object ownership and matrices.
    // Refresh internal evidence; previously issued external associations stay stale.
    void RevalidatePhysicsPresentationBindings(const Renderer& renderer) noexcept;
    std::shared_ptr<const CookedScene::ArchiveScene> m_archiveScene;
    // No coordinator exists in S.5B. Only the future matching renderer/DXR
    // transaction may call this at the main-thread pre-recording boundary.
    friend class SceneMembershipPublication;
    friend class MeshResidencyPublication;
    friend class SceneMaterialBindingPublication;
#if defined(ENGINE_HAS_VERIFICATION)
    friend class SceneMaterialBindingVerification;
#endif
    friend class Engine;
#if defined(ENGINE_HAS_VERIFICATION)
    friend class ScenePhysicsDataVerification;
    friend class PrimitiveCreationVerification;
    friend class MarqueeSelectionVerification;
    friend class PresentationTransformVerification;
    friend class SceneHierarchyVerification;

    // H.3 internal seam only. Production admission keeps rejecting nonempty parents until H.4.
    // Validates the candidate edges, evaluates every world, then commits document parents,
    // hierarchy state and Scene topology together, or changes nothing.
    bool CommitHierarchyForVerification(std::span<const std::string> parentIDs, std::string* errorMessage);
#endif
    // Advances `state` to `objects` and maps the forest onto commit-time slots, where slot i will
    // hold slotObjectIDs[i]. A flat forest yields an empty topology. Changes only its outputs.
    static bool PrepareCommitHierarchy(SceneHierarchy::State& state, std::span<const SceneObjectDocument> objects,
        std::span<const std::string_view> slotObjectIDs, SceneHierarchyTopology& topology, std::string* errorMessage);

    // Maps a validated forest onto runtime slots through the object-ID mapping.
    bool BuildHierarchyTopology(const SceneHierarchy::State& state, std::span<const SceneObjectDocument> objects,
        SceneHierarchyTopology& topology, std::string* errorMessage) const;

    // Only Engine archive bootstrap or an explicit diagnostic can open this runtime's gate.
    bool m_cookedPublicationAuthorized = false;
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
    SceneHierarchy::State m_hierarchy;

    Scene m_scene;

    SceneObjectMapping m_objectMapping;

    std::optional<PreparedScene> m_preparedScene;
    std::optional<PreparedMetadataScene> m_preparedMetadata;
};
