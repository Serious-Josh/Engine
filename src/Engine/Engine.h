#pragma once

#include "Core/Time.h"
#include "Core/Input.h"

#include "DemoContent/DemoSceneManager.h"

#include "Platform/Windows/WindowsWindow.h"
#include "Platform/Windows/EditorFileDialogs.h"
#include "Platform/WindowProperties.h"

#include "Renderer/Renderer.h"

#include "Editor/Editor.h"

#include "assets/ProjectAssetRegistry.h"
#include "assets/ProjectTexturePreparationService.h"
#include "assets/ProjectMeshPreparationService.h"
#include "Scene/FileSceneRuntime.h"
#include "Scene/FileSceneAssetCatalog.h"
#include "Scene/MeshResidencyPublication.h"
#include "Scene/SceneMembershipPublication.h"
#include "Scene/SceneMaterialBindingPublication.h"
#include "Engine/PhysicsSimulationController.h"
#include "Project/ProjectContext.h"
#include "Project/ProjectLoader.h"
#include "Project/ProjectWorkspace.h"
#include "Editor/EditorPreferences.h"
#include "Editor/ProjectBrowser.h"
#include <chrono>

#include <cstdint>
#include <filesystem>
#include <functional>

// Uninitialized -> ProjectIndependent -> ProjectReady -> Running -> Stopped.
// Packaged runtime goes from ProjectIndependent straight to Running with its archive scene.
enum class EngineState
{
    Uninitialized,
    ProjectIndependent, // Window, renderer and UI exist; no project or scene.
    ProjectReady,       // Active project initialized; no scene yet.
    Running,            // A scene owns Renderer scene resources; the frame loop may run.
    Stopped
};

class FileBackedDemoScene;

struct EngineStartup
{
    enum class Mode { Editor, ArchiveDiagnostic, PackagedRuntime };
    Mode mode = Mode::Editor;
    std::filesystem::path toc, pak;
    std::optional<AssetID> sceneID;
    std::string gameTitle; // UTF-8; empty selects the diagnostic caption.
    unsigned int width=1280, height=720;

    // Ordinary interactive Editor launches only: load/save global preferences, the per-project
    // workspace and the relocated ImGui layout. Diagnostics leave it off so user settings never
    // change their behavior and they never write personal state.
    bool persistPersonalSettings = false;
    // Interactive startup only. Diagnostic callers retain the legacy DefaultProject/demo path.
    bool projectBrowserWorkflow = false;

    // Overrides the Editor settings directory (tests); empty selects EditorSettingsDirectory().
    std::filesystem::path personalSettingsDirectory;
};
class Engine
{
    public:
    // Archive diagnostics skip loose-project registry, reconciliation and scans.
    bool Initialize(bool archiveDiagnostic = false);

    // Interactive Editor: browser or explicit project + workspace scene. Diagnostic callers
    // retain the DefaultProject/Sphere Grid path. Packaged runtime: archive scene only.
    bool Initialize(const EngineStartup& startup);

    enum class InitialScene
    {
        SphereGridDemo,
        CleanUnsaved
    };

    // Individual phases, in order. Each returns false on failure; as with Initialize, the
    // caller then runs Shutdown. Packaged runtime uses only Initialize.
    bool InitializeProjectIndependent(const EngineStartup& startup);
    bool InitializeProject();
    bool InitializeInitialScene(InitialScene scene);

    EngineState GetState() const noexcept
    {
        return m_state;
    }

    bool HasActiveProject() const noexcept
    {
        return m_activeProject.has_value();
    }

    // No-project/no-scene message dispatch; interactive startup also draws the browser.
    void ProcessProjectIndependentFrame();

    // Replaces the active scene with a clean, never-saved scene through the normal scene
    // switch on a later frame. Rejected while operations are pending, physics is playing or
    // the active scene has unsaved changes (no prompt exists yet). Writes nothing.
    bool RequestCleanScene(std::string& errorMessage);

    // Recorded last edited scene of the active project, if it still exists inside the project.
    std::optional<std::filesystem::path> ResolveLastEditedScene(std::string& errorMessage) const;

    // The authoritative project for Editor and archive-diagnostic modes. Packaged runtime has
    // no loose project; calling this there is a logic error.
    const ProjectContext& GetActiveProject() const;

    // Editor mode only, before InitializeProject (before Initialize, or in the no-project
    // state): work in this validated, locked project instead of the default project. Engine
    // owns the lock until Shutdown or destruction.
    void SetStartupProject(OpenedProject project);
#if !defined(NDEBUG) && defined(ENGINE_HAS_VERIFICATION)
    friend class PortableEditorVerification;
    friend class ProjectIndependentStartupVerification;
    friend class EditorPreferencesVerification;
    friend class ProjectBrowserVerification;
    friend class SceneLifecycleVerification;
    friend class AssetBrowserSceneVerification;
    friend class StartupSceneVerification;
    friend class ProjectSystemWorkflowVerification;
    friend class ProjectRelaunchVerification;
    void VerifyPackagedBootstrap();
    // Preparation checkpoint only. Never enters the publication coordinator.
    bool VerifyCookedMeshGPUPreparation(const std::filesystem::path& scenePath,
        std::shared_ptr<const CookedMeshSource::Provider> source = {});
    bool VerifyCookedMeshPublication(const std::filesystem::path& scenePath, bool visualReview = false,
        std::shared_ptr<const CookedMeshSource::Provider> source = {});
    bool VerifyMaterialLocate();
    bool VerifyViewportPan();
    bool VerifyViewportGridSnap();
    bool VerifyEditorGrid();
    bool VerifySceneDuplication(const std::filesystem::path& scene);
    bool VerifySceneMembership(const std::filesystem::path& scene);
    // H.4 real-device hierarchy rendering diagnostic over a schema-9 project scene.
    bool VerifySceneHierarchyGPU(const std::filesystem::path& scene);
    // B.1 real-device descriptor exhaustion, reclaim, cancellation and recovery diagnostic.
    bool VerifyDescriptorRecovery(const std::filesystem::path& project);
    // B.2 real-device Asset Browser thumbnail scheduling, invalidation and pressure diagnostic.
    bool VerifyThumbnailScheduling(const std::filesystem::path& project);
    bool VerifyMeshImportUnitsGPU(const std::filesystem::path& scene);
    bool VerifyNormalMaps();
    bool VerifySceneHDRI(const std::filesystem::path& scene, const std::filesystem::path& output);
    bool VerifyBuildGame(const std::filesystem::path& scene, const std::filesystem::path& output);
    bool VerifyArchiveMaterials(const std::filesystem::path& sourceScene);
    bool VerifyCookedSceneArchive(const std::filesystem::path& sourceScene);
    bool ActivateCookedSceneArchive(const std::filesystem::path& toc, const std::filesystem::path& pak, const AssetID& sceneID);
    bool VerifyArchiveCookedMeshPublication(const std::filesystem::path& scenePath);
    bool VerifyMaterialBindingPublication();
#endif
    // Explicit main-thread capability; publishes at the next ready pre-Render boundary.
    bool PrepareMeshResidencyAcceptance(const MeshResidencyToken& token, std::string& error);
    // Main-thread only; drops coordination ownership, preserving GPU manager pins.
    void CancelMeshResidencyAcceptance() noexcept;
    // Last boundary outcome, including whether publication completed.
    const std::optional<MeshResidencyPublication::AcceptanceResult>& GetMeshResidencyAcceptance() const noexcept
    {
        return m_meshResidencyAcceptance;
    }

    void Run();
    void Shutdown();
    // Main-thread runtime command seam. Callers name the current session and exact
    // fixed step; input bindings and gameplay policy are outside Engine scheduling.
    const PhysicsSimulationController& GetPhysicsSimulation() const noexcept
    {
        return m_physicsSimulation;
    }

    bool SubmitCharacterMovement(std::uint64_t sessionGeneration, std::string_view objectID,
        std::uint64_t fixedStep, Vector3 velocity, std::string& error);
    bool SubmitCharacterJump(std::uint64_t sessionGeneration, std::string_view objectID,
        std::uint64_t fixedStep, std::string& error);
    // Main-thread texture preparation integration.
    ProjectTexturePreparationService& GetTexturePreparationService() noexcept
    {
        return m_texturePreparation;
    }
    // Main-thread mesh preparation and shared CPU cache integration.
    ProjectMeshPreparationService& GetMeshPreparationService() noexcept { return m_meshPreparation; }
    ProjectMeshCPUCache& GetMeshCPUCache() noexcept { return m_meshCPUCache; }
    // Main-thread, non-UI seam. Queues copied identity; executes at the next safe frame boundary.
    bool RequestMaterialAssignment(const SceneObjectMetadata& selection, std::string reference, std::string& error);
    bool RequestMaterialAssignment(RenderObjectID index, std::string reference, std::string& error);
    // Absent material removes only this slot's explicit override. Captures current object/mesh identity.
    bool RequestMaterialSlotAssignment(std::string_view objectID, std::string slotID,
        std::optional<AssetID> materialID, std::string& error);
    bool RequestMaterialSlotAssignment(const SceneObjectMetadata& selection, std::string slotID,
        std::optional<AssetID> materialID, std::string& error);

    // Main-thread API; queues for the next pre-recording frame boundary.
    // Default material is the catalog's builtin:default. Creation remains unsaved.
    bool RequestSceneObjectCreation(std::string meshReference, const Transform& transform,
        std::string& error, std::string materialReference = "builtin:default", std::string name = "Object");
    struct SceneObjectCreationResult
    {
        std::string objectID;
        std::string error;

        bool succeeded = false;
    };
    const std::optional<SceneObjectCreationResult>& GetLastSceneObjectCreationResult() const noexcept
    {
        return m_lastSceneObjectCreation;
    }

    // Main-thread API; captures current scene identity and executes at a safe frame boundary.
    // Deletes only an instance. Asset files/catalog allocations are never removed.
    bool RequestSceneObjectDeletion(std::string_view objectID, std::string& error);
    bool RequestSceneObjectDeletion(const SceneObjectMetadata& selection, std::string& error,
        std::optional<Editor::HistoryReplayIdentity> replay = std::nullopt);
    struct SceneObjectDeletionResult
    {
        std::uint64_t operationID = 0;
        std::uint64_t historyEpoch = 0;
        std::string objectID;
        std::string error;

        bool succeeded = false;
    };
    const std::optional<SceneObjectDeletionResult>& GetLastSceneObjectDeletionResult() const noexcept
    {
        return m_lastSceneObjectDeletion;
    }

    struct SceneObjectRestorationIdentity
    {
        std::filesystem::path projectIdentity;
        std::filesystem::path scenePath;
        std::string sceneID;

        std::uint64_t runtimeIdentity = 0;
        std::uint64_t structureRevision = 0;
        std::uint64_t historyEpoch = 0;
        bool operator==(const SceneObjectRestorationIdentity&) const = default;
    };
    // Capture at the call site; requests revalidate this identity at queue and execution time.
    std::optional<SceneObjectRestorationIdentity> GetSceneObjectRestorationIdentity() const;
    bool RequestSceneObjectRestoration(const SceneObjectDocument& object, size_t insertionIndex,
        const SceneObjectRestorationIdentity& identity, std::string& error, bool selectRestored = false,
        std::optional<Editor::HistoryReplayIdentity> replay = std::nullopt);
    struct SceneObjectRestorationResult
    {
        std::optional<Editor::HistoryReplayIdentity> historyReplay;
        std::string objectID;
        std::string error;
        std::optional<SceneObjectMetadata> restoredObject;
        std::optional<SceneObjectRestorationIdentity> publishedIdentity;

        size_t documentIndex = 0;
        bool succeeded = false;
    };
    const std::optional<SceneObjectRestorationResult>& GetLastSceneObjectRestorationResult() const noexcept
    {
        return m_lastSceneObjectRestoration;
    }

    private:
#if defined(ENGINE_HAS_VERIFICATION)
    friend class PresentationTransformVerification;
#endif
    PhysicsSimulationController m_physicsSimulation;
    std::uint64_t m_packagedPhysicsRuntimeIdentity = 0;
    bool StartPackagedPhysics(FileSceneRuntime& runtime, std::string& error);
    bool AdvancePackagedPhysics(FileSceneRuntime& runtime, double elapsedSeconds, std::string& error);
    std::string m_physicsMessage;
    bool CanStartEditorPhysics() const;
    void RefreshEditorPhysicsStatus(bool compatibleScene);
    void ProcessEditorPhysicsSimulation(FileSceneRuntime* runtime, double elapsedSeconds, bool compatibleScene);
    bool StopEditorPhysicsSimulation(FileSceneRuntime& runtime);
    bool DrainAndResolveEditorScene(IDemoScene& scene);
    bool PhysicsBlocksSceneEdits() const noexcept;
#if defined(ENGINE_HAS_VERIFICATION)
    friend class MaterialLocateVerification;
    friend class ViewportPanVerification;
    friend class ViewportGridSnapVerification;
    friend class EditorGridVerification;
#endif
    struct EditorMembershipOperation
    {
        Editor::HistoryReplayRequest request;
        SceneObjectRestorationIdentity identity;
        bool replay=false, prepared=false;
        std::optional<SceneObjectMetadata> deletion;
        std::optional<SceneObjectMetadata> selection;
        std::string sceneName;

        // Descriptor exhaustion gets one reclaim-and-retry per operation, then cancels.
        bool descriptorReclaimAttempted = false;
    };
    SceneMembershipPublication m_editorMembership;
    std::optional<EditorMembershipOperation> m_pendingEditorMembership;
    std::optional<EditorColliderOverlay> CaptureEditorColliderOverlayForFrame(const FileSceneRuntime*, bool editableScene) const;
    bool BeginPrimitiveCreation(const Editor::PrimitiveCreationRequest&, std::string& error);
    bool BeginEmptyObjectCreation(const Editor::EmptyObjectCreationRequest&, std::string& error);

    // Checked insertion of one new authored object at the end of the scene, selected after
    // creation, as one history entry. prepareObject builds it from the captured document.
    using PrepareCreatedObject = std::function<bool(const std::filesystem::path& project, const SceneDocument& document,
        SceneObjectDocument& object, std::string& error)>;
    bool BeginObjectCreation(std::uint64_t runtimeIdentity, std::uint64_t historyEpoch, const char* operationName,
        const PrepareCreatedObject& prepareObject, std::string& error);
    bool BeginEditorMembership(Editor::HistoryReplayRequest,bool replay,std::string& error,
        std::optional<SceneObjectMetadata> deletion = {});
    bool BeginEditorMembershipDeletion(const SceneObjectMetadata&,std::string&);
    bool AdvanceEditorMembership();
    void CancelEditorMembership(const std::string&);
    void PopulateArchiveScene(std::shared_ptr<const CookedScene::ArchiveScene>);
    bool ApplySceneEnvironment(std::string& error);
    bool AssignSceneEnvironment(size_t index, std::string& error);
    // Builds the game startup scene from its saved file (see ResolveBuildStartupScene). A dirty
    // active startup scene is refused, setting *startupSceneUnsaved, unless saveStartupScene asks
    // to save it first through the File > Save service; unsaved edits are never packaged.
    bool StartBuildGame(BuildGame::Settings, std::string&, bool saveStartupScene = false,
        bool* startupSceneUnsaved = nullptr);

    // The scene Build Game packages: the manifest's settings.startupScene when configured (an
    // invalid configured scene fails; there is no fallback), otherwise the active saved scene.
    struct BuildStartupScene
    {
        std::filesystem::path path;
        bool configured = false;

        // Set when the startup scene is the active editable scene, whose edits may be unsaved.
        FileBackedDemoScene* activeOwner = nullptr;
    };

    bool ResolveBuildStartupScene(BuildStartupScene& startup, std::string& error);

    // Validates scene as the active project's game startup scene (an empty path clears the
    // setting) and persists it to the locked manifest; the context changes only after success.
    bool SetProjectStartupScene(const std::filesystem::path& scene, std::string& message);

    // Fail-closed state after full manifest ownership (lock + verified pin) could not be confirmed.
    // Project writes, scene lifecycle, builds and file-drop imports are refused; the Editor shows a
    // blocking prompt offering only Retry (re-verify ownership) or Close (controlled shutdown that
    // skips project writes). Other Editors stay excluded by the byte-range lock throughout.
    bool m_projectOwnershipLost = false;
    void FailClosedProjectOwnership(const std::string& reason);
    void ProcessProjectOwnership();

    // Project containment and existence rules shared with File > Open/Save Scene.
    bool ResolveEditorSceneLocation(const std::filesystem::path& requested, bool mustExist,
        std::filesystem::path& resolved, std::string& error) const;
    BuildGame::Service m_buildGame;
    std::string m_buildGameLastMessage="Ready.";
    EngineStartup m_startup;

    enum class SceneSource
    {
        Archive,
        SphereGridDemo,
        CleanUnsaved,
        SavedFile
    };

    bool InitializeScene(SceneSource source);
    bool InitializeProjectScene();
    void ProcessProjectBrowserRequest(const ProjectBrowser::Request& request);
    ProjectBrowser m_projectBrowser;
    std::filesystem::path m_initialScenePath;
    std::string m_projectNotice;

    // PR.5 personal state. Inactive unless m_startup.persistPersonalSettings. The *Saved strings
    // hold the last loaded or written serialization so unchanged state is never rewritten.
    void LoadEditorPreferences();
    void ConfigureImGuiSettingsLocation();
    void LoadProjectWorkspace();
    void RecordRecentProject();
    void RecordLastEditedScene();
    void SaveEditorPreferences();
    void SaveProjectWorkspace();

    bool m_personalSettingsActive = false;
    std::filesystem::path m_personalSettingsDirectory;
    EditorPreferences m_editorPreferences;
    PersonalSettingsFile::Status m_editorPreferencesStatus = PersonalSettingsFile::Status::Missing;
    std::string m_editorPreferencesSaved;
    std::optional<ProjectWorkspace> m_projectWorkspace;
    PersonalSettingsFile::Status m_projectWorkspaceStatus = PersonalSettingsFile::Status::Missing;
    std::string m_projectWorkspaceSaved;

    // ImGui keeps this pointer until its context is destroyed in Renderer shutdown.
    std::string m_imguiSettingsPath;

    // Startup data carried between phases. The archive is released once its scene is active.
    std::chrono::steady_clock::time_point m_initializationStarted;
    std::shared_ptr<const CookedScene::ArchiveScene> m_startupArchive;
    bool m_rendererAttempted = false;
    bool m_windowInitialized = false;
#if !defined(NDEBUG) && defined(ENGINE_HAS_VERIFICATION)
    bool VerifyCookedMeshLifecycle(const std::filesystem::path& scenePath);
#endif
    void ProcessMeshResidencyDiagnostics();
    void RequestDiagnosticMeshResidency();
    void AdvanceDiagnosticMeshPreparation();
    bool BeginMeshResidencyRequest(const SceneObjectMetadata& selection, bool automatic);
    void StartAutomaticMeshResidency();
    void OnFileSceneActivated(std::chrono::steady_clock::time_point started);
    void ContinueAutomaticMeshResidencyAfterReplacement(std::uint64_t previousRuntimeIdentity) noexcept;
    void AdvanceAutomaticMeshResidency();
    void FinishAutomaticMeshResidency(MeshResidencyPublication::Status status);
    void RejectMeshResidencyRequest(const std::string& message,
        MeshResidencyPublication::Status status = MeshResidencyPublication::Status::Failed);
    std::string GetMeshSourceIdentity(const SceneObjectMetadata& selection) const;

    struct AutomaticMeshResidency
    {
        struct Failure
        {
            SceneObjectMetadata selection;
            std::string sourceIdentity;
        };

        std::vector<Failure> failures; // At most one current failure per runtime object; max 4096.
        std::uint64_t runtimeIdentity = 0;
        std::uint64_t structureRevision = 0;
        size_t cursor = 0;
        size_t eligible = 0;
        size_t published = 0;
        size_t initiallyResident = 0;
        size_t stale = 0;
        bool enabled = false;
        bool settled = false;
        std::chrono::steady_clock::time_point activated;
        std::chrono::steady_clock::time_point retryAfter;
        std::string backpressure;
    };

    AutomaticMeshResidency m_automaticMeshResidency;
    // Automatic scheduling intent is scoped to a successfully activated runtime,
    // independent of its cursor, cooldowns and transient request handles.
    std::uint64_t m_automaticResidencyIntent = 0;
    std::optional<SceneObjectMetadata> m_residencyRequestSelection;
    std::string m_residencyRequestSourceIdentity;
    bool m_residencyRequestAutomatic = false;
    struct DiagnosticMeshPreparation
    {
        SceneObjectMetadata selection;
        std::uint64_t ticket = 0;
        std::uint64_t generation = 0;
        std::optional<ProjectMeshPreparationService::Completion> completion;
        bool cooked = false;
    };

    std::optional<DiagnosticMeshPreparation> m_diagnosticCPUPreparation;
    std::uint64_t m_cookedPublicationTicket = 0;
    void LogMeshResidencyDiagnostic(const MeshResidencyPublication::AcceptanceResult& result);

    std::optional<MeshResidencyToken> m_diagnosticMeshToken;
    std::optional<MeshResidencyPublication::AcceptanceResult> m_diagnosticMeshReadiness;
    std::uint64_t m_diagnosticRendererLifetime = 0;

    MeshResidencyPublication m_meshResidencyPublication;
    std::optional<MeshResidencyPublication::AcceptanceResult> m_meshResidencyAcceptance;

    bool ExecuteSceneObjectCreation();
    bool ExecuteSceneObjectDeletion();
    bool ExecuteSceneObjectRestoration();
    struct SceneObjectRestorationRequest
    {
        SceneObjectDocument object;
        SceneObjectRestorationIdentity identity;
        size_t insertionIndex = 0;
        bool selectRestored = false;
        std::optional<Editor::HistoryReplayIdentity> historyReplay;
    };
    std::optional<SceneObjectRestorationRequest> m_pendingSceneObjectRestoration;
    std::optional<SceneObjectRestorationResult> m_lastSceneObjectRestoration;

    struct SceneObjectDeletionRequest
    {
        std::filesystem::path projectIdentity;
        SceneObjectMetadata selection;
        std::uint64_t operationID = 0;
        std::uint64_t historyEpoch = 0;
        std::optional<Editor::HistoryReplayIdentity> historyReplay;
    };
    std::optional<SceneObjectDeletionRequest> m_pendingSceneObjectDeletion;
    std::optional<SceneObjectDeletionResult> m_lastSceneObjectDeletion;

    struct SceneObjectCreationRequest
    {
        std::filesystem::path projectIdentity;

        std::string meshReference;
        std::string materialReference;
        std::string name;

        Transform transform;

        std::uint64_t runtimeIdentity = 0;
        std::uint64_t structureRevision = 0;
    };
    std::optional<SceneObjectCreationRequest> m_pendingSceneObjectCreation;
    std::optional<SceneObjectCreationResult> m_lastSceneObjectCreation;

    void Frame();
    // Continuation gates preserve the existing fatal-return boundaries in Frame.
    bool DeliverFrameDisplacementCompletion(bool& completionDelivered);
    bool ExecutePendingEditorMutations();
    // Immediate pre-recording acceptance; request outcomes do not decide frame continuation.
    void AcceptPreparedMeshResidencyForFrame(IDemoScene& activeDemo);
    bool ActivateRequestedFileScene();
    bool ExecuteMaterialAssignment();
    struct MaterialAssignmentRequest
    {
        SceneObjectMetadata selection;

        std::string reference;
        std::optional<std::string> slotID;
        std::optional<AssetID> slotMaterial;
        bool requireEditorSelection = false;
    };
    bool QueueMaterialAssignment(MaterialAssignmentRequest request, std::string& error);
    std::optional<MaterialAssignmentRequest> m_pendingMaterialAssignment;
    void ProcessEditorSceneRequests(IDemoScene& activeScene);
    void ProcessEditorMaterialRequests();
    bool ChangeMeshImportUnits(const ProjectPaths& paths, const AssetBrowser::MeshImportUnitsRequest& request,
        const FileBackedDemoScene* activeScene, std::string& error);
    void ProcessSavedMaterialRevisions();
    void ProcessSceneTextureStreaming();
    void ProcessWorkingMaterialNotifications();
    void PublishWorkingMaterialParameters();
    struct WorkingMaterialOverride
    {
        Editor::SceneMaterialWorkingRevision selection;

        std::optional<MaterialAssetData> parameters;
        std::optional<MaterialAssetData> lastPublished;

        std::uint64_t promotionSequence = 0;
        std::uint64_t lastTableRevision = 0;
    };
    std::optional<WorkingMaterialOverride> m_workingMaterialOverride;

    std::vector<Editor::SceneMaterialWorkingRevision> m_pendingWorkingMaterialRevisions;

    std::vector<RuntimeMaterialVersions> m_workingMaterialRestores;

    std::uint64_t m_lastWorkingMaterialSequence = 0;

    std::vector<Editor::SceneMaterialSavedRevision> m_pendingSavedMaterialRevisions;
    std::vector<Editor::SceneMaterialSavedRevision> m_savedMaterialRevisionQueue;
    // Successfully propagated saved revisions, scoped to this runtime/catalog instance.
    struct SavedMaterialObservation
    {
        MaterialSavedRevision saved;

        RuntimeMaterialVersions runtime;
    };
    std::vector<SavedMaterialObservation> m_savedMaterialObservations;

    void QueueEditorSceneOperation();

    WindowProperties m_windowProperties;

    WindowsWindow m_window;

    Time m_time;

    Input m_input;

    Renderer m_renderer;

#if defined(ENGINE_HAS_VERIFICATION)
    friend class SceneDuplicationVerification;
    friend class SceneMembershipVerification;
    friend class SceneHierarchyVerification;
    friend class DescriptorRecoveryVerification;
    friend class ThumbnailSchedulingVerification;
    friend class ViewportMaterialDropVerification;
#endif
    struct PendingViewportMaterialBinding
    {
        Editor::MaterialBindingRequest request;
        std::chrono::steady_clock::time_point started;
        bool prepared = false;
        const ProjectAssetRegistry* registry = nullptr;
        std::filesystem::path project;
        std::uint64_t rendererIdentity = 0;
        std::uint64_t preparationTicket = 0;
        std::shared_ptr<const ProjectMaterialPreparationService::Candidate> candidate;
        bool admissionStarted = false;
        bool materialReady = false;
        std::chrono::steady_clock::time_point retryAfter{}, progressAfter{};
    };
    SceneMaterialBindingPublication m_viewportMaterialPublication;
    std::optional<PendingViewportMaterialBinding> m_pendingViewportMaterialBinding;
    void ProcessViewportMaterialAssignment(IDemoScene& scene);
    void CancelViewportMaterialAssignment(std::string_view reason) noexcept;
    void ReleaseViewportMaterialPreparation() noexcept;
    bool PrepareViewportDropMaterial(FileBackedDemoScene& owner);

    std::optional<Renderer::FrameRenderResult> m_diagnosticRenderOutcome;

    // Declared before scene owners so cache lifetime includes catalog teardown.
    ProjectMeshCPUCache m_meshCPUCache;

    DemoSceneManager m_demoSceneManager;

    uint64_t m_renderedSceneStructureRevision = 0;

    Shader* m_demoVertexShader = nullptr;
    Shader* m_demoPixelShader = nullptr;

    Editor m_editor;

    std::optional<Editor::FileRequest> m_pendingEditorSceneOperation;

    std::string m_sceneOperationError;
    bool m_sceneDecisionPending = false;
    bool m_projectCreationPending = false;
    bool m_editorRelaunchStarted = false;
    bool CanPerformSceneLifecycle(std::string& error) const;
    enum class SceneSaveResult { Saved, Canceled, Failed };
    SceneSaveResult SaveEditorScene(FileBackedDemoScene& scene, bool saveAs,
        const std::filesystem::path& selectedPath = {}, bool overwriteConfirmed = false);
    void ResolveSceneChangeDecision(Editor::SceneChangeDecision decision);
    void ContinueEditorSceneOperation();
    void RelaunchEditorProject(const std::filesystem::path& manifest, bool created = false);

    // A replacement Editor that was created but has not yet shown that it opened its project. This
    // Editor stays open (lifecycle commands refused) until the child shows its window (then this
    // Editor closes), exits (failure reported, this Editor stays usable) or the wait times out.
    struct PendingRelaunch
    {
        EditorFileDialogs::ChildEditor child;
        std::string prefix;
        std::chrono::steady_clock::time_point started;
    };

    std::optional<PendingRelaunch> m_pendingRelaunch;
    void ProcessPendingRelaunch();
    void CompleteRelaunch();
#if defined(ENGINE_HAS_VERIFICATION)
    std::function<std::optional<std::filesystem::path>(bool)> m_sceneDialogForVerification;
    std::function<bool(const std::filesystem::path&)> m_overwriteForVerification;
    std::function<bool(const std::filesystem::path&, std::string&)> m_relaunchForVerification;
    // Runs after the target preflight lock is released and before the real or injected launch.
    std::function<void(const std::filesystem::path&)> m_beforeRelaunchForVerification;
#endif

    EngineState m_state = EngineState::Uninitialized;

    bool m_sceneResourcesReleasedForSwitch = false;

    // Set once during Initialize and never replaced: Editor and scene owners keep references to
    // it, and project switching is not supported yet. Empty in packaged runtime.
    std::optional<ProjectContext> m_activeProject;

    // Held for explicitly opened projects; empty for the legacy DefaultProject (see PR.3 report).
    ProjectLock m_projectLock;

    ProjectAssetRegistry m_projectAssetRegistry;

    ProjectTexturePreparationService m_texturePreparation;
    std::unique_ptr<ProjectMaterialPreparationService> m_materialPreparation;
    ProjectMeshPreparationService m_meshPreparation;
};
