
#pragma once

#include "DemoContent/IDemoScene.h"
#include "Scene/FileSceneRuntime.h"
#include "Scene/FileSceneAssetCatalog.h"
#include "Scene/CompositeWorld.h"
#include "Scene/SceneEnvironmentResolution.h"

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
    // W4: a World opened as a read-only composite preview (CompositeWorld). Set before staging.
    explicit FileBackedDemoScene(std::shared_ptr<CompositeWorld> composite);
    const CompositeWorld* GetCompositeWorld() const noexcept { return m_compositeWorld.get(); }

    // W5: composite World editing. Dirty state and saves are per source document: index 0 is the World, i + 1 is levels[i].
    bool GetCompositeDirtyDocuments(std::vector<bool>& dirty, std::string* errorMessage = nullptr) const;
    // Writes the dirty documents (or only `onlyDocument`) in one transaction and advances their baselines; `saved` counts them.
    bool SaveCompositeDocuments(const ProjectPaths& paths, ProjectAssetRegistry& registry, std::optional<std::size_t> onlyDocument,
        std::size_t& saved, std::string* errorMessage = nullptr);
    // The level new entities join (the World or one of its Local Levels).
    const std::string& GetEditingLevel() const noexcept { return m_editingLevel; }
    bool SetEditingLevel(const std::string& levelID);
    // Gives a new entity its level namespace and default parent (see AssignCompositeEntity in the source).
    void AssignCompositeEntity(SceneObjectDocument& object) const;

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

    // S0.7B: metadata-first initialization in steps (open, catalog, preparation, commit), one per call, so staging an
    // inactive candidate can be spread over frames. Begin stores the borrowed renderer objects, which outlive staging.
    // Advance returns false on failure (the candidate must then be shut down) and sets `done` after the commit. The
    // candidate is private to its stager until done: nothing else reads or renders it.
    void BeginStagedInitialization(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& descriptorHeap,
        const Shader* vertexShader, const Shader* pixelShader);
    bool AdvanceStagedInitialization(bool& done);

    void SetProjectAssets(const std::filesystem::path& root, const ProjectAssetRegistry& registry);
    void SetMeshCPUCache(ProjectMeshCPUCache* cache) noexcept { m_meshCPUCache = cache; }
    void SetTextureStreamingEnabled(bool enabled) noexcept { m_textureStreamingEnabled = enabled; }
    void AdvanceTextureStreaming(FileSceneAssetCatalog::TextureStreamingPass pass, double expensiveBudget,
        bool forceExpensive, ProjectTexturePreparationService& preparation, Renderer& renderer,
        const std::function<bool(const AssetID&, const MaterialAssetData&)>& mayPublish);
    bool IsTextureStreamingSettled() const { return m_assets.IsTextureStreamingSettled(); }
    bool HasFullyStreamedMaterial() const { return m_assets.HasFullyStreamedMaterial(); }
    bool HasFullyTexturedResidentMesh() const { return m_assets.HasFullyTexturedResidentMesh(GetScene()); }

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
    // W2: the open document as edited (including unsaved edits); null when none is open. Read-only.
    const SceneDocument* GetSceneDocument() const noexcept
    {
        return m_runtime->GetFileScene().HasDocument() ? &m_runtime->GetFileScene().GetDocument() : nullptr;
    }
    // W2: the document Save would write now (live edits captured), without writing it.
    bool CaptureDocumentForSave(SceneDocument& captured, std::string* errorMessage = nullptr) const
    {
        return m_runtime->CaptureDocumentForSave(captured, errorMessage);
    }
    // W2: the open Local Level's owning World (asset ID), empty when unassigned.
    std::string_view GetOwningWorld() const noexcept
    {
        return m_runtime->GetFileScene().HasDocument() ?
            std::string_view(m_runtime->GetFileScene().GetDocument().owningWorld) : std::string_view{};
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
    // The runtime's retained snapshot (see FileSceneRuntime::GetHierarchySnapshot); null with the reason when unavailable.
    std::shared_ptr<const SceneHierarchySnapshot> GetHierarchySnapshot(std::string* errorMessage = nullptr) const;
    bool ReorderObjects(const SceneObjectOrderIdentity& identity,
        const std::vector<std::string>& expectedOrder, const std::vector<std::string>& replacementOrder,
        std::string* errorMessage = nullptr);
    // GetObjectMetadata(runtimeIndex) followed by that object's inspection capture, as one read.
    bool InspectSelectedObject(RenderObjectID runtimeIndex, SceneObjectMetadata& metadata,
        SceneObjectDocument& object) const
    {
        return m_initialized && m_runtime->InspectSelectedObject(runtimeIndex, metadata, object);
    }
    bool CapturePhysicsInspection(std::string_view objectID, SceneObjectDocument& object) const
    {
        return m_runtime->CaptureObjectInspection(objectID, object);
    }
    bool SetObjectPhysics(const SceneObjectMetadata& selection,
        const std::optional<Physics::PhysicsComponent>& expected,
        const std::optional<Physics::PhysicsComponent>& replacement, std::string* errorMessage = nullptr);
    bool CaptureTriggerColliderInspection(std::string_view objectID, SceneObjectDocument& object) const
    {
        return m_runtime->CaptureObjectInspection(objectID, object);
    }
    bool SetObjectTriggerCollider(const SceneObjectMetadata& selection,
        const std::optional<Physics::TriggerColliderComponent>& expected,
        const std::optional<Physics::TriggerColliderComponent>& replacement, std::string* errorMessage = nullptr);
    bool CaptureCharacterControllerInspection(std::string_view objectID, SceneObjectDocument& object) const
    {
        return m_runtime->CaptureObjectInspection(objectID, object);
    }
    bool SetObjectCharacterController(const SceneObjectMetadata& selection,
        const std::optional<Physics::CharacterControllerComponent>& expected,
        const std::optional<Physics::CharacterControllerComponent>& replacement, std::string* errorMessage = nullptr);
    bool CaptureCameraInspection(std::string_view objectID, SceneObjectDocument& object) const
    {
        return m_runtime->CaptureObjectInspection(objectID, object);
    }
    bool SetObjectCameras(std::span<const SceneCameraChange> changes, std::string* errorMessage = nullptr);
    bool CaptureLightInspection(std::string_view objectID, SceneObjectDocument& object) const
    {
        return m_runtime->CaptureObjectInspection(objectID, object);
    }
    bool SetObjectLight(const SceneObjectMetadata& selection, const std::optional<LightComponent>& expected,
        const std::optional<LightComponent>& replacement, std::string* errorMessage = nullptr);
    bool SetObjectAirMotor(const SceneObjectMetadata& selection, const std::optional<AirMotorComponent>& expected,
        const std::optional<AirMotorComponent>& replacement, std::string* errorMessage = nullptr);
    bool SetObjectMistEmitter(const SceneObjectMetadata& selection, const std::optional<MistEmitterComponent>& expected,
        const std::optional<MistEmitterComponent>& replacement, std::string* errorMessage = nullptr);
    bool SetObjectScript(const SceneObjectMetadata& selection, const std::optional<ScriptComponent>& expected,
        const std::optional<ScriptComponent>& replacement, std::string* errorMessage = nullptr);
    bool SetObjectAcoustics(const SceneObjectMetadata& selection, const std::optional<AcousticSettings>& expected,
        const std::optional<AcousticSettings>& replacement, std::string* errorMessage = nullptr);
    bool SetObjectProbeVolume(const SceneObjectMetadata& selection, const std::optional<ProbeVolumeComponent>& expected,
        const std::optional<ProbeVolumeComponent>& replacement, std::string* errorMessage = nullptr);
    bool SetObjectsVoxelSettings(const std::vector<FileSceneRuntime::VoxelSettingsChange>& changes, std::string* errorMessage = nullptr);
    bool SetSceneVoxelBake(const std::optional<VoxelBakeSettings>& expected, const std::optional<VoxelBakeSettings>& replacement,
        std::string* errorMessage = nullptr);
    // AIR.4F F4.4.1.
    bool SetSceneEnvironmentalHaze(const std::optional<EnvironmentalHazeSettings>& expected,
        const std::optional<EnvironmentalHazeSettings>& replacement, std::string* errorMessage = nullptr);
    // Physical sky S1: the open document's sky type and physical sky (the Sky window), as SetSceneEnvironmentalHaze.
    bool SetSceneSky(const SceneSky& expected, const SceneSky& replacement, std::string* errorMessage = nullptr);
    bool SetSceneAirEnvironment(const std::optional<AirEnvironmentSettings>& expected, const std::optional<AirEnvironmentSettings>& replacement,
        std::string* errorMessage = nullptr);
    // R1.1.
    bool SetSceneAirSimulation(const std::optional<AirSimulationSettings>& expected, const std::optional<AirSimulationSettings>& replacement,
        std::string* errorMessage = nullptr);
    bool SetSceneMistRendering(const std::optional<MistRenderSettings>& expected, const std::optional<MistRenderSettings>& replacement,
        std::string* errorMessage = nullptr);
    bool SetObjectAudio(const SceneObjectMetadata& selection, const AudioComponentState& expected,
        const AudioComponentState& replacement, std::string* errorMessage = nullptr);
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

    // W3 standalone environment preview. The owning World is read as a document (metadata only; its entities are never loaded),
    // and ResolveSceneEnvironment decides the effective environment. The runtime Scene shows the effective sky and exposure for
    // inherited groups; the document keeps the Local Level's own values (CaptureSceneDocument never writes inherited values).
    const EffectiveSceneEnvironment& GetEffectiveEnvironment() const noexcept { return m_effectiveEnvironment; }
    // Re-reads the owning World when asked, re-resolves and shows the result. Returns true when the effective sky reference changed
    // since the last publication (the caller then requests the environment through the S0.5 path).
    bool RefreshEnvironmentPreview(bool rereadWorld);
    // The environment request the Engine last published (keeps RefreshEnvironmentPreview from requesting it again).
    void NotePublishedEnvironment(const std::string& reference) { m_publishedEnvironmentReference = reference; }
    // Physical sky S0: the effective sky: a file scene's resolved Sky group, an archive activation's own cooked values (packaged
    // scenes are never resolved before W10).
    SkyType GetEffectiveSkyType() const noexcept;
    // The physical sky the renderer bakes: the effective one (its defaults when none is stored) when the effective type is
    // Physical, else none.
    std::optional<PhysicalSkySettings> GetEffectivePhysicalSky() const;
    // The environment the S0.5 path prepares for the effective sky: an Hdri sky's reference (empty: neutral, builtin:none:
    // black); the neutral environment (empty) for Neutral and Physical skies, whose HDRI reference stays stored but unused.
    std::string GetEnvironmentRequestReference() const;
    // The owning World has Directional Lights, which a standalone Local Level preview does not show (W4 decides World lighting).
    bool WorldHasDirectionalLights() const noexcept { return m_worldHasDirectionalLights; }

    enum class EnvironmentGroup
    {
        Sky,
        Exposure,
        Haze,
        Mist,
        AirEnvironment,
        Wind
    };

    static bool GetOverride(const SceneEnvironmentOverrides& overrides, EnvironmentGroup group);
    // Switches one override group of the open Local Level (marks it dirty). No stored value is lost: enabling sky or exposure shows
    // the Local Level's stored values again; disabling first stores its live values.
    bool SetEnvironmentOverride(EnvironmentGroup group, bool enabled, std::string* errorMessage = nullptr);

private:
    void ReadEnvironmentWorld();

    bool InitializeWithPolicy(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& descriptorHeap,
        const Shader* vertexShader, const Shader* pixelShader, bool metadataOnly);

    enum class StagingStep
    {
        Idle,
        Open,
        Assets,
        Prepare,
        Commit,
    };

    struct StagingInputs
    {
        ID3D12Device* device = nullptr;
        ResourceUploader* uploader = nullptr;
        DescriptorHeap* descriptorHeap = nullptr;
        const Shader* vertexShader = nullptr;
        const Shader* pixelShader = nullptr;
        bool metadataOnly = true;
    };

    StagingStep m_stagingStep = StagingStep::Idle;
    StagingInputs m_stagingInputs;

    friend class Editor;
    friend class Engine;
#if defined(ENGINE_HAS_VERIFICATION)
    friend class ProjectIndependentStartupVerification;
    friend class SceneLifecycleVerification;
    friend class AssetBrowserSceneVerification;
    friend class StartupSceneVerification;
    friend class SceneMembershipEditorVerification;
    friend class CompositeWorldEditorVerification;
    friend class DemoWorldEditorVerification;
    friend class CompositePlacementStress;
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
    friend class RenderingVerification;
    friend class GamePlayGPUVerification;
    friend class MemoryRegistryVerification;
    friend class AirPackagedParityVerification;
    friend class ScriptWorkflowVerification;
    friend class VoxelEditorVerification;
    friend class VoxelOverlayVerification;
    friend class VoxelStreamingVerification;
    friend class VoxelResidencyParityVerification;
    friend class AirMotorEditorVerification;
#endif
    // Engine retains both runtimes until checked candidate initialization/cleanup.
    std::unique_ptr<FileSceneRuntime> m_objectMutationCandidate;

    std::shared_ptr<const CookedScene::ArchiveScene> m_archiveScene;
    std::filesystem::path m_scenePath;

    // Source document of a never-saved scene; cleared once SaveAs adopts a file.
    std::optional<SceneDocument> m_unsavedDocument;
    std::shared_ptr<CompositeWorld> m_compositeWorld;
    std::string m_editingLevel;
    bool ProjectComposite(CompositeWorldProjection& projection, std::string* errorMessage) const;
    std::filesystem::path m_projectRoot;
    const ProjectAssetRegistry* m_registry = nullptr;

    // W3 environment preview state (see RefreshEnvironmentPreview).
    std::optional<SceneDocument> m_environmentWorld;
    std::string m_environmentWorldProblem;
    bool m_worldHasDirectionalLights = false;
    EffectiveSceneEnvironment m_effectiveEnvironment;
    // RefreshEnvironmentPreview's resolution input (environment fields only), kept to avoid per-frame allocations.
    SceneDocument m_environmentInput;
    std::string m_publishedEnvironmentReference;

    FileSceneAssetCatalog m_assets;

    ProjectMeshCPUCache* m_meshCPUCache = nullptr;

    std::unique_ptr<FileSceneRuntime> m_runtime = std::make_unique<FileSceneRuntime>();

    bool m_initialized = false;
    bool m_metadataOnlyActivation = false;
    bool m_textureStreamingEnabled = false;
};
