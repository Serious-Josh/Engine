
#pragma once

#include "DemoContent/IDemoScene.h"
#include "Scene/FileSceneRuntime.h"
#include "Scene/FileSceneAssetCatalog.h"

#include <filesystem>
#include <string>
#include <vector>

class FileBackedDemoScene final : public IDemoScene
{
public:
    explicit FileBackedDemoScene(std::filesystem::path scenePath);
    explicit FileBackedDemoScene(std::shared_ptr<const CookedScene::ArchiveScene> source);

    // Never-saved scene: the document is loaded from memory and the scene has no path until
    // SaveAs. Uses the same metadata-first preparation as a file scene.
    explicit FileBackedDemoScene(SceneDocument unsavedDocument);

    bool IsArchiveActivation() const noexcept { return bool(m_archiveScene); }

    // True for an editable scene that has never been written to a file.
    bool IsUnsaved() const noexcept { return !m_archiveScene && m_scenePath.empty(); }
    const std::shared_ptr<const CookedScene::ArchiveScene>& ArchiveSource() const noexcept { return m_archiveScene; }

    FileBackedDemoScene(const FileBackedDemoScene&) = delete;
    FileBackedDemoScene& operator=(const FileBackedDemoScene&) = delete;

    bool Initialize(
        ID3D12Device* device,
        ResourceUploader& uploader,
        DescriptorHeap& descriptorHeap,
        const Shader* vertexShader,
        const Shader* pixelShader) override;

    // Normal activation uses metadata preparation; the scheduler supplies geometry.
    bool IsMetadataOnlyActivation() const noexcept { return m_metadataOnlyActivation; }

    bool InitializeMetadataOnly(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& descriptorHeap,
        const Shader* vertexShader, const Shader* pixelShader);

#ifdef _DEBUG
    bool InitializeSynchronousDiagnostic(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& descriptorHeap,
        const Shader* vertexShader, const Shader* pixelShader);
#endif

    void SetProjectAssets(const std::filesystem::path& root, const ProjectAssetRegistry& registry);
    void SetMeshCPUCache(ProjectMeshCPUCache* cache) noexcept { m_meshCPUCache = cache; }
    void SetTextureStreamingEnabled(bool enabled) noexcept { m_textureStreamingEnabled = enabled; }
    void AdvanceTextureStreaming(ProjectTexturePreparationService& preparation, Renderer& renderer,
        const std::function<bool(const AssetID&, const MaterialAssetData&)>& mayPublish);

    const std::filesystem::path& GetScenePath() const noexcept
    {
        return m_scenePath;
    }

    const std::filesystem::path& GetProjectRoot() const noexcept { return m_projectRoot; }
    std::string_view GetPersistentSceneID() const noexcept
    {
        return m_runtime->GetFileScene().HasDocument() ?
            std::string_view(m_runtime->GetFileScene().GetDocument().id) : std::string_view{};
    }

    const ImportedMesh* GetMeshGeometry(const Mesh& mesh) const noexcept override
    {
        return m_assets.GetMeshGeometry(mesh);
    }

    bool PublishSavedMaterialParameters(const RuntimeMaterialVersions& expected, const MaterialAssetData& data,
        std::vector<GPUMaterial> table, const std::function<bool(const std::vector<GPUMaterial>&)>& publisher,
        std::string* errorMessage);
    SavedMaterialRevisionResult AdvanceSavedMaterialRevision(const RuntimeMaterialVersions& expected,
        const MaterialAssetData& data, const std::string& sourceIdentity, Renderer& renderer,
        const std::function<bool()>& sourceStillCurrent, std::string& error);
    void CancelSavedMaterialRevision();
    bool PublishWorkingMaterialParameters(const RuntimeMaterialVersions& expected,
        const std::optional<MaterialAssetData>& parameters, Renderer& renderer, std::string& error);
    const std::string& GetDisplayName() const noexcept;
    bool BuildHierarchySnapshot(SceneHierarchySnapshot& snapshot, std::string* errorMessage = nullptr) const;
    bool ReorderObjects(const SceneObjectOrderIdentity& identity,
        const std::vector<std::string>& expectedOrder, const std::vector<std::string>& replacementOrder,
        std::string* errorMessage = nullptr);
    bool CapturePhysicsInspection(std::string_view objectID, SceneObjectDocument& object) const
    {
        size_t index = 0;
        return m_runtime->CaptureObjectDeletionSnapshot(objectID, object, index);
    }
    bool SetObjectPhysics(const SceneObjectMetadata& selection,
        const std::optional<Physics::PhysicsComponent>& expected,
        const std::optional<Physics::PhysicsComponent>& replacement, std::string* errorMessage = nullptr);
    bool CaptureTriggerColliderInspection(std::string_view objectID, SceneObjectDocument& object) const
    {
        size_t index = 0;
        return m_runtime->CaptureObjectDeletionSnapshot(objectID, object, index);
    }
    bool SetObjectTriggerCollider(const SceneObjectMetadata& selection,
        const std::optional<Physics::TriggerColliderComponent>& expected,
        const std::optional<Physics::TriggerColliderComponent>& replacement, std::string* errorMessage = nullptr);
    bool CaptureCharacterControllerInspection(std::string_view objectID, SceneObjectDocument& object) const
    {
        size_t index = 0;
        return m_runtime->CaptureObjectDeletionSnapshot(objectID, object, index);
    }
    bool SetObjectCharacterController(const SceneObjectMetadata& selection,
        const std::optional<Physics::CharacterControllerComponent>& expected,
        const std::optional<Physics::CharacterControllerComponent>& replacement, std::string* errorMessage = nullptr);
    bool CaptureCameraInspection(std::string_view objectID, SceneObjectDocument& object) const
    {
        size_t index = 0;
        return m_runtime->CaptureObjectDeletionSnapshot(objectID, object, index);
    }
    bool SetObjectCameras(std::span<const SceneCameraChange> changes, std::string* errorMessage = nullptr);
    bool CaptureLightInspection(std::string_view objectID, SceneObjectDocument& object) const
    {
        size_t index = 0;
        return m_runtime->CaptureObjectDeletionSnapshot(objectID, object, index);
    }
    bool SetObjectLight(const SceneObjectMetadata& selection, const std::optional<LightComponent>& expected,
        const std::optional<LightComponent>& replacement, std::string* errorMessage = nullptr);
    bool ValidateObjectTransform(RenderObjectID index, const Transform& transform, std::string& error) const;
    bool RenameObject(const SceneObjectMetadata& selection, std::string_view expectedName,
        std::string name, std::string* errorMessage = nullptr,
        SceneObjectNamePolicy policy = SceneObjectNamePolicy::TrimmedInput);
    bool Save(std::string* errorMessage = nullptr);

    // Writes the presented scene to path and adopts it as the scene's file. Internal support
    // for saving an unsaved scene; no Save As UI exists yet.
    bool SaveAs(const std::filesystem::path& path, std::string* errorMessage = nullptr, bool replaceExisting = true);
    bool ClearPhysicsPresentation(Renderer& renderer, std::string& error)
    {
        return m_runtime->ClearPhysicsPresentation(renderer, error);
    }
    std::uint64_t GetRuntimeIdentity() const noexcept
    {
        return m_runtime->GetRuntimeIdentity();
    }

    std::optional<RuntimeMaterialVersions> GetMaterialVersions(
        const std::filesystem::path& canonicalProject, const AssetID& id,
        std::string* errorMessage = nullptr) const;

    std::optional<SceneObjectMetadata> GetObjectMetadata(
        RenderObjectID runtimeIndex,
        std::string* errorMessage = nullptr) const;
    std::optional<SceneObjectMetadata> GetObjectMetadata(
        std::string_view objectID, std::string* errorMessage = nullptr) const;
    std::optional<MaterialAssignmentToken> PrepareMaterialAssignment(
        const SceneObjectMetadata& selection, std::string materialReference,
        std::string* errorMessage = nullptr);
    std::optional<MaterialAssignmentToken> PrepareMaterialSlotAssignment(
        const SceneObjectMetadata& selection, std::string slotID, const std::optional<AssetID>& materialID,
        std::string* errorMessage = nullptr);
    Material* GetPreparedMaterialAssignmentMaterial(const MaterialAssignmentToken& token) const noexcept;
    // Load outside active recording; completed uploads remain owned by this scene.
    Material* ResolveAssignmentMaterial(std::string_view reference, std::string* errorMessage = nullptr);
    Material* ResolvePendingAssignmentMaterial(std::string_view reference, std::string* errorMessage);
    // Read-only admission query: never loads assets or grows renderer tables.
    bool IsAssignmentMaterialAdmitted(std::string_view reference, const Renderer& renderer);
    const MeshMetadata* GetPendingSlotMetadata(std::string_view reference) const
    {
        return m_assets.FindRetainedSlotMetadata(reference);
    }
    // Engine must establish Renderer/GPU safety first and retain old/new catalog assets.
    bool CommitMaterialAssignment(const MaterialAssignmentToken& token, Material& sceneOwnedMaterial,
        std::string* errorMessage = nullptr);
    bool RollbackMaterialAssignment(const MaterialAssignmentToken& token, std::string* errorMessage = nullptr);
    bool FinishMaterialAssignment(const MaterialAssignmentToken& token, std::string* errorMessage = nullptr);
    bool CancelMaterialAssignment(const MaterialAssignmentToken& token, std::string* errorMessage = nullptr);

    // Engine retains the inactive candidate and this catalog through checked GPU cleanup.
    std::unique_ptr<FileSceneRuntime> PrepareObjectRestoration(const SceneObjectDocument& object,
        size_t insertionIndex, std::string* errorMessage = nullptr);

    bool CheckUnsavedChanges(bool& dirty, std::string* errorMessage = nullptr) const;

    void Shutdown() override;
    void Update(float deltaTime) override;

    Scene& GetScene() override;
    const Scene& GetScene() const override;

    std::vector<Material*> GetMaterials() override;

    void ResetCamera() override;
    void ResetDirectionalLight() override;
    void SetAspectRatio(float aspectRatio) override;

private:
    bool InitializeWithPolicy(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& descriptorHeap,
        const Shader* vertexShader, const Shader* pixelShader, bool metadataOnly);

    friend class Editor;
    friend class Engine;
#if defined(ENGINE_HAS_VERIFICATION)
    friend class ProjectIndependentStartupVerification;
    friend class SceneLifecycleVerification;
    friend class AssetBrowserSceneVerification;
    friend class StartupSceneVerification;
    friend class ProjectSystemWorkflowVerification;
    friend class SceneMaterialBindingVerification;
    friend class SceneDuplicationVerification;
    friend class ScenePhysicsDataVerification;
    friend class PrimitiveCreationVerification;
    friend class MarqueeSelectionVerification;
    friend class SceneMembershipVerification;
    friend class SceneHierarchyVerification;
    friend class DescriptorRecoveryVerification;
    friend class ViewportMaterialDropVerification;
    friend class ResidencyBatchVerification;
#endif
    // Engine retains both runtimes until checked candidate initialization/cleanup.
    std::unique_ptr<FileSceneRuntime> m_objectMutationCandidate;

    std::shared_ptr<const CookedScene::ArchiveScene> m_archiveScene;
    std::filesystem::path m_scenePath;

    // Source document of a never-saved scene; cleared once SaveAs adopts a file.
    std::optional<SceneDocument> m_unsavedDocument;
    std::filesystem::path m_projectRoot;
    const ProjectAssetRegistry* m_registry = nullptr;

    FileSceneAssetCatalog m_assets;

    ProjectMeshCPUCache* m_meshCPUCache = nullptr;

    std::unique_ptr<FileSceneRuntime> m_runtime = std::make_unique<FileSceneRuntime>();

    bool m_initialized = false;
    bool m_metadataOnlyActivation = false;
    bool m_textureStreamingEnabled = false;
};
