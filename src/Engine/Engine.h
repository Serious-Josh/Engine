#pragma once

#include "Core/Time.h"
#include "Core/Input.h"

#include "DemoContent/DemoSceneManager.h"

#include "Platform/Windows/WindowsWindow.h"
#include "Platform/Windows/EditorFileDialogs.h"
#include "Platform/WindowProperties.h"

#include "Renderer/Renderer.h"

#include "Editor/Editor.h"
#include "Engine/DxrPreparationTasks.h"

#include "assets/ProjectAssetRegistry.h"
#include "assets/ProjectTexturePreparationService.h"
#include "assets/ProjectMeshPreparationService.h"
#include "Scene/FileSceneRuntime.h"
#include "Scene/FileSceneAssetCatalog.h"
#include "Scene/MeshResidencyPublication.h"
#include "Scene/SceneMembershipPublication.h"
#include "Scene/SceneMaterialBindingPublication.h"
#include "Engine/PhysicsSimulationController.h"
#include "Audio/AudioSystem.h"
#include "Engine/GameRuntime.h"
#include "Engine/VoxelResidencyController.h"
#include "Project/ProjectContext.h"
#include "Project/ProjectLoader.h"
#include "Project/ProjectWorkspace.h"
#include "Editor/EditorPreferences.h"
#include "Editor/ProjectBrowser.h"
#include <array>
#include <chrono>

#include <cstdint>
#include <filesystem>
#include <limits>
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
    friend class ProjectIndependentStartupVerification;
    friend class EditorPreferencesVerification;
    friend class ProjectBrowserVerification;
    friend class SceneLifecycleVerification;
    friend class AssetBrowserSceneVerification;
    friend class StartupSceneVerification;
    friend class SceneMembershipEditorVerification;
    friend class CompositeWorldEditorVerification;
    friend class DemoWorldEditorVerification;
    friend class CompositePlacementStress;
    friend class RenderingVerification;
    friend class GamePlayGPUVerification;
    friend class AirPackagedParityVerification;
    friend class MemoryRegistryVerification;
    friend class ScriptWorkflowVerification;
    friend class VoxelEditorVerification;
    friend class VoxelOverlayVerification;
    friend class VoxelStreamingVerification;
    friend class AirMotorEditorVerification;
    friend class AirFieldVerification;
    friend class VoxelResidencyVerification;
    friend class VoxelQueryVerification;
    friend class VoxelResidencyParityVerification;
    friend class ProjectSystemWorkflowVerification;
    friend class ProjectRelaunchVerification;
    void VerifyPackagedBootstrap();
    // R1.4b: after VerifyPackagedBootstrap, with ENGINE_DIAG_PACKAGED_VOXEL_RESIDENCY naming the Editor run's digest file.
    void VerifyPackagedVoxelResidency();
    // R1.5b: with ENGINE_DIAG_PACKAGED_AIR naming the Editor run's AIR parity digest, before VerifyPackagedBootstrap.
    void VerifyPackagedAirParity();
    void VerifyPackagedReflections();
    void VerifyPackagedProbes();
    void VerifyPackagedPathing();
    // Preparation checkpoint only. Never enters the publication coordinator.
    bool VerifyCookedMeshGPUPreparation(const std::filesystem::path& scenePath,
        std::shared_ptr<const CookedMeshSource::Provider> source = {});
    bool VerifyCookedMeshPublication(const std::filesystem::path& scenePath, bool visualReview = false,
        std::shared_ptr<const CookedMeshSource::Provider> source = {});
    bool VerifyMaterialLocate();
    bool VerifyViewportPan();
    bool VerifyViewportGridSnap();
    bool VerifyEditorGrid();
    bool VerifyVoxelResidency();
    bool VerifyBackgroundTasks();
    bool VerifyDxrBackgroundTasks();
    bool VerifyDxrBackgroundTaskFailure();
    // Interactive: runs the Editor with representative fake tasks for manual UI review.
    bool RunBackgroundTasksDemo();
    bool VerifySceneDuplication(const std::filesystem::path& scene);
    bool VerifySceneMembership(const std::filesystem::path& scene);
    // Phase 2 multi-object residency transactions over a self-created fixture project.
    bool VerifyResidencyBatches();
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
    // Holds plain data only. Declared ahead of every system that uses it, so it is destroyed after all of them.
    BackgroundTaskManager m_backgroundTasks;
    // BT.3: Editor frames only. The activity structs are reused so polling the workers does not allocate.
    DxrPreparationTasks m_dxrPreparationTasks;
    // M0c.4 Stage 4.3: the pending rendering configuration as one Background Task (its DXR jobs are DxrPreparationTasks').
    BackgroundTaskHandle m_configurationPreparationTask;
    std::uint64_t m_configurationPreparationTaskGeneration = 0;
    std::uint64_t m_configurationPreparationTaskSwitches = 0;
    void UpdateConfigurationPreparationPresentation();
    AsyncWorkerActivity m_dxrLibraryActivity;
    AsyncWorkerActivity m_dxrPipelineActivity;
    PhysicsSimulationController m_physicsSimulation;
    // AIR.4F.3 (diagnostic overlay): the physics session whose movable bodies were opted in as moving solids.
    std::uint64_t m_movingSolidOptInGeneration = 0;
    // Shared by Editor Play and the packaged runtime; borrows m_physicsSimulation.
    GameRuntime m_gameRuntime{m_physicsSimulation};
    // The process's only FMOD Studio system (Audio.1A): started with the project-independent services, updated once
    // per frame, released in Shutdown after gameplay stops.
    Audio::AudioSystem m_audio;
    // Audio.1B: discovers the project's FMOD Studio banks and loads every available one for the project's (= the
    // process's) lifetime; AudioSystem::Shutdown unloads them. Failures are logged and never block project opening.
    void LoadProjectAudioBanks(const ProjectPaths& paths);
    // Audio.1D: packaged games load every bank the archive lists from Game.eap through FMOD's copying memory path,
    // before gameplay starts. A missing list means no banks; a damaged list or bank is logged and skipped (nonfatal).
    void LoadPackagedAudioBanks();
    bool StartPackagedGame(FileSceneRuntime& runtime, std::string& error);
    bool AdvancePackagedGame(FileSceneRuntime& runtime, double elapsedSeconds, std::string& error);
    // AS.6B cursor lock policy shared by both hosts. A script's lock request is granted over
    // `area` (client pixels; nullopt while gameplay lacks focus) unless Home or window focus
    // loss released it; only a click on the game view (`areaClicked`) clears that release, so
    // repeated script requests cannot. Returns whether the cursor is captured this frame.
    // Without an active session it releases everything.
    bool UpdateGameCursor(const std::optional<WindowsWindow::CursorRectangle>& area, bool areaClicked);
    bool m_gameCursorReleased = false;
    // Editor Play host: toolbar requests, edit locks and the viewport's gameplay view.
    std::string m_gameMessage;
    bool m_gameStopping = false;
    bool IsEditorGameOwningSimulation() const noexcept;
    bool IsEditorSceneStreamingSettled() const;
    void RefreshEditorGameStatus(bool compatibleScene);
    void ProcessEditorGamePlay(FileSceneRuntime* runtime, double elapsedSeconds, bool compatibleScene,
        std::uint32_t viewWidth, std::uint32_t viewHeight);
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
    friend class BackgroundTasksVerification;
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
    void ReportCameraFrustumOmissions(const std::vector<std::string>& rejected) const;
    // Last logged set of omitted Camera frusta; display bookkeeping only.
    mutable std::string m_cameraFrustumOmissions;
    // LIGHT.1: this frame's lighting snapshot for any file-backed scene (Editor or packaged).
    // A failed capture yields a dark, component-free snapshot rather than the legacy sun.
    SceneLightingSnapshot CaptureSceneLightingForFrame(const FileSceneRuntime& runtime) const;
    // M3.5: the frame's snapshot, shared and reused while unchanged (FileSceneRuntime::CaptureSceneLightingShared).
    std::shared_ptr<const SceneLightingSnapshot> CaptureSharedSceneLightingForFrame(const FileSceneRuntime& runtime) const;
    // M3.5 ENGINE_DIAG_LIGHT_PUBLICATION_VERIFY (Debug): cached-snapshot checks made and differences found.
    mutable std::uint64_t m_lightPublicationVerifications = 0;
    mutable std::uint64_t m_lightPublicationMismatches = 0;
    // Bounded diagnostics, logged only when the named set changes, never once per frame.
    void ReportLightOmissions(const std::vector<std::string>& rejected, const char* context,
        std::string& lastReported) const;
    mutable std::string m_lightingOmissions;
    mutable std::string m_lightWireframeOmissions;
    mutable std::string m_lightingCaptureError;
    bool BeginPrimitiveCreation(const Editor::PrimitiveCreationRequest&, std::string& error);
    bool BeginEmptyObjectCreation(const Editor::EmptyObjectCreationRequest&, std::string& error);
    bool BeginCameraObjectCreation(const Editor::CameraObjectCreationRequest&, std::string& error);
    bool BeginLightObjectCreation(const Editor::LightObjectCreationRequest&, std::string& error);
    bool BeginProbeVolumeObjectCreation(const Editor::ProbeVolumeObjectCreationRequest&, std::string& error);
    // CAM.4 packaged camera policy: whether StartPackagedGame requires a Main Camera.
    bool AdmitPackagedMainCamera(const FileSceneRuntime& runtime, std::uint32_t payloadVersion, std::string& error);
    bool m_packagedMainCamera = false;

    // PS.4: the packaged game's rendering configuration, resolved once in preflight from its cooked Project
    // Settings (no session overrides), and the last requested/effective lighting report logged.
    RenderingConfiguration m_packagedRenderingConfiguration;
    ProjectLightingMode m_packagedRequestedLightingMode = ProjectLightingMode::Legacy;
    std::string m_packagedLightingReport;
    bool m_packagedFirstPathTraceImageLogged = false;
    void ReportPackagedLightingMode(double renderMilliseconds);
    // R1.1: resolves the active archive scene's cooked AIR configuration into m_airConfiguration and reports, once per scene, that it
    // requests simulation. R1.5b: the packaged frame then requests AIR when PreparePackagedAir initialized it.
    void ResolvePackagedAirConfiguration(const FileBackedDemoScene& scene);
    // R1.2b/R1.5b: the packaged frame's fog request: the haze, and the AIR mist while packaged AIR runs, both through the Original
    // Raymarch (RestrictToPackagedVolumetrics); nullopt with neither.
    std::optional<AirMistRenderRequest> BuildPackagedFogRequest(const FileBackedDemoScene& scene) const;

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
    // S0.5: requests the active scene's environment asynchronously (see SceneEnvironment.cpp); PollSceneEnvironment
    // observes the request once per frame. m_sceneEnvironmentTicket is 0 when no request is observed.
    bool ApplySceneEnvironment(std::string& error, bool activation = false);
    void PollSceneEnvironment();
    std::uint64_t m_sceneEnvironmentTicket = 0;
    bool m_sceneEnvironmentActivation = false;
    float ActiveEnvironmentRadianceScale() const;
    void RefreshSceneEnvironmentCalibration();
    // The HDRI calibration ApplySceneEnvironment last applied (NaN before the first).
    float m_appliedEnvironmentRadianceScale = std::numeric_limits<float>::quiet_NaN();
    // Set on scene activation; the next frame's automatic exposure snaps to its target.
    bool m_exposureResetPending = true;
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

    // PS.1 explicit save of the active project's ProjectSettings.json, under the same Editor, lock
    // and ownership rules as the startup scene. Nothing else writes the file.
    bool SaveProjectSettings(const ProjectSettings& settings, std::string& message);

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
    // failedObjectID names the batch entry responsible for a rejection, when known.
    void FinishAutomaticMeshResidency(MeshResidencyPublication::Status status, std::string_view failedObjectID = {});
    void RejectMeshResidencyRequest(const std::string& message,
        MeshResidencyPublication::Status status = MeshResidencyPublication::Status::Failed,
        std::string_view failedObjectID = {});
    std::optional<MeshResidencyCandidate> DescribeSynchronousMeshCandidate(FileBackedDemoScene& scene,
        const SceneObjectMetadata& selection);
    void SubmitAutomaticResidencyBatch(FileBackedDemoScene& scene,
        const std::vector<FileSceneRuntime::MeshResidencyRequest>& batch);
    bool IsTerminalAutomaticFailure(const SceneObjectMetadata& selection) const;
    // Whether every acoustic mesh of the scene has geometry to bake from: nullopt when ready (including when nothing is acoustic),
    // otherwise why not. Read-only, from the automatic residency state: loading, a recorded terminal failure (missing file or
    // unloadable source), or nothing loading it.
    std::optional<std::string> DescribeAcousticGeometryReadiness(const FileBackedDemoScene& scene) const;
    std::string GetMeshSourceIdentity(const SceneObjectMetadata& selection) const;

    struct AutomaticMeshResidency
    {
        struct Failure
        {
            SceneObjectMetadata selection;
            std::string sourceIdentity;
        };

        std::vector<Failure> failures; // At most one current failure per runtime object (RayTracingSceneResources::MaxCandidateObjects).
        std::uint64_t runtimeIdentity = 0;
        std::uint64_t structureRevision = 0;
        size_t cursor = 0;
        size_t eligible = 0;
        size_t published = 0;
        size_t initiallyResident = 0;
        size_t stale = 0;
        bool enabled = false;
        bool settled = false;
        // The runtime state (document edit revision, validity epoch, residency revision) when the current scan started, and
        // when the last complete scan started. While settled with no recorded failure, an unchanged state cannot hold new
        // work, so the periodic rescan is skipped.
        std::array<std::uint64_t, 3> scanStartState{};
        std::array<std::uint64_t, 3> scannedState{};
        std::chrono::steady_clock::time_point activated;
        std::chrono::steady_clock::time_point retryAfter;
        std::string backpressure;

        // Bounded batching. An unattributed multi-object failure halves the limit
        // until a publication succeeds; an attributed object is retried alone.
        size_t batchLimit = MinimumResidencyBatch;

        // W6.4: frame-budgeted batch size, between MinimumResidencyBatch (the former fixed size) and the renderer bound. After
        // each published batch: doubled when the batch was full and its worst main-thread residency stage (scan, staging and
        // candidate preparation, or publication) stayed under half the budget; halved when it exceeded the budget.
        size_t adaptiveLimit = MinimumResidencyBatch;
        double batchWorstStageMilliseconds = 0.0;
        size_t completedBatch = 0;
        bool adaptationPending = false;
        std::string isolate;
        size_t batchesPublished = 0;

        // Observation-only streaming counters, reported once at settlement.
        size_t frames = 0;
        size_t idleFrames = 0; // No request outstanding; scanning or cooling down.
        size_t requests = 0;
        size_t workerPendingFrames = 0;
        size_t publicationAttempts = 0;
        std::vector<std::pair<std::string, size_t>> retries; // By readiness message.
        std::chrono::steady_clock::time_point lastPublished;
        const char* blockedReason = nullptr; // Current scheduler gate; reported only once persistent.
        std::chrono::steady_clock::time_point blockedSince;
        size_t blockedSinceFrame = 0;
        bool blockedReported = false;
    };

    AutomaticMeshResidency m_automaticMeshResidency;

    // W6.4: the smallest adaptive residency batch (the pre-W6.4 fixed size), so no scene batches less than before.
    static constexpr size_t MinimumResidencyBatch = 32;
    // Called once per frame after the publication boundary: records the outstanding batch's stage costs and applies a
    // published batch's adaptation.
    void ObserveAutomaticResidencyFrame();
    static std::size_t ResidencyNewGeometryBudget();

    // S0.4: bounded import prefetch for automatic residency. While the scheduler waits on an object whose FBX mesh is not yet
    // catalog-retained, the worker is given up to MaxMeshImportPrefetch distinct imports at once instead of one. A completed
    // import is turned into catalog-retained GPU geometry by exactly the single-request path's steps (currentness, cache
    // integration, source-validated ResolvePreparedMesh), only at the scheduler's own idle boundary (no residency token, CPU
    // request or candidate outstanding), so no in-flight candidate sees the catalog change. Retained meshes then publish through
    // the existing batch path. Tickets belong to one runtime session and generation; anything stale is canceled and dropped, and
    // a failed prefetch only sends its object back to the single-request path, which records failures as before.
    struct MeshImportPrefetch
    {
        std::string meshReference;
        std::uint64_t ticket = 0;
        std::optional<ProjectMeshPreparationService::Completion> completion;
    };

    static constexpr size_t MaxMeshImportPrefetch = 8;
    static constexpr size_t MeshImportPrefetchLookAhead = 256;
    static constexpr size_t MaxMeshImportRequestsPerFrame = 1;
    std::vector<MeshImportPrefetch> m_meshImportPrefetch;
    std::vector<std::string> m_meshImportPrefetchFailed; // References that fall back to the single-request path.
    std::uint64_t m_meshImportPrefetchRuntime = 0;
    std::uint64_t m_meshImportPrefetchGeneration = 0;
    void CancelMeshImportPrefetch() noexcept;
    void PollMeshImportPrefetch(FileBackedDemoScene& scene);
    void IntegrateMeshImportPrefetch(FileBackedDemoScene& scene);
    // True when `selection`'s mesh is (now) being prefetched, so the scheduler waits instead of requesting it alone.
    bool PrefetchMeshImports(FileBackedDemoScene& scene, const SceneObjectMetadata& selection);
    void CountAutomaticPublicationAttempt(const MeshResidencyPublication::AcceptanceResult& result);
    void CountAutomaticRetry(const std::string& reason);
    const char* GetAutomaticResidencyGate(const FileSceneRuntime& runtime) const;
    bool TryPrepareRetainedMeshResidency(FileBackedDemoScene& scene, const SceneObjectMetadata& selection);
    void ReportAutomaticResidencyGate(const char* reason);
    // Automatic scheduling intent is scoped to a successfully activated runtime,
    // independent of its cursor, cooldowns and transient request handles.
    std::uint64_t m_automaticResidencyIntent = 0;
    std::optional<SceneObjectMetadata> m_residencyRequestSelection;
    // Every entry of an outstanding automatic batch; empty for single-object requests.
    std::vector<SceneObjectMetadata> m_residencyRequestBatch;
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
        // W6 finalization: the incremental path refused or timed out; rebuild scene resources as before.
        bool reconstruct = false;
    };
    bool QueueMaterialAssignment(MaterialAssignmentRequest request, std::string& error);
    std::optional<MaterialAssignmentRequest> m_pendingMaterialAssignment;
    void ProcessEditorSceneRequests(IDemoScene& activeScene);
    void ProcessEditorMaterialRequests();
    bool ChangeMeshImportUnits(const ProjectPaths& paths, const AssetBrowser::MeshImportUnitsRequest& request,
        const FileBackedDemoScene* activeScene, std::string& error);
    void ProcessSavedMaterialRevisions();
    // S0.8A: the cheap pass runs early in the frame; the expensive pass runs after mesh residency, within the frame's
    // remaining streaming budget (see Engine.cpp).
    void ProcessSceneTextureStreaming(bool expensivePass);
    // Both passes as one complete update (diagnostics that drive frames by hand).
    void ProcessSceneTextureStreaming()
    {
        ProcessSceneTextureStreaming(false);
        m_frameStarted = std::chrono::steady_clock::now();
        ProcessSceneTextureStreaming(true);
    }
    std::chrono::steady_clock::time_point m_frameStarted;
    int m_textureExpensiveSkippedFrames = 0;
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
    // M0c.4 Stage 4.0 diagnostics: the configuration readiness trace, only when ENGINE_DIAG_READINESS is set (Editor and
    // packaged). Observes after each Render; never changes rendering.
    std::unique_ptr<ConfigurationReadinessTrace> m_readinessTrace;
    // M0c.5 Stage 5.0 diagnostics (ENGINE_STARTUP_TIMING): one-shot startup milestones, logged by TransitionTiming.
    bool m_startupBrowserFrameNoted = false;
    bool m_startupSceneFrameNoted = false;
    bool m_startupLitFrameNoted = false;
    void NoteStartupFrame(Renderer::FrameRenderResult result);

#if defined(ENGINE_HAS_VERIFICATION)
    friend class SceneDuplicationVerification;
    friend class SceneMembershipVerification;
    friend class ResidencyBatchVerification;
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

    // W6 finalization: a resident object's material assignment published incrementally: material-table admission when the
    // material is new, then SceneMaterialBindingPublication at the frame boundary. No scene resources, BLAS, geometry or
    // membership are released. A refusal or a ten-second stall returns the request to the reconstruction path.
    struct PendingIncrementalMaterial
    {
        MaterialAssignmentRequest request;
        std::chrono::steady_clock::time_point started;
        std::optional<AssetID> material; // Admitted first when not yet in the renderer's table; empty for a built-in reference.
        std::uint64_t preparationTicket = 0;
        std::shared_ptr<const ProjectMaterialPreparationService::Candidate> candidate;
        bool admissionStarted = false;
        bool prepared = false;
        std::chrono::steady_clock::time_point retryAfter{};
        double prepareMilliseconds = 0.0;
        std::size_t frames = 0;
    };
    std::optional<PendingIncrementalMaterial> m_pendingIncrementalMaterial;
    SceneMaterialBindingPublication m_incrementalMaterialPublication;
    bool BeginIncrementalMaterialAssignment(const MaterialAssignmentRequest& request);
    void ProcessIncrementalMaterialAssignment(IDemoScene& scene);
    void ReleaseIncrementalMaterialPreparation() noexcept;
    void FallBackFromIncrementalMaterial(std::string_view reason);
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
    // VX.5: keeps the active scene's baked voxel regions near the camera resident on the GPU, for obstacle queries.
    VoxelResidencyController m_voxelResidency;
    // R1.4b: feeds the controller for `scene` (or none): its runtime identity, its bake's manifest, and a store for the scene's
    // source (the project's registered assets for a file scene, the activation's archive for an archive scene). Not admitted, or
    // without a bake, the controller is told there is nothing (identity 0), which retires any residency and does no work.
    // Returns the residency scene identity this frame used. Main thread, between frames, before Render.
    std::uint64_t UpdateVoxelResidency(const FileBackedDemoScene* scene, bool admitted, const Vector3& camera);
    // R1.4b: whether a packaged frame keeps `scene`'s bake resident: it has one and its cooked AIR configuration requests AIR (the
    // only consumer), or the diagnostics override forces it. Logged once per scene runtime.
    bool AdmitPackagedVoxelResidency(const FileBackedDemoScene& scene);
    // R1.4b: the single AIR reset decision: a different scene starts from a fresh air field. R1.5b (decision D2): keyed by the active
    // scene runtime's identity (0 for none), in the Editor and in packaged frames, so an unbaked scene (no residency) still resets.
    // World-owned AIR will change this one place (reset on a world identity instead). R1.6: also keyed by the physics session generation
    // (0 outside a session), which Editor Play and Physics Simulation share, so every Start and Stop begins from a fresh field too: a
    // session never inherits the previous session's or the editing field's state, and editing never inherits a session's.
    void FollowAirScene(std::uint64_t airScene, std::uint64_t physicsSession);
    // R1.5b (verification only, never set otherwise): a fixed AIR frame delta for both the Editor's and packaged frames, so a parity run
    // takes the same AIR steps in both. The AIR camera follows m_voxelResidencyCameraOverride as residency does.
    std::optional<double> m_airDeltaOverrideForVerification;
    // R1.5b (decision D4): when the packaged scene's cooked AIR configuration requests AIR, initializes it (Renderer::
    // InitializePackagedAir, with the mist noise read from the archive) before the scene's first AIR frame. Once per scene runtime; a
    // scene that does not request AIR initializes nothing.
    void PreparePackagedAir(const FileBackedDemoScene& scene);
    std::uint64_t m_packagedAirPreparedScene = 0;
    Renderer::PackagedAirInitialization m_packagedAirInitialization;
    // R1.5: the packaged scene runtime's cooked moving-solid participants (ResolveMovingSolidParticipants), for request-level parity.
    std::uint64_t m_packagedMovingSolidScene = 0;
    std::vector<std::string> m_packagedMovingSolidParticipants;
    // R1.5b: the inputs of an AIR frame request that only the Editor provides. A packaged runtime passes the defaults: running, the mist
    // running, no diagnostics, the production transport, and no diagnostic overrides.
    struct AirRequestControls
    {
        bool paused = false;
        bool mistPaused = false;
        // The pressure solver's diagnostics (the Air window's divergence view).
        bool pressureDiagnostics = false;
        // AIR.4F.2d.4 diagnostics (ENGINE_AIR_CONSERVATIVE_MIST=0, ENGINE_AIR_MIST_SUBSTEPS): the Editor reads them; production keeps
        // the conservative transport with adaptive substeps.
        bool conservativeMistTransport = true;
        std::uint32_t forcedMistSubsteps = 0;
        // AIR.4F.3 diagnostic (ENGINE_AIR_MOVING_SOLIDS=1, Debug builds): overlay on and every movable body participating.
        bool diagnosticMovingSolids = false;
    };
    // R1.5b: the one construction of an AIR frame request, shared by the Editor and packaged frames: the simulation and environment
    // settings, the pressure projection and buoyancy, the mist and its transport, the scene's motors and emitters (captured from this
    // frame's presented poses) and its moving solids. `scene` may be null (no motors, emitters or environment). Main thread, after
    // physics publication, before Render.
    void BuildAirFrameRequest(AirFrameRequest& request, const AirSimulationSettings& simulation, FileBackedDemoScene* scene,
        const Vector3& camera, double deltaSeconds, const AirRequestControls& controls);
    // R1.5: the frame's moving-solid overlay request (enable, participants and their poses) from `simulation` (this frame's resolved
    // AIR configuration) and the physics snapshot presented this frame. Applies the participation policy once per physics session.
    // Main thread, Editor frames, before Render.
    void BuildAirMovingSolidRequest(AirMovingSolidRequest& request, const AirSimulationSettings& simulation, bool diagnosticOverlay);
    // R1.4c: logs, once per admitted packaged scene runtime, how long its residency took to settle and what it holds; and at
    // packaged shutdown, the totals with the AIR step count. Observation only: nothing here changes what residency does.
    void ReportPackagedVoxelResidency(std::uint64_t residencyScene);
    void ReportPackagedVoxelResidencyTotals() const;
    // M0b.1 P2: gathers existing subsystem CPU statistics for the Editor-only memory monitor when a sample is due.
    // The disabled monitor (packaged games) is never due, so nothing is gathered there.
    void SampleMemoryMonitor();
    std::uint64_t m_packagedResidencyReportScene = 0;
    bool m_packagedResidencySettledReported = false;
    std::chrono::steady_clock::time_point m_packagedResidencyStarted;
    // R1.4b (diagnostics only, verification builds): ENGINE_DIAG_PACKAGED_VOXEL_RESIDENCY forces packaged residency without AIR.
    bool m_packagedVoxelResidencyForced = false;
    std::optional<std::uint64_t> m_packagedVoxelResidencyLoggedScene;
    // R1.4b (verification only): a fixed residency camera, so Editor and packaged runs settle the same window. Never set otherwise.
    std::optional<Vector3> m_voxelResidencyCameraOverride;
    // AIR.1: the air field request (disabled until the Editor's air controls exist) and the scene it belongs to.
    AirFrameRequest m_airRequest;
    // R1.1: the AIR simulation configuration ResolveAirSimulation gave the latest frame, before the packaged restriction (a packaged
    // runtime resolves its cooked settings but m_airRequest stays disabled).
    AirSimulationSettings m_airConfiguration;
    // R1.1: the scene runtime whose cooked AIR request the packaged restriction last reported (logged once per scene).
    std::optional<std::uint64_t> m_packagedAirRestrictionLoggedScene;
    // AIR.4F.2b: how many motors lacked a slot when that count was last logged.
    std::size_t m_loggedMotorsWithoutCapacity = 0;
    std::uint64_t m_airScene = 0;
    std::uint64_t m_airPhysicsSession = 0;

    std::optional<Editor::FileRequest> m_pendingEditorSceneOperation;
    // W4: the World composed and validated when its open was queued; staged once the scene change is confirmed.
    std::shared_ptr<CompositeWorld> m_pendingCompositeWorld;

    // W4: World-open timings, logged once the composite scene is active (composition, then request to activation).
    struct CompositeWorldTiming
    {
        double composeMilliseconds = 0.0;
        std::chrono::steady_clock::time_point requested;
        bool reported = true;
    };

    CompositeWorldTiming m_compositeWorldTiming;

    // CPU time of the last frame's Editor drawing and its viewport overlay capture (colliders, camera frusta, light and AIR motor
    // wireframes), for diagnostics: two clock reads per region, no synchronisation.
    struct FrameCpuTimings
    {
        double editorDraw = 0.0;
        double overlays = 0.0;
        double residency = 0.0;
        double textureStreaming = 0.0;
        double acceptResidency = 0.0;
    };

    FrameCpuTimings m_frameCpuTimings;
    void ReportCompositeWorldActivation(const FileBackedDemoScene* scene);
    void ProcessCompositeSaveRequest(FileBackedDemoScene* scene);

    std::string m_sceneOperationError;
    bool m_sceneDecisionPending = false;
    bool m_projectCreationPending = false;
    bool m_editorRelaunchStarted = false;
    bool CanPerformSceneLifecycle(std::string& error) const;
    enum class SceneSaveResult { Saved, Canceled, Failed };
    SceneSaveResult SaveCompositeWorld(FileBackedDemoScene& scene, std::optional<std::size_t> onlyDocument);
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
