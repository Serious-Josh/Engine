#pragma once
#include "Scene/CloudPresets.h"
#include "Scene/SceneHierarchyEdits.h"

#include "Scene/Transform.h"
#include "Scene/AirSimulationSettings.h"
#include "Engine/BuildGame.h"
#include "Platform/Windows/ProjectFileAssociation.h"
#include "Engine/CharacterDriveScheduling.h"
#include "Physics/CharacterControllerDiagnostics.h"
#include "SceneSelection.h"
#include "ViewportMarquee.h"
#include "AssetBrowser.h"
#include "EditorWorkspace.h"
#include "EditorFilesystemCache.h"
#include "BackgroundTasksUI.h"
#include "InspectorPanel.h"
#include "SceneHierarchyPanel.h"
#include "ViewportGridSnap.h"
#include "EditorPreferences.h"
#include "ViewportIcons.h"
#include "Engine/ProbeAssetCleanup.h"
#include "Engine/ProbeVolumes.h"
#include "Engine/PathingBakes.h"
#include "Engine/ReflectionBakeJobs.h"
#include "Engine/ReflectionBakeStatus.h"
#include "Engine/VoxelScene.h"
#include "Engine/VoxelVisualization.h"
#include "assets/ProbeBatchAsset.h"
#include "Scene/FileSceneAssetCatalog.h"
#include "Project/ProjectContext.h"
#include "Debug/EditorPhaseTiming.h"
#include "Scripting/ScriptHost.h"
#include "Audio/AudioSystem.h"

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <variant>
#include <memory>
#include <utility>

#include "Editor/SceneMembershipPanel.h"

class IDemoScene;
class FileBackedDemoScene;
struct CompositeWorld;
class Scene;
class SphereGridScene;
class StressTestScene;
class DemoSceneManager;
class DebugUI;
class ProjectPaths;
class ProjectAssetRegistry;

class Editor
{
public:
    enum class FileRequestType
    {
        New,
        Save,
        Load,
        Demo,
        SaveAs,
        NewProject,
        OpenProject,
        ProjectSettings,
        Exit
    };

    struct FileRequest
    {
        FileRequestType type;

        std::filesystem::path path;

        std::string sceneName;
    };

    enum class SceneChangeDecision
    {
        None,
        Save,
        Discard,
        Cancel
    };

    // Normal user-close requests may be deferred until material edits are resolved.
    void ProcessAssetRename(const ProjectPaths& paths, ProjectAssetRegistry& registry,
        const std::filesystem::path& activeScenePath);
    void ProcessAssetDeletion(const ProjectPaths& paths, ProjectAssetRegistry& registry);
    void ProcessAssetMove(const ProjectPaths& paths, ProjectAssetRegistry& registry);
    // W2: performs the Scene Membership panel's request (World creation, assignment, placement, duplication) through
    // ProjectSceneMembership. Refused while edits or scene operations are pending, and for the open scene (see the .cpp).
    // W3: the HDRIs the World environment editor offers (the Renderer's list; built-in files under `builtinRoot`).
    void SyncWorldEnvironmentChoices(const std::vector<EnvironmentAsset>& environments, const std::filesystem::path& builtinRoot)
    {
        m_sceneMembership.SyncEnvironmentChoices(environments, builtinRoot);
        // Physical sky S4: the Sky window's HDRI Sky picker offers the same list (indices are the Renderer's).
        SyncSkyHdriChoices(environments);
    }

    void SyncSkyHdriChoices(const std::vector<EnvironmentAsset>& environments);

    // Physical sky S4: the HDRI chosen in the Sky window's HDRI Sky section (an index into the Renderer's list), once. Engine
    // applies it as the former Debug > Rendering Settings HDRI combo did (Engine::AssignSceneEnvironment).
    std::optional<size_t> TakeSkyHdriRequest() noexcept
    {
        return std::exchange(m_skyHdriRequest, std::nullopt);
    }

    // Physical sky S4: opens Level > Sky (the redirect of the former HDRI and luminance controls).
    void OpenSkyWindow() noexcept
    {
        m_showSkyWindow = true;
    }

    void ProcessSceneMembership(const ProjectPaths& paths, ProjectAssetRegistry& registry, const FileBackedDemoScene* activeScene);
    // W3: a membership or World environment change succeeded since the last call (the open Local Level's World may have changed).
    bool TakeEnvironmentWorldChanged() noexcept { return std::exchange(m_environmentWorldChanged, false); }
    bool RequestApplicationClose();
    bool TakeApplicationCloseApproval();

    void ImportFileDrop(
        const std::vector<std::filesystem::path>& files,
        float screenX,
        float screenY,
        const ProjectPaths& paths,
        ProjectAssetRegistry& registry);

    bool HasBlockingMaterialOperation() const
    {
        return m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending ||
            m_inspectorPanel.HasUnsavedChanges();
    }

    // The queued creation's own scene-operation UI lock is not a conflict.
    bool HasBlockingObjectCreation() const
    {
        return m_materialLeaveAction != MaterialLeaveAction::None || m_inspectorPanel.HasUnsavedChanges() ||
            m_materialAssignmentPending || m_transformFieldEditing || m_gizmoWasUsing || m_marqueeGesture.has_value();
    }

    // Also binds the Engine-owned active project, which outlives the Editor's use of it.
    void ConfigureMaterialThumbnails(Renderer& renderer, const ProjectContext& activeProject)
    {
        m_activeProject = &activeProject;
        m_materialDropRenderer = &renderer;
        m_assetBrowser.ConfigureMaterialThumbnails(renderer, activeProject.GetPaths());
        m_inspectorPanel.ConfigureThumbnails(renderer);
    }

    void SetThumbnailCompletedFence(std::uint64_t fence)
    {
        m_assetBrowser.SetThumbnailCompletedFence(fence);
    }

    bool NeedsThumbnailReset() const
    {
        return m_assetBrowser.NeedsThumbnailReset();
    }

    // Audio.2B: the process's AudioSystem, read-only, for the Audio Source event picker. Null or not running shows
    // the inspector without event names.
    void SetAudioSystem(Audio::AudioSystem* audio) noexcept
    {
        m_audioSystem = audio;
    }

    // Engine owns the manager and destroys it after this Editor. Null shows an idle bar.
    void SetBackgroundTasks(BackgroundTaskManager* manager) noexcept
    {
        m_backgroundTasks = manager;
    }

    // Engine reports whether the scene's acoustic meshes have loaded: nullopt when ready, otherwise why not. Baking and probe
    // generation read that geometry, so they refuse until it is ready. Unset means ready.
    using AcousticGeometryReadiness = std::function<std::optional<std::string>(const FileBackedDemoScene&)>;
    void SetAcousticGeometryReadiness(AcousticGeometryReadiness readiness)
    {
        m_acousticGeometryReadiness = std::move(readiness);
    }

    bool TakeScriptRefreshRequest()
    {
        return m_assetBrowser.TakeScriptRefreshRequest();
    }

    void ResetThumbnailRequests()
    {
        m_assetBrowser.ResetThumbnailRequests();
    }

    const std::optional<MaterialPreviewRequest>& GetMaterialPreviewRequest()
    {
        return m_inspectorPanel.GetPreviewRequest();
    }

    void SetSceneOperationPending(bool pending) noexcept;
    enum class PhysicsState { Stopped, Playing, Stopping };
    enum class PhysicsRequest { Play, Stop };
    void SetPhysicsStatus(PhysicsState state, bool canPlay, std::string message);
    std::optional<PhysicsRequest> TakePhysicsRequest();
    bool CanStartPhysics() const;
    // AS.4 Game Play. Engine reports the shared GameRuntime's state each frame; the viewport
    // toolbar only requests Play/Stop. Playing turns the Scene viewport into the game view.
    enum class GameState { Editing, Playing, Stopping };
    enum class GameRequest { Play, Stop };
    void SetGameStatus(GameState state, bool canPlay, std::string message);
    std::optional<GameRequest> TakeGameRequest();
    bool IsGamePlaying() const noexcept { return m_gameState != GameState::Editing; }
    // Gameplay receives input only while the viewport it renders into has focus and no ImGui
    // widget or window is capturing the keyboard.
    bool HasGameplayFocus() const;
    // AS.6B cursor lock area: the game view image in client pixels while it has gameplay focus.
    std::optional<EditorWorkspaceRect> GetGameplayCursorArea() const;
    // A left click on the game view image this frame (recaptures a released cursor lock).
    bool WasGameViewClicked() const;
    // Keeps ImGui from showing its cursor over the captured game view this frame.
    static void HideGameplayCursor();
    bool ArePhysicsCollidersVisible() const noexcept { return m_showPhysicsColliders; }
    bool AreCameraFrustaVisible() const noexcept { return m_showCameraFrusta; }
    bool AreLightWireframesVisible() const noexcept { return m_showLightWireframes; }
    // The Overlay menu's Emitters: Air Motor (wind) and Mist Emitter wireframes together.
    bool AreAirMotorWireframesVisible() const noexcept { return m_showAirMotorWireframes; }
    // G.3 viewport grid spacing for this frame: the applied translation snap interval, or empty
    // when the Grid overlay is off or Play mode has locked authored editing.
    std::optional<float> GetViewportGridSpacing() const;

    // Snap and grid values persisted in global Editor preferences (PR.5). Apply accepts only
    // valid intervals (invalid ones keep the current value) and also resets the text drafts.
    ViewportPreferences GetViewportPreferences() const;
    void ApplyViewportPreferences(const ViewportPreferences& preferences);
    // CM.9 diagnostics. Engine supplies the published controller diagnostics after each
    // physics frame (null when no session is active), and takes the diagnostic drive intent
    // immediately before the next Update; it acknowledges a jump once it is submitted.
    void SetCharacterDiagnostics(const Physics::CharacterDiagnosticsFrame* frame);
    std::optional<CharacterDriveIntent> TakeCharacterDriveIntent() const;
    void AcknowledgeCharacterDriveJump();
    // Authored edits are locked while Physics Simulation or Game Play owns the scene.
    bool IsSceneEditingLocked() const noexcept
    {
        return IsSceneSimulationLocked();
    }
    // Physics Simulation or Game Play owns the scene: leaving it (opening another scene or project) is refused as well.
    bool IsSceneSimulationLocked() const noexcept
    {
        return m_physicsState != PhysicsState::Stopped || m_gameState != GameState::Editing;
    }
    // W5: the active scene is a composite World. It is editable; the flag gates World-only restrictions (bakes, Play, Save As) and
    // the per-document Save controls.
    void SetCompositePreview(bool composite) noexcept { m_compositePreview = composite; }
    // A Save request from the World panel: empty = Save All, otherwise one document (0 = World, i + 1 = Local Level i).
    std::optional<std::optional<std::size_t>> TakeCompositeSaveRequest()
    {
        return std::exchange(m_compositeSaveRequest, std::nullopt);
    }
    bool IsCompositePreview() const noexcept { return m_compositePreview; }
    void OnSceneActivated();
    // Interaction cleanup alone does not change the history session or cursor.
    void ResetSceneInteractions();
    std::uint64_t GetSceneHistoryEpoch() const noexcept { return m_historyEpoch; }
    bool CanPreserveSceneHistory(const IDemoScene& scene, const std::filesystem::path& canonicalProject,
        std::uint64_t epoch) const;
    // Caller has checked session compatibility before Renderer publication. No fallible lookup follows.
    void PublishRestoredObjectSelection(const FileBackedDemoScene& scene,
        std::optional<SceneObjectMetadata> selection, std::string activeSceneName, bool retainGroup = true) noexcept;
    // Future checked structural publication may explicitly rebind the same editing session.
    // Unsupported creation continues using OnSceneActivated().
    bool RebindSceneHistoryAfterStructureChange(const IDemoScene& scene);
    // Call only after the candidate runtime is published. Persistent identity is
    // resolved before rebinding selection; structural edits clear index history.
    void PublishCreatedObjectSelection(const Scene& scene, SceneObjectMetadata metadata,
        std::string activeSceneName) noexcept;
    struct SceneMaterialSavedRevision
    {
        MaterialSavedRevision saved;

        std::uint64_t catalogIdentity = 0;
        std::uint64_t runtimeIdentity = 0;
        std::uint64_t structureRevision = 0;
    };
    std::vector<SceneMaterialSavedRevision> TakeSavedMaterialRevisions();
    struct SceneMaterialWorkingRevision
    {
        MaterialWorkingRevision working;

        RuntimeMaterialVersions runtime;
    };
    std::vector<SceneMaterialWorkingRevision> TakeWorkingMaterialRevisions();

    struct MaterialAssignmentRequest
    {
        SceneObjectMetadata selection;

        std::string reference;
        std::optional<std::string> slotID;
        std::optional<AssetID> slotMaterial;
    };
    std::optional<MaterialAssignmentRequest> TakeMaterialAssignmentRequest();
    void OnMaterialAssignmentCompleted(const SceneObjectMetadata& selection, bool succeeded, std::string message);

    struct MaterialBindingRequest
    {
        SceneObjectMetadata selection;
        std::optional<AssetID> allSlotsMaterial;
        std::map<std::string, std::string> overrides;
        MeshMetadata layout;
        std::uint64_t operationID = 0;
        std::uint64_t epoch = 0;
    };
    std::optional<MaterialBindingRequest> TakeViewportMaterialRequest();
    bool IsViewportMaterialRequestCurrent(const MaterialBindingRequest& request) const noexcept;
    void NotifyViewportMaterialRetry(const MaterialBindingRequest& request, std::string_view message);
    void CompleteViewportMaterialRequest(const MaterialBindingRequest& request,
        bool succeeded, bool published, std::string_view message) noexcept;
    std::optional<size_t> GetMaterialDropHighlight(const IDemoScene& scene) const;

    struct PrimitiveCreationRequest
    {
        ScenePrimitive primitive;
        std::uint64_t runtimeIdentity = 0;
        std::uint64_t historyEpoch = 0;
    };
    std::optional<PrimitiveCreationRequest> TakePrimitiveCreationRequest();

    // Hierarchy "Empty Object": a meshless, materialless object created through the same
    // checked membership and history path as primitives.
    struct EmptyObjectCreationRequest
    {
        std::uint64_t runtimeIdentity = 0;
        std::uint64_t historyEpoch = 0;
    };
    std::optional<EmptyObjectCreationRequest> TakeEmptyObjectCreationRequest();
    void OnEmptyObjectCreationFailed(const std::string& error);
    // Hierarchy "Camera": same eligibility and identity as Empty Object creation. Engine reads
    // the fly view when it begins creation, so the pose is current and never stored here.
    using CameraObjectCreationRequest = EmptyObjectCreationRequest;
    std::optional<CameraObjectCreationRequest> TakeCameraObjectCreationRequest();
    void OnCameraObjectCreationFailed(const std::string& error);
    // Hierarchy "Light" entries: same eligibility and identity as Camera creation, plus the type.
    struct LightObjectCreationRequest
    {
        LightType type = LightType::Point;
        std::uint64_t runtimeIdentity = 0;
        std::uint64_t historyEpoch = 0;
    };
    std::optional<LightObjectCreationRequest> TakeLightObjectCreationRequest();
    void OnLightObjectCreationFailed(const std::string& error);
    // Hierarchy "Steam Audio > Probe Volume": same eligibility and identity as Camera creation.
    using ProbeVolumeObjectCreationRequest = EmptyObjectCreationRequest;
    std::optional<ProbeVolumeObjectCreationRequest> TakeProbeVolumeObjectCreationRequest();
    void OnProbeVolumeObjectCreationFailed(const std::string& error);
    struct MeshCreationRequest
    {
        std::string meshReference;
        std::string name;

        Transform transform;

        std::uint64_t runtimeIdentity = 0;
    };
    std::optional<MeshCreationRequest> TakeMeshCreationRequest();
    void OnMeshCreationCompleted(bool succeeded, const std::string& error);

    struct HistoryReplayIdentity
    {
        std::uint64_t operationID = 0;
        std::uint64_t epoch = 0;
        bool redo = false;
        std::optional<std::string> selectionID;
    };
    struct HistoryReplayRequest
    {
        HistoryReplayIdentity identity;
        SceneObjectDocument object;
        size_t insertionIndex = 0;
        std::optional<std::string> bindingSourceID;
        bool membership = false;
        bool remove = false;
        std::optional<SceneHierarchyEdit> hierarchy;
    };
    std::optional<HistoryReplayRequest> TakeSceneDuplicationRequest();
    void HandleSceneObjectDuplication(IDemoScene&);
    bool QueueSceneObjectDuplication(IDemoScene& scene, const SceneObjectMetadata& target, std::string& error);
    bool PrepareHierarchyDrop(IDemoScene& scene, const HierarchyDropRequest& request,
        SceneHierarchyEdit& edit, std::string& error);
    bool SubmitHierarchyDrop(IDemoScene& scene, const HierarchyDropRequest& request, std::string& error);
    bool QueueHierarchyEmpty(IDemoScene& scene, std::string_view parentID, std::string& error);
    void ProcessMaterialLocateRequest(const ProjectPaths&,const ProjectAssetRegistry&);
    std::uint64_t AllocateSceneHistoryOperationID() noexcept { return ++m_historyOperationSerial; }
    bool HasPendingSceneHistoryOperation() const noexcept;
    bool MatchesPendingHistoryReplay(const HistoryReplayIdentity& identity) const noexcept;
    std::optional<HistoryReplayRequest> TakeSceneHistoryReplayRequest();
    void InvalidateSceneHistoryRedoAfterEdit() noexcept;
    bool StageObjectDeletionHistory(SceneObjectDocument object, size_t documentIndex,
        std::optional<std::string> selectionBefore, std::optional<std::string> selectionAfter,
        std::uint64_t operationID, std::uint64_t epoch, bool replay, std::string& error, bool insertion = false);
    bool StageHierarchyHistory(const SceneHierarchyEdit& edit,
        std::optional<std::string> selectionBefore, std::optional<std::string> selectionAfter,
        std::uint64_t operationID, std::string& error);
    // H.5 backend seam; tree UI delivery belongs to H.6.
    bool QueueHierarchyReparent(IDemoScene& scene, std::string_view objectID, std::string_view parentID,
        bool keepWorld, std::string_view beforeSibling, std::string& error);
    bool QueueHierarchyCreation(IDemoScene& scene, SceneObjectDocument object, std::string_view parentID,
        std::string& error);
    bool QueueHierarchyEdit(IDemoScene& scene, SceneHierarchyEdit edit,
        std::optional<std::string> selectionAfter, std::string& error);
    void CompleteObjectDeletionHistory(std::uint64_t operationID, std::uint64_t epoch,
        bool succeeded, const std::string& error);
    void CompleteSceneHistoryReplay(const HistoryReplayIdentity& identity, bool succeeded, const std::string& error);

    std::optional<SceneObjectMetadata> TakeSceneObjectDeletionRequest();
    void OnSceneObjectDeletionCompleted(const IDemoScene& scene, const SceneObjectMetadata& selection,
        bool succeeded, const std::string& error);

    void SuspendViewportInteraction();

    bool IsWorkspaceSplitterDragging() const
    {
        return m_workspace.IsSplitterDragging();
    }

    SceneChangeDecision DrawSceneChangeConfirmation(const std::string& error);

    struct BuildGameRequest
    {
        BuildGame::Settings settings;

        // Explicit "Save Scene and Build": save the active startup scene through File > Save first.
        bool saveStartupScene = false;
    };

    // Taking a request clears it. Pending actions cannot be overwritten.
    std::optional<BuildGameRequest> TakeBuildGameRequest()
    {
        return std::exchange(m_buildGameRequest, std::nullopt);
    }

    bool TakeBuildGameCancel() { return std::exchange(m_buildGameCancel,false); }
    void SetBuildGameStatus(BuildGame::Status status) { m_buildGameStatus=std::move(status); }

    // Offers "Save Scene and Build" after a build was refused for an unsaved active startup scene.
    void SetBuildStartupSceneUnsaved(bool unsaved)
    {
        m_buildStartupSceneUnsaved = unsaved;
    }

    // Project Settings request for the shared game startup scene: an absolute scene path, or an
    // empty path to clear the setting. Engine validates and persists it, then reports the result.
    std::optional<std::filesystem::path> TakeStartupSceneRequest()
    {
        return std::exchange(m_startupSceneRequest, std::nullopt);
    }

    void OnStartupSceneChanged(bool succeeded, std::string message);

    // PS.3 Project Settings (Edit > Project Settings). The window edits a draft of the saved settings; Save
    // queues the validated draft, which Engine persists through Engine::SaveProjectSettings and reports back.
    void OpenProjectSettings();
    std::optional<ProjectSettings> TakeProjectSettingsSaveRequest()
    {
        return std::exchange(m_projectSettingsSaveRequest, std::nullopt);
    }
    void OnProjectSettingsSaveCompleted(bool succeeded, std::string message);
    // The draft differs from the saved settings.
    bool HasUnsavedProjectSettings() const;

    // Blocking prompt while project manifest ownership cannot be verified (Engine fail-closed state).
    enum class OwnershipAction
    {
        None,
        Retry,
        Close
    };

    void SetProjectOwnershipLost(std::string message)
    {
        m_projectOwnershipMessage = std::move(message);
    }

    OwnershipAction TakeOwnershipAction()
    {
        return std::exchange(m_ownershipAction, OwnershipAction::None);
    }
    std::optional<FileRequest> TakeFileRequest();
    bool HasFileRequest(FileRequestType type) const noexcept;
    bool QueueFileRequest(FileRequest request);
    void InvalidateSceneFiles()
    {
        m_assetBrowser.InvalidateFileListings();
        m_sceneMembership.Invalidate();
    }
    void ReportLifecycleMessage(std::string message);
    bool RebindSceneHistoryAfterSaveAs(const FileBackedDemoScene& scene, const std::filesystem::path& previousPath);
    void DrawMaterialLeaveConfirmation(IDemoScene& scene, const ProjectPaths& projectPaths,
        const ProjectAssetRegistry& assetRegistry);
    std::optional<AssetBrowser::MaterialCreationRequest> TakeMaterialCreationRequest();
    std::optional<AssetBrowser::AcousticMaterialCreationRequest> TakeAcousticMaterialCreationRequest();
    // M7: Engine owns the registry, so it stores a volume's probes on the Editor's behalf.
    struct ProbeBatchRequest
    {
        std::string objectID;
        std::vector<std::byte> encoded;
    };
    // M7.1, M8.3: applies finished acoustic jobs (probe generation and reflection baking), cancels any that lost their scene, and
    // refreshes the bake status the Inspector and Scene Hierarchy show. Runs every frame, whatever is selected.
    void PollAcousticJobs(IDemoScene& scene, const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    // M8.3: Engine stores a validated bake on the Editor's behalf, as for probes.
    struct ReflectionBakeStoreRequest
    {
        std::string objectID;
        std::vector<std::byte> encoded;
    };
    std::optional<ReflectionBakeStoreRequest> TakeReflectionBakeStoreRequest()
    {
        return std::exchange(m_reflectionBakeStoreRequest, std::nullopt);
    }
    void OnReflectionBakeStoreCompleted(IDemoScene& scene, const std::string& objectID, const std::string& reference, const std::string& error);
    // M9.6: the same for a baked pathing asset.
    std::optional<ReflectionBakeStoreRequest> TakePathingStoreRequest()
    {
        return std::exchange(m_pathingStoreRequest, std::nullopt);
    }
    void OnPathingStoreCompleted(IDemoScene& scene, const std::string& objectID, const std::string& reference, const std::string& error);
    // M9.6, display only: Play-mode pathing status per Audio Source object. Engine supplies it from the game session's events.
    using PathingStatusProvider = std::function<std::optional<Audio::AudioSystem::PathingEventStatus>(std::string_view objectID)>;
    void SetPathingStatusProvider(PathingStatusProvider provider)
    {
        m_pathingStatusProvider = std::move(provider);
    }
    // After AudioSystem::Shutdown has stopped and joined the acoustic job: finishes every task it left open as cancelled.
    void StopAcousticWork(std::string_view reason);

    // VX.4C.1 (VoxelControls.cpp). Engine calls this every frame after the physics gate, with its registry: it starts a requested
    // voxel bake, publishes a finished one on this (main) thread and records its manifest in the scene through history, and keeps
    // the Voxels window's status current. A finished bake waits here while Play or a simulation blocks edits.
    void PollVoxelBake(IDemoScene& scene, const ProjectPaths& paths, ProjectAssetRegistry& registry);

    // AIR.4A: the Air window's session settings (AirControls.cpp): pause, views, diagnostics and rendering quality, never saved. R1.1:
    // the simulation configuration (enabled, pressure, buoyancy, steps, wind, damping, motor cap, mist dissipation) is scene data,
    // AirSimulationSettings, resolved by the Engine through ResolveAirSimulation with GetAirSimulationOverride.
    struct AirEditorSettings
    {
        bool paused = false;
        bool showVectors = true;
        // Vectors are 2^exponent cells apart per axis (-2 to 3: a quarter of a cell to 8 cells).
        int vectorSpacingExponent = 0;
        // The most vectors drawn; past it, only a box around the camera.
        int vectorBudget = 262'144;
        float vectorLengthScale = 0.1f;
        float vectorMaximumLength = 1.5f;
        float colorMaximumSpeed = 10.0f;
        float minimumSpeed = 0.05f;
        // AIR.4D D3: one vector per fluid piece at its anchor, instead of one per block at its hashed point.
        bool vectorsAtPieceAnchors = false;
        // AIR.4F F4.5.1b.1: temperature points over the whole window, 2^exponent cells apart (0 to 3), saturating at +/- range kelvin.
        bool showTemperature = false;
        int temperatureSpacingExponent = 1;
        float temperatureDisplayRange = 100.0f;
        // AIR.4E.2 diagnostic: each fluid piece's divergence as points (the temperature points' placement), saturating at +/- range in
        // 1/s; measure 0 is the pressure operator's face fluxes, 1 the stored (collocated) velocities'.
        bool showDivergence = false;
        int divergenceSpacingExponent = 1;
        float divergenceDisplayRange = 2.0f;
        int divergenceMeasure = 0;
        // AIR.4C diagnostic: the obstacle mask's blocked and unknown cells.
        bool showBlockedCells = false;
        bool showUnknownCells = false;
        // AIR.4D diagnostic: the fluid pieces' sub-cell boundary.
        bool showFluidPieces = false;
        // AIR.4F: the passive mist. Its storage exists only while an enabled Mist Emitter feeds it; Show Mist only draws it.
        bool showMist = true;
        bool mistPaused = false;
        // AIR.4F F4.4.1: the mist's appearance (opacity, Worley, curl, lighting) is scene data (SceneDocument::mistRendering). R1.3: its
        // quality preset and resolution basis are project settings (RenderingSettings::volumetrics), overridden here only through the
        // Debug panel's session RenderingOverrides.
        // V1d follow-up, session only (never saved): the volumetric renderer for the mist and haze, the original raymarch (0, the
        // default) or the experimental integrated froxels (1; AirVolumetricV1dReport.md).
        int volumetricRenderer = 0;
        // V1d distant banding (temporary, session only): the medium's adaptive samples per slice (one per AIR cell of depth) or a fixed 4.
        bool volumetricAdaptiveSamples = true;
        // V1d lateral experiment (temporary, session only): 0 baseline (1 x 1 rays, 4 depth samples), 1 2 x 2 rays with 4 depth samples,
        // 2 2 x 2 rays with 1 depth sample (cost parity); and twice the froxel XY resolution.
        int volumetricLateralMode = 0;
        bool volumetricDoubleXY = false;
        // Fog step 1 (session only, never saved): the Original Raymarch reads open footprints from the mist render volume
        // (AirMistRenderRequest::renderVolume; this default matches the request's).
        bool mistRenderVolume = true;
        // Fog step 2 (session only, never saved): the render volume march's table Lights from the hybrid froxel light volume
        // (AirMistRenderRequest::froxelLights as AirMistFroxelLights: 0 Automatic, the request's default; 1 Off; 2 On).
        int mistFroxelLightsMode = 0;
    };

    const AirEditorSettings& GetAirSettings() const noexcept
    {
        return m_airSettings;
    }

    // R1.1: the Play or Physics Simulation session override of the scene runtime `runtimeIdentity`'s AIR simulation configuration,
    // if the Air window made one; nullopt in a stopped scene and for any other runtime. Never saved: it is dropped when Play or the
    // simulation ends and when a scene is activated.
    std::optional<AirSimulationSettings> GetAirSimulationOverride(std::uint64_t runtimeIdentity) const;

    // AIR.4F F4.4.1: the mist appearance to draw for `document` of the scene runtime `runtimeIdentity` (null: no file scene, so the
    // defaults): the value being dragged in the Air window for that runtime, else the scene's own (its defaults when it has none).
    MistRenderSettings GetEffectiveMistRendering(const SceneDocument* document, std::uint64_t runtimeIdentity) const;

    // Physical sky S1: the physical sky the renderer bakes for `scene`: the value being dragged in the Sky window for that scene's
    // runtime, else the scene's effective one (FileBackedDemoScene::GetEffectivePhysicalSky); none unless the effective sky type is
    // Physical.
    std::optional<PhysicalSkySettings> GetRenderedPhysicalSky(const FileBackedDemoScene& scene) const;
    // S4: the Sky window's Update Now (an On Demand sky's rebuild), once.
    bool ConsumeSkyUpdateRequest() noexcept
    {
        return std::exchange(m_skyUpdateRequested, false);
    }

    // Physical sky S1: the windows that reopen at startup (the Level > Sky window), kept in the Editor preferences.
    EditorWindowPreferences GetWindowPreferences() const;
    void ApplyWindowPreferences(const EditorWindowPreferences& preferences);

    // AIR.4D D2: the latest emission status of each Air Motor the air field ran, keyed by AirMotorTag of its object's ID (Engine::
    // Frame passes it on from the Renderer); shown by the Air Motor Inspector.
    void SetAirMotorEmission(std::map<std::uint64_t, AirMotorEmissionStatus> emission)
    {
        m_airMotorEmission = std::move(emission);
    }

    // AIR.4E.5b.1: the fixed air steps the latest rendered frame ran (Engine::Frame passes it on from the Renderer); shown in the Air window.
    void SetAirStepCount(std::uint32_t steps) noexcept
    {
        m_airStepCount = steps;
    }

    // AIR.4F F2: the latest status of each Mist Emitter, keyed by MistEmitterTag of its object's ID; shown by its Inspector.
    void SetMistEmitterStatus(std::map<std::uint64_t, MistEmitterStatus> status)
    {
        m_mistEmitterStatus = std::move(status);
    }

    // True once after the Air window's Clear Mist.
    bool TakeMistClearRequest() noexcept
    {
        return std::exchange(m_mistClearRequested, false);
    }

    // True once after the Air window's Reset.
    bool TakeAirResetRequest() noexcept
    {
        return std::exchange(m_airResetRequested, false);
    }
    // Cancels and joins voxel work; nothing it staged survives, and the scene keeps its manifest. Before the project and its registry go.
    void StopVoxelWork() noexcept;
    // VX.4C.2 voxel overlay. Engine calls this between frames (never while recording): it hands a finished overlay build to the
    // Renderer through `publish` (which returns the generation, or 0 with an error), or drops the overlay through `clear`.
    void PublishVoxelOverlay(const std::function<std::uint64_t(const EditorVoxelOverlay::Data&, std::string*)>& publish,
        const std::function<void()>& clear);
    // The frame's draw request while the overlay is shown and published.
    std::optional<EditorVoxelOverlay::Request> GetVoxelOverlayRequest() const;
    bool IsVoxelBakeRunning() const noexcept
    {
        return m_voxelBake.has_value();
    }
    struct ProbeCleanupRequest
    {
        std::set<std::string> liveReferences;
        std::vector<AssetID> confirmed;
    };
    std::optional<ProbeCleanupRequest> TakeProbeCleanupRequest()
    {
        return std::exchange(m_probeCleanupRequest, std::nullopt);
    }
    void OnProbeCleanupCompleted(std::size_t removed, const std::string& error);
    std::optional<ProbeBatchRequest> TakeProbeBatchRequest()
    {
        return std::exchange(m_probeBatchRequest, std::nullopt);
    }
    void OnProbeBatchRequestCompleted(IDemoScene& scene, const std::string& objectID, const std::string& reference, const std::string& error);
    std::optional<AssetBrowser::MeshImportUnitsRequest> TakeMeshImportUnitsRequest()
    {
        return m_assetBrowser.TakeMeshImportUnitsRequest();
    }

    void OnMeshImportUnitsCompleted(const std::string& error)
    {
        m_assetBrowser.OnMeshImportUnitsCompleted(error);
    }
    void OnMaterialCreationCompleted(
        const std::filesystem::path& relativePath,
        const std::string& error);

    const EditorWorkspaceRect& PrepareWorkspace(
        IDemoScene& demoScene,
        DemoSceneManager& sceneManager,
        DebugUI& debugUI);

    void Draw(
        IDemoScene& demoScene,
        DemoSceneManager& sceneManager,
        DebugUI& debugUI,
        std::uint64_t sceneTextureHandle,
        unsigned int sceneWidth,
        unsigned int sceneHeight,
        float deltaTime,
        const ProjectPaths& projectPaths,
        const ProjectAssetRegistry& assetRegistry,
        const std::optional<MaterialPreviewImage>& materialPreview);

    size_t GetSelectedObject() const
    {
        return m_selectedObject;
    }

    void SetViewportPublicationIdentity(std::uint64_t rendererLifetime, std::uint64_t bindingEpoch);

    // M0c.4 Stage 4.3: shown over the viewport while the renderer prepares (or cannot prepare) the requested configuration;
    // empty hides it.
    // progress in [0, 1] draws a bar under the status; a negative value draws none.
    void SetViewportPreparationStatus(std::string status, float progress)
    {
        m_viewportPreparationStatus = std::move(status);
        m_viewportPreparationProgress = progress;
    }

    // UPSCALING.3B Resolution Info overlay: the renderer's actual extents and presentation mode for this frame.
    struct ViewportResolutionInfo
    {
        unsigned int outputWidth = 0;
        unsigned int outputHeight = 0;
        unsigned int renderWidth = 0;
        unsigned int renderHeight = 0;
        // Renderer::GetPresentationModeName(); static storage.
        const char* mode = "";
    };

    void SetViewportResolutionInfo(const ViewportResolutionInfo& info) noexcept { m_viewportResolutionInfo = info; }
    void RefreshViewportMarqueePreview(const IDemoScene& scene);
    const std::vector<std::size_t>* GetViewportMarqueePreview(const IDemoScene& scene) const;
    const std::vector<size_t>& GetSelectedObjects() const noexcept { return m_selectedObjects; }

    // Diagnostics only (ENGINE_FRAME_HITCH_LOGGING=1): Draw records its phase durations for the frame-hitch report.
    void SetPhaseTimingEnabled(bool enabled) noexcept { m_phaseTimingEnabled = enabled; }
    const EditorPhaseTiming::Frame* GetPhaseTiming() const noexcept
    {
        return m_phaseTimingEnabled ? &m_phaseTiming : nullptr;
    }

    uint64_t GetSelectionStructureRevision() const
    {
        return m_selectionStructureRevision;
    }

private:
    bool m_compositePreview = false;
    std::filesystem::path m_compositeRebuildPath;
    bool SelectionIncludesLevelRoot(const IDemoScene& scene) const;

    // W5 refinement: each Local Level of a World is in Object Edit (default) or Level Edit mode. Editor-only state, never
    // serialized. In Level Edit the Local is edited as one placement: selecting any of its entities (viewport, marquee or
    // hierarchy) selects its level node, whose transform is the placement (translation and rotation only).
    std::unordered_map<std::string, bool> m_levelEditModes;
    bool IsLevelEditMode(std::string_view levelID) const;
    // The object a selection of objectID should target: the level node when objectID belongs to a Local in Level Edit.
    std::string ResolveLevelEditSelection(const FileBackedDemoScene& owner, const std::string& objectID) const;
    // Changes no transform and records no history. Entering Level Edit moves any selected entity of that Local to its node.
    void SetLevelEditMode(const IDemoScene& scene, const std::string& levelID, bool levelEdit);
    // The selected object's inspection (metadata and document), read once per frame and shared by every Inspector panel.
    // Reading it validates and captures the whole scene; each component panel used to repeat that every frame. The cached read
    // is discarded when the frame, the selection, the runtime or session, the scene structure or the history changes, so any
    // committed edit is seen by the next panel; edits themselves re-check expected values in the runtime.
    struct InspectionCache
    {
        int frame = -1;
        size_t selection = InvalidObjectSelection;
        const void* runtime = nullptr;
        std::uint64_t runtimeIdentity = 0;
        std::uint64_t structureRevision = 0;
        size_t historySize = 0;
        size_t historyCursor = 0;
        std::uint64_t historyEpoch = 0;
        bool succeeded = false;
        SceneObjectMetadata metadata;
        SceneObjectDocument object;
    };

    // A memoisation only (no observable state), so the const read paths can use it.
    mutable InspectionCache m_inspectionCache;
    // Availability of the assets the Inspector names and previews, shared with m_inspectorPanel (ResolveObjectInspection is const).
    mutable AssetAvailabilityCache m_assetAvailability;
    // UpdateSceneHistorySession runs every frame; the two project roots it compares are canonicalized only when they change.
    CanonicalPathCache m_historyProjectRoot;
    CanonicalPathCache m_historySceneProjectRoot;
    bool InspectSelection(const FileBackedDemoScene& owner, SceneObjectMetadata& metadata, SceneObjectDocument& object) const;
    std::optional<SceneObjectMetadata> SelectedObjectMetadata(const FileBackedDemoScene& owner) const;
    std::optional<std::optional<std::size_t>> m_compositeSaveRequest;
    std::string m_compositeSaveStatus;
    void DrawCompositeWorldInspection(FileBackedDemoScene& owner);
    void DrawCompositeDocuments(FileBackedDemoScene& owner, const CompositeWorld& composite);
    std::vector<bool> m_compositeDirty;
    int m_compositeDirtyCountdown = 0;
    static bool ImportBuiltinEnvironment(const ProjectPaths& paths, ProjectAssetRegistry& registry, const std::filesystem::path& source,
        std::string& reference, std::string& error);
#if defined(ENGINE_HAS_VERIFICATION)
    friend class PresentationTransformVerification;
#endif
    PhysicsState m_physicsState = PhysicsState::Stopped;
    bool m_canPlayPhysics = false;
    std::string m_physicsMessage;
    std::optional<PhysicsRequest> m_physicsRequest;
    void DrawPhysicsControls();
    GameState m_gameState = GameState::Editing;
    bool m_canPlayGame = false;
    std::string m_gameMessage;
    std::string m_viewportPreparationStatus;
    float m_viewportPreparationProgress = -1.0f;
    std::optional<GameRequest> m_gameRequest;
    void DrawGamePlayControls(const EditorIcons& icons, float centerX);
#if defined(ENGINE_HAS_VERIFICATION)
    friend class MaterialLocateVerification;
    friend class ViewportPanVerification;
    friend class ViewportGridSnapVerification;
    friend class EditorGridVerification;
    friend class BackgroundTasksVerification;
    friend class ProjectIndependentStartupVerification;
    friend class SceneLifecycleVerification;
    friend class AssetBrowserSceneVerification;
    friend class StartupSceneVerification;
    friend class SceneMembershipEditorVerification;
    friend class CompositeWorldEditorVerification;
    friend class DemoWorldEditorVerification;
    friend class CompositePlacementStress;
    friend class EditorCpuVerification;
    friend class RenderingVerification;
    friend class GamePlayGPUVerification;
    friend class MemoryRegistryVerification;
    friend class ScriptWorkflowVerification;
    friend class VoxelEditorVerification;
    friend class VoxelOverlayVerification;
    friend class VoxelStreamingVerification;
    friend class AirMotorEditorVerification;
    friend class ProjectSystemWorkflowVerification;
    friend class ProjectRelaunchVerification;
    friend class SceneDuplicationVerification;
    friend class ScenePhysicsDataVerification;
    friend class PrimitiveCreationVerification;
    friend class MarqueeSelectionVerification;
    friend class ViewportMaterialDropVerification;
    friend class DescriptorRecoveryVerification;
    friend class SceneHierarchyVerification;
#endif
    void DrawMaterialDropTarget(IDemoScene& scene, const ProjectPaths& paths,
        const ProjectAssetRegistry& registry);
    void QueueViewportMaterialDrop(const IDemoScene& scene, const SceneObjectMetadata& target, const AssetID& material);
    Renderer* m_materialDropRenderer = nullptr;

    // Borrowed from Engine; null until ConfigureMaterialThumbnails binds it.
    const ProjectContext* m_activeProject = nullptr;
    std::optional<SceneObjectMetadata> m_materialDropHover;
    std::string m_materialDropStatus;
    int m_materialDropDeliveryFrame = -1;
    std::optional<MaterialBindingRequest> m_viewportMaterialRequest;
    void DrawMeshDropTarget(const IDemoScene& scene, const ProjectPaths& paths,
        const ProjectAssetRegistry& registry);
    std::optional<MeshCreationRequest> m_meshCreationRequest;
    std::optional<PrimitiveCreationRequest> m_primitiveCreationRequest;
    std::optional<EmptyObjectCreationRequest> m_emptyObjectCreationRequest;
    std::optional<CameraObjectCreationRequest> m_cameraObjectCreationRequest;
    std::optional<LightObjectCreationRequest> m_lightObjectCreationRequest;
    std::optional<ProbeVolumeObjectCreationRequest> m_probeVolumeObjectCreationRequest;
    std::string m_meshCreationStatus;

    std::uint64_t m_meshDragRuntimeIdentity = 0;
    int m_meshDropDeliveryFrame = -1;

    void DrawHierarchy(IDemoScene& scene, const std::string& activeSceneName);
    void DrawSceneObjectDeletionControls(IDemoScene& scene);
    void DrawSceneObjectContextMenu(IDemoScene& scene);
    void OpenSceneObjectContext(const IDemoScene& scene, const SceneObjectMetadata& target);
    void QueueSceneObjectDeletion(const IDemoScene& scene, const SceneObjectMetadata& selection);
    bool SceneObjectDeletionMatches(const IDemoScene& scene, const SceneObjectMetadata& selection) const;
    bool SceneObjectDeletionBlocked() const;
    void CaptureViewportRightClick(const IDemoScene& scene);
    void FinishViewportRightClick(const IDemoScene& scene);

    std::optional<SceneObjectMetadata> m_viewportRightObject;
    std::optional<SceneObjectMetadata> m_sceneContextObject;
    std::uint64_t m_sceneContextEpoch = 0;
    std::optional<SceneObjectMetadata> m_sceneObjectDeletionRequest;
    std::optional<SceneObjectMetadata> m_sceneObjectDeletionPending;
    std::optional<SceneObjectMetadata> m_sceneObjectDeletionStatusObject;
    std::string m_sceneObjectDeletionStatus;

    bool m_viewportRightGesture = false;
    bool m_viewportRightDragged = false;
    bool m_openSceneContext = false;

    void DrawSceneViewport(
        std::uint64_t sceneTextureHandle,
        unsigned int sceneWidth,
        unsigned int sceneHeight);

    void DrawMainMenu(
        const IDemoScene& demoScene,
        DemoSceneManager& sceneManager,
        DebugUI& debugUI);

    enum class MaterialLeaveAction
    {
        None,
        Selection,
        ObjectSelection,
        Refresh,
        Hide,
        Scene,
        Close
    };

    enum class InspectorTarget { None, Asset, SceneObject };
    InspectorTarget m_inspectorTarget = InspectorTarget::None;

    bool m_objectInspectionRequested = false;
    struct ObjectInspectionIdentity
    {
        const Scene* scene = nullptr; // Identity only; never dereferenced after scene replacement.
        std::uint64_t revision = 0;
        std::uint64_t runtimeIdentity = 0;

        std::string objectID;
        std::string sceneID;
    };
    std::optional<ObjectInspectionIdentity> m_objectInspectionIdentity;

    std::vector<SceneMaterialSavedRevision> m_savedMaterialRevisions;

    std::vector<SceneMaterialWorkingRevision> m_workingMaterialRevisions;

    std::optional<MaterialAssignmentRequest> m_materialAssignmentRequest;

    std::optional<SceneObjectMetadata> m_materialAssignmentSelection;
    std::optional<std::string> m_materialAssignmentSlotID;

    bool m_materialAssignmentPending = false;

    std::string m_materialAssignmentStatus;

    void ActivateObjectInspection(IDemoScene& demoScene, const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    std::optional<SceneObjectInspectorView> ResolveObjectInspection(
        const IDemoScene& demoScene, const ProjectPaths& paths, const ProjectAssetRegistry& registry) const;

    MaterialLeaveAction m_materialLeaveAction = MaterialLeaveAction::None;

    std::filesystem::path m_inspectedAssetSelection;
    std::filesystem::path m_pendingAssetSelection;

    bool m_applicationCloseApproved = false;

    bool m_showBuildGame=false, m_buildAdvanced=false, m_buildGameCancel=false;
    char m_buildTitle[128]="MyGame";
    char m_buildOutput[1024]="";
    BuildGame::Status m_buildGameStatus;
    std::optional<BuildGameRequest> m_buildGameRequest;
    bool m_buildStartupSceneUnsaved = false;
    void DrawBuildGame(const IDemoScene& demoScene);
    std::optional<FileRequest> m_fileRequest;

    bool m_sceneOperationPending = false;
    bool m_showProjectSettings = false;
    std::string m_lifecycleMessage;
    void DrawLifecycleWindows(const IDemoScene& demoScene, const DebugUI& debugUI);

    // Project Settings: shared game startup scene. Candidates are listed from the Scenes directory
    // when the window opens or on Refresh, never per frame and without reading scene documents.
    std::optional<std::filesystem::path> m_startupSceneRequest;
    std::vector<std::filesystem::path> m_startupSceneCandidates;
    bool m_startupSceneCandidatesListed = false;
    std::string m_projectSettingsMessage;
    bool m_projectSettingsMessageIsError = false;
    void DrawProjectSettings(const IDemoScene& demoScene, const DebugUI& debugUI);
    void ListStartupSceneCandidates();

    // PS.3 Project Settings draft. The startup scene above is a separate, immediately saved legacy section.
    enum class ProjectSettingsCloseAction
    {
        None,
        // The window's close button with unsaved changes.
        CloseWindow,
        // A held Exit, New Project or Open Project request with unsaved changes.
        LeaveProject
    };

    std::optional<ProjectSettings> m_projectSettingsDraft;
    std::optional<ProjectSettings> m_projectSettingsSaveRequest;
    bool m_projectSettingsSavePending = false;
    ProjectSettingsCloseAction m_projectSettingsCloseAction = ProjectSettingsCloseAction::None;
    // The close action to finish once the requested save succeeds.
    ProjectSettingsCloseAction m_projectSettingsCloseAfterSave = ProjectSettingsCloseAction::None;
    std::size_t m_projectSettingsCategory = 0;
    std::vector<std::string> m_projectSettingsCorrections;
    std::string m_renderingSettingsMessage;
    bool m_renderingSettingsMessageIsError = false;
    void DrawStartupSceneSection(const IDemoScene& demoScene);
    void DrawRenderingSettingsCategory(const DebugUI& debugUI, bool editable);
    // SA.2B: Steam Audio enablement (audio.steamAudio.enabled) and this session's runtime state.
    void DrawAudioSettingsCategory(const DebugUI& debugUI, bool editable);
    void DrawRenderingRuntimeStatus(const DebugUI& debugUI);
    void DrawProjectSettingsCloseConfirmation();
    void RequestProjectSettingsSave(ProjectSettingsCloseAction closeAfterSave);
    void CloseProjectSettings();
    // Why Save is unavailable; empty when it is available.
    std::string ProjectSettingsSaveBlocker() const;

    // Windows .engineproj association: a personal Windows preference, separate from the project.
    bool m_showFileAssociation = false;
    bool m_fileAssociationStatusRead = false;
    bool m_fileAssociationConfirmReplace = false;
    ProjectFileAssociation::Status m_fileAssociationStatus;
    std::filesystem::path m_fileAssociationEffectiveExecutable;
    std::string m_fileAssociationMessage;
    void DrawFileAssociation();
    void RefreshFileAssociationStatus();

    std::string m_projectOwnershipMessage;
    OwnershipAction m_ownershipAction = OwnershipAction::None;
    void DrawProjectOwnershipLost();
    ProjectFileAssociation::RegistryLocation FileAssociationLocation() const;
#if defined(ENGINE_HAS_VERIFICATION)
    // Diagnostics drive the real window against an isolated registry key, never the user's classes.
    std::optional<ProjectFileAssociation::RegistryLocation> m_fileAssociationLocationForVerification;
#endif
    bool m_showEditorPanel = false;

    bool m_phaseTimingEnabled = false;
    // Diagnostics only: mutable so that const inspection helpers can record their phases.
    mutable EditorPhaseTiming::Frame m_phaseTiming{};

    EditorPhaseTiming::Frame* PhaseTiming() const noexcept { return m_phaseTimingEnabled ? &m_phaseTiming : nullptr; }

    AssetBrowser m_assetBrowser;
    SceneMembershipPanel m_sceneMembership;
    bool m_environmentWorldChanged = false;

    EditorWorkspace m_workspace;
    BackgroundTasksUI m_backgroundTasksUI;
    BackgroundTaskManager* m_backgroundTasks = nullptr;
    AcousticGeometryReadiness m_acousticGeometryReadiness;
    // Why the scene's acoustic geometry cannot be read yet, or nullopt.
    std::optional<std::string> AcousticGeometryNotReady(const IDemoScene& scene) const;
    SceneHierarchyPanel m_hierarchyPanel;

    // The Hierarchy's retained inputs. The runtime returns the same immutable snapshot until the scene's storage, document or
    // bindings change; holding it here keeps its address unique, so a new address is a new generation for the panel. The
    // object icons are derived from the same document state and rebuilt only with it (or when the composite World changes).
    struct HierarchyView
    {
        std::shared_ptr<const SceneHierarchySnapshot> snapshot;
        std::uint64_t generation = 0;
        std::uint64_t iconGeneration = 0;
        const void* iconComposite = nullptr;
        std::unordered_map<std::string_view, EditorIcon> icons;
    };

    HierarchyView m_hierarchyView;

    InspectorPanel m_inspectorPanel;

    static constexpr size_t InvalidObjectSelection = (std::numeric_limits<size_t>::max)();

    enum class GizmoMode
    {
        Translate,
        Rotate,
        Scale
    };

    GizmoMode m_gizmoMode = GizmoMode::Translate;

    bool m_cameraLooking = false;
    bool m_cameraPanning = false;
    // Keep a pan's held left button from becoming a gizmo gesture after RMB release.
    bool m_panLeftHeld = false;
    // Session display preference: never reset with scene interactions or authored history.
    bool m_showPhysicsColliders = true;
    // CAM.5 Scene viewport Camera gizmos; session display preference like the collider overlay.
    bool m_showCameraFrusta = true;
    // Session-only display preference like Camera Frusta; never authored or persisted.
    bool m_showLightWireframes = true;
    // AIR.4A Air window.
    void DrawAirWindow(IDemoScene& scene, DebugUI& debugUI);
    // R1.3: the Air window's view of the project's Volumetrics settings, edited as session rendering overrides.
    void DrawVolumetricQualityControls(DebugUI& debugUI);
    // AIR.4F F4.4.1: the Air window's scene-owned Mist rendering controls.
    void DrawMistRenderingControls(IDemoScene& scene);
    // AIR.4F F4.5.1b: the scene's air environment (ambient temperature, thermal relaxation time).
    void DrawAirEnvironmentControls(IDemoScene& scene);
    bool m_showAirWindow = false;
    // Physical sky S1: the Level > Sky window (SkyControls.cpp), HDRP's Visual Environment and Physically Based Sky for the open
    // document's Sky group. Its visibility is an Editor preference (GetWindowPreferences).
    void DrawSkyWindow(IDemoScene& scene);
    // S4: HDRP's HDRI Sky section (the HDRI, its luminance and its rotation), editing the open scene's Sky group.
    void DrawSkyHdriSection(IDemoScene& scene, bool editable);
    // Physical sky S2: the Sky window's Sun section (the scene's sun owner: its elevation and azimuth, one transform history
    // entry per drag, and its atmosphere flag).
    void DrawSkySunSection(IDemoScene& scene, FileBackedDemoScene& owner);
    bool m_showSkyWindow = false;
    AirEditorSettings m_airSettings;
    bool m_airResetRequested = false;
    bool m_mistClearRequested = false;
    bool m_showAirMotorWireframes = true;
    bool m_showLightIcons = true;
    bool m_showAudioIcons = true;
    bool m_showCameraIcons = true;
    std::vector<ViewportIcons::Icon> m_viewportIcons;
    // Audio.2B: facing direction of each visible Audio Listener icon; the active listener is highlighted.
    struct ViewportListenerFacing
    {
        ImVec2 start;
        ImVec2 end;
        bool active = false;
    };
    std::vector<ViewportListenerFacing> m_viewportListenerFacings;
    // The active listener's runtime slot: its icon is tinted, since a facing line along the view is foreshortened.
    std::optional<size_t> m_viewportActiveListener;
    ImDrawListSplitter m_viewportIconLayers;
    const Scene* m_viewportIconScene = nullptr;
    std::uint64_t m_viewportIconRuntime = 0;
    std::uint64_t m_viewportIconRevision = 0;
    int m_viewportIconFrame = -1;

    // The objects that show viewport icons and their artwork. Finding them scans every document object and hashes each
    // icon object's id, so the list is kept until the document, its structure or the icon toggles change; positions are
    // still read every frame.
    struct ViewportIconSource
    {
        size_t objectIndex = 0;
        std::optional<EditorIcon> cameraOrLight;
        bool audioSource = false;
        bool audioListener = false;
        bool activeListener = false;
    };

    struct ViewportIconSourceKey
    {
        const Scene* scene = nullptr;
        std::uint64_t runtimeIdentity = 0;
        std::uint64_t structureRevision = 0;
        std::uint64_t editRevision = 0;
        const void* documentObjects = nullptr;
        size_t documentObjectCount = 0;
        bool cameraIcons = false;
        bool lightIcons = false;
        bool audioIcons = false;

        bool operator==(const ViewportIconSourceKey&) const = default;
    };

    std::vector<ViewportIconSource> m_viewportIconSources;
    std::optional<ViewportIconSourceKey> m_viewportIconSourceKey;
    void BuildViewportIcons(const IDemoScene& scene, const EditorIcons& icons);
    std::optional<size_t> PickViewportIcon(const IDemoScene& scene, ImVec2 mouse) const;
    void DrawViewportTools(IDemoScene& scene, const EditorIcons& icons, bool viewportWasFocused);
    // Display preference, independent of Snap; default visible. Persisted in Editor preferences.
    bool m_showViewportGrid = true;
    // UPSCALING.3B Resolution Info overlay: session-only, hidden by default, never persisted.
    bool m_showResolutionInfo = false;
    ViewportResolutionInfo m_viewportResolutionInfo;
    // Gizmo snapping (G.1 translation, G.2 rotation). One master toggle drives both modes; each
    // has its own interval. Persisted only in global Editor preferences (PR.5), never in scenes,
    // cooked payloads or history.
    bool m_snapEnabled = false;
    float m_translationSnapInterval = ViewportGridSnap::kDefaultInterval;
    float m_rotationSnapIntervalDegrees = ViewportGridSnap::kDefaultRotationIntervalDegrees;
    // Text-entry values; only a finite positive draft replaces the applied interval.
    float m_translationSnapIntervalDraft = ViewportGridSnap::kDefaultInterval;
    float m_rotationSnapIntervalDraft = ViewportGridSnap::kDefaultRotationIntervalDegrees;
    EditorWorkspaceRect m_viewportContentRegion;
    void DrawResolutionInfo(const ImVec2& position) const;
    void DrawSceneViewportToolbar(const EditorWorkspaceRect& toolbarRectangle, const EditorIcons& icons);
    void DrawGridSnapControls(float rightEdge);
    Vector3 SnapGizmoTranslation(const Vector3& dragStart, const Vector3& continuousPosition) const;
    bool IsRotationSnapActive() const;
    void UpdateGizmoRotationDrag(bool gizmoUsing, const Matrix4& gizmoModel, const Vector3& authoredRotation);

    // Transient state of one gizmo rotation drag. ImGuizmo rotates incrementally from the
    // matrix it receives, so during a drag it is fed continuousModel (its own previous output,
    // ImGuizmo layout) rather than the snapped authored rotation. Cleared whenever the gesture
    // ends or is reset; startRotation is the authored drag-start rotation of the active object.
    struct GizmoRotationDrag
    {
        Matrix4 continuousModel;
        Vector3 startRotation;
        ViewportGridSnap::RotationDrag tracker;
    };
    std::optional<GizmoRotationDrag> m_gizmoRotationDrag;
    // Euler rotation for the active object this frame: snapped when snapping is on, otherwise
    // the existing decomposition of the continuous gizmo output.
    Vector3 ResolveGizmoRotation(const Vector3& continuousRotationRadians) const;
    bool m_viewportHovered = false;
    bool m_viewportFocused = false;

    EditorWorkspaceRect m_viewportImage;

    float m_cameraYaw = 0.0f;
    float m_cameraPitch = 0.0f;

    struct PersistentHistoryTarget { std::string objectID; };
    struct ProceduralHistoryTarget { size_t objectIndex = InvalidObjectSelection; };
    using HistoryTarget = std::variant<PersistentHistoryTarget, ProceduralHistoryTarget>;
    struct TransformEdit
    {
        HistoryTarget target;
        Transform before;
        Transform after;
    };
    struct GroupTransformEdit
    {
        std::vector<TransformEdit> edits;
        GizmoMode mode = GizmoMode::Translate;
    };
    using SelectedObjectIdentity = SceneSelection::Identity;
    std::optional<ViewportMarquee::Gesture> m_marqueeGesture;
    std::uint64_t m_viewportRendererLifetime = 0;
    std::uint64_t m_viewportBindingEpoch = 0;
    void CancelViewportMarquee();
    bool ViewportMarqueeMatches(const IDemoScene& scene) const;
    void StartViewportMarquee(const IDemoScene& scene, ImVec2 imageMouse);
    bool UpdateViewportMarquee(IDemoScene& scene);
    void DrawViewportMarquee() const;
    std::vector<SelectedObjectIdentity> m_orderedSelection;
    std::vector<size_t> m_selectedObjects;
    void SelectSceneObject(const IDemoScene& scene, size_t index, bool toggle = false);
    void RefreshSceneSelection(const IDemoScene& scene);
    void UpdateGroupGizmo(IDemoScene& scene, bool usingGizmo, const Transform& anchorAfter);
    std::unique_ptr<GroupTransformEdit> m_gizmoGroupEdit;
    size_t m_gizmoGroupPayloadBytes = 0;
    struct SceneHistorySession
    {
        std::filesystem::path projectIdentity;
        std::filesystem::path scenePath;
        std::string sceneIdentity;
        std::uint64_t epoch = 0;
    };
    struct ObjectDeletion
    {
        SceneObjectDocument object;
        size_t documentIndex = 0;
        std::optional<std::string> selectionBefore;
        std::optional<std::string> selectionAfter;
        SceneHistorySession session;
        bool insertion = false;
        std::optional<SceneHierarchyEdit> hierarchy;
    };
    struct PhysicsComponentEdit
    {
        SceneHistorySession session;
        std::string objectID;
        std::optional<Physics::PhysicsComponent> before;
        std::optional<Physics::PhysicsComponent> after;
    };
    void DrawObjectPhysicsControls(IDemoScene& scene);
    bool CommitObjectPhysics(IDemoScene& scene, const SceneObjectMetadata& selection,
        const std::optional<Physics::PhysicsComponent>& before,
        const std::optional<Physics::PhysicsComponent>& after);
    std::optional<SceneObjectMetadata> m_physicsInspection;
    std::optional<Physics::PhysicsComponent> m_physicsInspectedValue;
    std::uint64_t m_physicsInspectionEpoch = 0;
    // One drag draft per numeric physics field, in DrawObjectPhysicsControls' field order.
    // Drafts hold displayed units (degrees for rotation) and reach the scene only on commit.
    std::array<float, 15> m_physicsDragDrafts{};
    std::string m_physicsEditStatus;
    struct TriggerColliderEdit
    {
        SceneHistorySession session;
        std::string objectID;
        std::optional<Physics::TriggerColliderComponent> before;
        std::optional<Physics::TriggerColliderComponent> after;
    };
    void DrawObjectTriggerColliderControls(IDemoScene& scene);
    bool CommitObjectTriggerCollider(IDemoScene& scene, const SceneObjectMetadata& selection,
        const std::optional<Physics::TriggerColliderComponent>& before,
        const std::optional<Physics::TriggerColliderComponent>& after);
    std::optional<SceneObjectMetadata> m_triggerInspection;
    std::optional<Physics::TriggerColliderComponent> m_triggerInspectedValue;
    std::uint64_t m_triggerInspectionEpoch = 0;
    // Displayed units, committed only when the existing numeric edit transaction completes.
    std::array<float, 12> m_triggerDragDrafts{};
    std::string m_triggerEditStatus;
    std::uint64_t m_triggerControlsRevision = 0;
    struct CharacterControllerEdit
    {
        SceneHistorySession session;
        std::string objectID;
        std::optional<Physics::CharacterControllerComponent> before;
        std::optional<Physics::CharacterControllerComponent> after;
    };
    void DrawObjectCharacterControllerControls(IDemoScene& scene);
    bool CommitObjectCharacterController(IDemoScene& scene, const SceneObjectMetadata& selection,
        const std::optional<Physics::CharacterControllerComponent>& before,
        const std::optional<Physics::CharacterControllerComponent>& after);
    std::optional<SceneObjectMetadata> m_controllerInspection;
    std::optional<Physics::CharacterControllerComponent> m_controllerInspectedValue;
    std::uint64_t m_controllerInspectionEpoch = 0;
    // Displayed units, committed only when the existing numeric edit transaction completes.
    std::array<float, 11> m_controllerDragDrafts{};
    std::string m_controllerEditStatus;
    std::uint64_t m_controllerControlsRevision = 0;
    // One logical Camera action. Make Main carries both the previous and new Main, so undo and
    // redo restore every affected value through one atomic, expected-value-checked runtime edit.
    struct CameraChange
    {
        std::string objectID;
        std::optional<CameraComponent> before;
        std::optional<CameraComponent> after;
    };
    struct CameraEdit
    {
        SceneHistorySession session;
        std::vector<CameraChange> changes;
    };
    void DrawObjectCameraControls(IDemoScene& scene);
    // Each change's selection is resolved by the caller this frame; nothing is retained.
    bool CommitObjectCameras(IDemoScene& scene, std::span<const SceneCameraChange> changes);
    bool CommitObjectCamera(IDemoScene& scene, const SceneObjectMetadata& selection,
        const std::optional<CameraComponent>& before, const std::optional<CameraComponent>& after);
    // Designates the selected Camera Main (clearing any other Main) or clears its designation.
    bool CommitCameraMainDesignation(IDemoScene& scene, const SceneObjectMetadata& selection, bool main);
    static size_t CameraEditPayloadBytes(const CameraEdit& edit);
    void ValidateCameraManagementPopup(IDemoScene& scene);
    void DismissCameraManagementPopup();
    std::optional<SceneObjectMetadata> m_cameraInspection;
    std::optional<CameraComponent> m_cameraInspectedValue;
    std::uint64_t m_cameraInspectionEpoch = 0;
    // Displayed units (FOV in degrees), committed only when a numeric edit transaction completes.
    std::array<float, 3> m_cameraDragDrafts{};
    std::string m_cameraEditStatus;
    std::uint64_t m_cameraControlsRevision = 0;
    // LIGHT.1: one complete Light replacement (add, edit, type switch or remove) per entry.
    struct LightEdit
    {
        SceneHistorySession session;
        std::string objectID;
        std::optional<LightComponent> before;
        std::optional<LightComponent> after;
    };
    // AIR.3: one complete Air Motor replacement (add, edit, type switch or remove) per entry (AirMotorControls.cpp).
    struct AirMotorEdit
    {
        SceneHistorySession session;
        std::string objectID;
        std::optional<AirMotorComponent> before;
        std::optional<AirMotorComponent> after;
    };
    void DrawObjectAirMotorControls(IDemoScene& scene);
    bool CommitObjectAirMotor(IDemoScene& scene, const SceneObjectMetadata& selection,
        const std::optional<AirMotorComponent>& before, const std::optional<AirMotorComponent>& after);
    static size_t AirMotorEditPayloadBytes(const AirMotorEdit& edit);
    void ValidateAirMotorManagementPopup(IDemoScene& scene);
    void DismissAirMotorManagementPopup();
    // AIR.4F F2: one complete Mist Emitter replacement (add, edit or remove) per entry (MistEmitterControls.cpp).
    struct MistEmitterEdit
    {
        SceneHistorySession session;
        std::string objectID;
        std::optional<MistEmitterComponent> before;
        std::optional<MistEmitterComponent> after;
    };
    void DrawObjectMistEmitterControls(IDemoScene& scene);
    // AIR.4F F4.4.1: one edit of the scene's mist appearance (the Air window's Mist rendering controls).
    struct MistRenderingEdit
    {
        SceneHistorySession session;
        std::optional<MistRenderSettings> before;
        std::optional<MistRenderSettings> after;
    };
    bool CommitMistRendering(IDemoScene& scene, const std::optional<MistRenderSettings>& before, const MistRenderSettings& after,
        std::string* error = nullptr);
    bool ReplayMistRendering(IDemoScene& scene, const MistRenderingEdit& edit, bool redo, std::string& error);
    static size_t MistRenderingEditPayloadBytes(const MistRenderingEdit& edit);
    // While a slider is held: the value shown and drawn for the scene runtime `runtimeIdentity`, committed as one edit on release.
    // Never saved and never a second source of truth: it is dropped on commit and on undo or redo, and applies to no other runtime (a
    // reopened or switched scene has a new identity).
    struct MistRenderingDraft
    {
        std::uint64_t runtimeIdentity = 0;
        MistRenderSettings settings;
    };
    std::optional<MistRenderingDraft> m_mistRenderingDraft;
    std::string m_mistRenderingStatus;
    // AIR.4F F4.5.1b: one edit of the scene's air environment (the Air window's Environment controls). Fields commit when their input
    // is finished, so there is no draft.
    // W3: one Local Level environment override switch (FileBackedDemoScene::EnvironmentGroup as an integer).
    struct EnvironmentOverrideEdit
    {
        SceneHistorySession session;
        int group = 0;
        bool before = false;
        bool after = false;
    };
    bool CommitEnvironmentOverride(IDemoScene& scene, int group, bool enabled, std::string* error = nullptr);
    bool ReplayEnvironmentOverride(IDemoScene& scene, const EnvironmentOverrideEdit& edit, bool redo, std::string& error);
    // W3: the open scene's World/Local status and its six environment override switches.
    void DrawEnvironmentOverrideControls(IDemoScene& scene);
    // W3: the open Local Level inherits this group (FileBackedDemoScene::EnvironmentGroup): its editing controls are not offered.
    static bool IsEnvironmentGroupInherited(IDemoScene& scene, int group);
    std::string m_environmentOverrideStatus;

    struct AirEnvironmentEdit
    {
        SceneHistorySession session;
        std::optional<AirEnvironmentSettings> before;
        std::optional<AirEnvironmentSettings> after;
    };
    bool CommitAirEnvironment(IDemoScene& scene, const std::optional<AirEnvironmentSettings>& before, const AirEnvironmentSettings& after,
        std::string* error = nullptr);
    bool ReplayAirEnvironment(IDemoScene& scene, const AirEnvironmentEdit& edit, bool redo, std::string& error);
    static size_t AirEnvironmentEditPayloadBytes(const AirEnvironmentEdit& edit);
    std::string m_airEnvironmentStatus;
    // R1.1: one edit of the scene's AIR simulation configuration (the Air window's simulation controls in a stopped scene). A checkbox,
    // combo or preset commits at once; a drag shows its value in a draft and commits on release, so one drag is one history entry.
    // Session overrides are never recorded.
    struct AirSimulationEdit
    {
        SceneHistorySession session;
        std::optional<AirSimulationSettings> before;
        std::optional<AirSimulationSettings> after;
    };
    bool CommitAirSimulation(IDemoScene& scene, const std::optional<AirSimulationSettings>& before, const AirSimulationSettings& after,
        std::string* error = nullptr);
    bool ReplayAirSimulation(IDemoScene& scene, const AirSimulationEdit& edit, bool redo, std::string& error);
    static size_t AirSimulationEditPayloadBytes(const AirSimulationEdit& edit);
    // The settings the Air window shows for `scene`: the session override while scene editing is locked, else a held drag's draft,
    // else the scene's (its defaults without a block or a project scene).
    AirSimulationSettings GetDisplayedAirSimulation(IDemoScene& scene) const;
    // Applies the Air window's edit: to the session override while scene editing is locked, else to the scene through history
    // (`commit`) or to the drag draft.
    void ApplyAirSimulationEdit(IDemoScene& scene, const AirSimulationSettings& edited, bool commit);
    void DiscardAirSimulationOverride() noexcept;
    // Both keyed by the scene runtime identity, so neither applies to a reopened or switched scene.
    struct AirSimulationSessionValue
    {
        std::uint64_t runtimeIdentity = 0;
        AirSimulationSettings settings;
    };
    std::optional<AirSimulationSessionValue> m_airSimulationOverride;
    std::optional<AirSimulationSessionValue> m_airSimulationDraft;
    std::string m_airSimulationStatus;
    // AIR.4F.2d.3: one edit of the scene's environmental haze (the Scene Inspector's Environmental Haze section). Fields commit when their
    // input is finished, as the air environment's.
    struct EnvironmentalHazeEdit
    {
        SceneHistorySession session;
        std::optional<EnvironmentalHazeSettings> before;
        std::optional<EnvironmentalHazeSettings> after;
    };
    void DrawEnvironmentalHazeControls(IDemoScene& scene);
    bool CommitEnvironmentalHaze(IDemoScene& scene, const std::optional<EnvironmentalHazeSettings>& before,
        const EnvironmentalHazeSettings& after, std::string* error = nullptr);
    bool ReplayEnvironmentalHaze(IDemoScene& scene, const EnvironmentalHazeEdit& edit, bool redo, std::string& error);
    static size_t EnvironmentalHazeEditPayloadBytes(const EnvironmentalHazeEdit& edit);
    std::string m_environmentalHazeStatus;
    // Physical sky S1: one edit of the open document's sky type and physical sky (the Sky window), as the environmental haze's. A
    // combo, checkbox or reset commits at once; a drag re-bakes live from a draft and commits on release, so one drag or one typed
    // value is one history entry.
    struct SkyEdit
    {
        SceneHistorySession session;
        SceneSky before;
        SceneSky after;
    };
    bool CommitSky(IDemoScene& scene, const SceneSky& before, const SceneSky& after, std::string* error = nullptr);
    bool ReplaySky(IDemoScene& scene, const SkyEdit& edit, bool redo, std::string& error);
    static size_t SkyEditPayloadBytes(const SkyEdit& edit);
    // While a Sky window drag is held: the physical sky drawn for the scene runtime `runtimeIdentity`. Never saved and never a
    // second source of truth: it is dropped on commit, cancel, undo and redo, and applies to no other runtime.
    struct PhysicalSkyDraft
    {
        std::uint64_t runtimeIdentity = 0;
        PhysicalSkySettings settings;
    };
    std::optional<PhysicalSkyDraft> m_physicalSkyDraft;
    std::string m_skyStatus;
    // The Sun section's drag (elevation, azimuth in degrees) and its transform history capture.
    bool m_skyUpdateRequested = false;
    // S5: the cloud presets (assets/Sky/CloudPresets.json, loaded on first use) for the Sky window's Clouds section.
    std::vector<CloudPreset> m_cloudPresets;
    std::string m_cloudPresetError;
    bool m_cloudPresetsLoaded = false;
    // S4: the HDRI Sky picker's list (the Renderer's, in its order) and request.
    struct SkyHdriChoice
    {
        std::string displayName;
        std::string reference;
        // A built-in HDRI, imported into the project when chosen.
        bool builtin = false;
    };
    std::vector<SkyHdriChoice> m_skyHdriChoices;
    std::optional<size_t> m_skyHdriRequest;
    std::array<float, 2> m_skySunDrafts{};
    bool m_skySunEditing = false;
    Transform m_skySunUndoStart;
    std::optional<HistoryTarget> m_skySunHistoryTarget;
    std::uint64_t m_skySunHistoryEpoch = 0;
    bool CommitObjectMistEmitter(IDemoScene& scene, const SceneObjectMetadata& selection,
        const std::optional<MistEmitterComponent>& before, const std::optional<MistEmitterComponent>& after);
    static size_t MistEmitterEditPayloadBytes(const MistEmitterEdit& edit);
    void ValidateMistEmitterManagementPopup(IDemoScene& scene);
    void DismissMistEmitterManagementPopup();
    std::optional<SceneObjectMetadata> m_mistEmitterInspection;
    std::optional<MistEmitterComponent> m_mistEmitterInspectedValue;
    std::uint64_t m_mistEmitterInspectionEpoch = 0;
    // Drafts of the Inspector's numeric fields, in MistEmitterControls.cpp's field order.
    std::array<float, 24> m_mistEmitterDragDrafts{};
    std::map<std::uint64_t, MistEmitterStatus> m_mistEmitterStatus;
    std::string m_mistEmitterEditStatus;
    std::uint64_t m_mistEmitterControlsRevision = 0;
    std::optional<SceneObjectMetadata> m_airMotorInspection;
    std::optional<AirMotorComponent> m_airMotorInspectedValue;
    std::uint64_t m_airMotorInspectionEpoch = 0;
    // Strength, falloff, half extents X, Y, Z, radius, length, inward pull, coupling, emission origin X, Y, Z, emission radius
    // (AirMotorControls.cpp).
    std::array<float, 18> m_airMotorDragDrafts{};
    std::map<std::uint64_t, AirMotorEmissionStatus> m_airMotorEmission;
    std::uint32_t m_airStepCount = 0;
    std::string m_airMotorEditStatus;
    std::uint64_t m_airMotorControlsRevision = 0;
    void DrawObjectLightControls(IDemoScene& scene);
    bool CommitObjectLight(IDemoScene& scene, const SceneObjectMetadata& selection,
        const std::optional<LightComponent>& before, const std::optional<LightComponent>& after);
    static size_t LightEditPayloadBytes(const LightEdit& edit);
    void ValidateLightManagementPopup(IDemoScene& scene);
    void DismissLightManagementPopup();
    std::optional<SceneObjectMetadata> m_lightInspection;
    std::optional<LightComponent> m_lightInspectedValue;
    std::uint64_t m_lightInspectionEpoch = 0;
    // Displayed units (angles in degrees): intensity (lm or lux), range, inner and outer cone angle, angular radius.
    std::array<float, 5> m_lightDragDrafts{};
    // Color gesture draft: edited live in the widget, committed once when the gesture ends.
    Vector3 m_lightColorDraft{1.0f, 1.0f, 1.0f};
    bool m_lightColorGestureActive = false;
    std::string m_lightEditStatus;
    std::uint64_t m_lightControlsRevision = 0;
    // AS.5: one complete Script component replacement (add, asset, class or enabled edit, remove) per entry.
    struct ScriptEdit
    {
        SceneHistorySession session;
        std::string objectID;
        std::optional<ScriptComponent> before;
        std::optional<ScriptComponent> after;
    };
    void DrawObjectScriptControls(IDemoScene& scene, const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    bool CommitObjectScript(IDemoScene& scene, const SceneObjectMetadata& selection,
        const std::optional<ScriptComponent>& before, const std::optional<ScriptComponent>& after);
    static size_t ScriptEditPayloadBytes(const ScriptEdit& edit);
    void ValidateScriptManagementPopup(IDemoScene& scene);
    void DismissScriptManagementPopup();
    void DrawScriptPickerContents(IDemoScene& scene, const SceneObjectMetadata& selection, const ScriptComponent& script,
        const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    // Eligible EAScript classes of an assigned script, validated on demand (assignment, a new
    // inspection target or Refresh), never every frame.
    void RefreshScriptClasses(const std::string& reference, const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    std::optional<SceneObjectMetadata> m_scriptInspection;
    std::optional<ScriptComponent> m_scriptInspectedValue;
    std::uint64_t m_scriptInspectionEpoch = 0;
    std::string m_scriptEditStatus;
    std::uint64_t m_scriptControlsRevision = 0;
    std::array<char, 128> m_scriptSearch{};
    struct ScriptClassList
    {
        std::string reference;
        std::vector<std::string> classes;
        std::string error;
    };
    std::optional<ScriptClassList> m_scriptClasses;
    std::unique_ptr<Scripting::ScriptHost> m_scriptValidator;
    // Audio.2B: one complete replacement of an object's Audio Source and Audio Listener (add, field edit, remove of
    // either) per entry.
    struct AudioComponentsEdit
    {
        SceneHistorySession session;
        std::string objectID;
        AudioComponentState before;
        AudioComponentState after;
    };
    // M4: one complete replacement of a mesh object's inherent acoustic settings per entry (AcousticControls.cpp).
    struct AcousticSettingsEdit
    {
        SceneHistorySession session;
        std::string objectID;
        std::optional<AcousticSettings> before;
        std::optional<AcousticSettings> after;
    };
    void DrawObjectAcousticControls(IDemoScene& scene, const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    void DrawAcousticSubmeshField(IDemoScene& scene, const SceneObjectMetadata& selection, const std::optional<AcousticSettings>& before,
        const std::string& submeshKey, const std::string& label, const ProjectPaths& paths, const ProjectAssetRegistry& registry, bool blocked);
    void DrawAcousticMaterialPicker(IDemoScene& scene, const SceneObjectMetadata& selection, const std::optional<AcousticSettings>& before,
        const std::string& submeshKey, const ProjectAssetRegistry& registry);
    bool CommitObjectAcoustics(IDemoScene& scene, const SceneObjectMetadata& selection, const std::optional<AcousticSettings>& before,
        const std::optional<AcousticSettings>& after);
    bool AssignAcousticMaterialReference(IDemoScene& scene, const SceneObjectMetadata& selection, const std::optional<AcousticSettings>& before,
        const std::string& submeshKey, std::string_view reference, const ProjectAssetRegistry& registry);
    static size_t AcousticEditPayloadBytes(const AcousticSettingsEdit& edit);
    // VX.4C.1: the voxel settings of one or more objects (a bulk edit is one entry), and the scene's voxel bake settings and
    // manifest reference (a settings edit or a published bake is one entry), each a complete before/after replacement.
    struct VoxelObjectChange
    {
        std::string objectID;
        std::optional<VoxelSettings> before;
        std::optional<VoxelSettings> after;
    };
    struct VoxelSettingsEdit
    {
        SceneHistorySession session;
        std::vector<VoxelObjectChange> changes;
    };
    struct VoxelBakeEdit
    {
        SceneHistorySession session;
        std::optional<VoxelBakeSettings> before;
        std::optional<VoxelBakeSettings> after;
    };
    void DrawObjectVoxelControls(IDemoScene& scene);
    void DrawVoxelWindow(IDemoScene& scene);
    bool CommitVoxelSettings(IDemoScene& scene, std::vector<VoxelObjectChange> changes);
    bool CommitVoxelBake(IDemoScene& scene, const std::optional<VoxelBakeSettings>& before, const std::optional<VoxelBakeSettings>& after,
        std::string* error);
    void StartVoxelBake(IDemoScene& scene, const ProjectPaths& paths, ProjectAssetRegistry& registry);
    void FinishVoxelBake(IDemoScene& scene);
    void RefreshVoxelStatus(IDemoScene& scene, const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    // History replay of the two voxel edits; false, with `error`, when the scene no longer matches the entry.
    bool ReplayVoxelSettings(IDemoScene& scene, const VoxelSettingsEdit& edit, bool redo, std::string& error);
    bool ReplayVoxelBake(IDemoScene& scene, const VoxelBakeEdit& edit, bool redo, std::string& error);
    static size_t VoxelEditPayloadBytes(const SceneHistorySession& session, std::size_t changes);
    // One running bake: the job, and what it must still match when it finishes to be published into this scene.
    struct VoxelBakeRun
    {
        std::unique_ptr<VoxelScene::SceneBakeJob> job;
        SceneHistorySession session;
        std::optional<VoxelBakeSettings> sceneBefore;
        Voxel::BakeSettings settings;
        VoxelScene::CaptureReport report;
        std::chrono::steady_clock::time_point started;
    };
    std::optional<VoxelBakeRun> m_voxelBake;
    bool m_voxelBakeRequested = false;
    bool m_showVoxelWindow = false;
    std::string m_voxelMessage;
    bool m_voxelMessageIsError = false;
    std::optional<VoxelScene::BakeOutcome> m_voxelLastOutcome;
    std::string m_voxelEditStatus;
    // The current bake's status, evaluated on a worker for the manifest it names.
    std::unique_ptr<VoxelScene::BakeStatusCheck> m_voxelStatusCheck;
    std::optional<VoxelScene::BakeStatus> m_voxelStatus;
    std::string m_voxelStatusManifest;
    bool m_voxelStatusRequested = true;
    // Participating meshes were still loading at the last refresh; the status is re-evaluated once they are.
    bool m_voxelStatusPending = false;

    // VX.4C.2 overlay: the latest finished build (its cube data is released once published), the build in progress, and what
    // the Renderer holds. A build is identified by the manifest reference and the isolated region it was made for.
    void RefreshVoxelOverlay(IDemoScene& scene, const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    void DrawVoxelOverlayControls();
    void ResetVoxelOverlay() noexcept;
    enum class VoxelOverlayUpload
    {
        None,
        Publish,
        Clear
    };
    bool m_voxelOverlayVisible = false;
    bool m_voxelOverlayBoxes = true;
    std::optional<Voxel::RegionKey> m_voxelOverlayIsolate;
    std::unique_ptr<VoxelVisualization::Job> m_voxelOverlayJob;
    std::string m_voxelOverlayJobFor;
    std::optional<VoxelVisualization::Result> m_voxelOverlayResult;
    std::string m_voxelOverlayBuiltFor;
    VoxelOverlayUpload m_voxelOverlayUpload = VoxelOverlayUpload::None;
    std::uint64_t m_voxelOverlayGeneration = 0;
    Matrix4 m_voxelOverlayWorldFromLattice = Matrix4::Identity();
    std::string m_voxelOverlayError;
    // M7: one complete replacement of an object's Probe Volume per entry (ProbeVolumeControls.cpp).
    struct ProbeVolumeEdit
    {
        SceneHistorySession session;
        std::string objectID;
        std::optional<ProbeVolumeComponent> before;
        std::optional<ProbeVolumeComponent> after;
    };
    // The selected volume's stored probes, reread only when its file changes.
    struct ProbeView
    {
        std::string objectID;
        std::string reference;
        std::filesystem::file_time_type fileTime{};
        bool fileExists = false;
        std::optional<ProbeBatchAssets::Data> stored;
        std::string loadError;
        std::uint64_t fingerprint = 0;
        double nextFingerprint = 0.0;
        ProbeVolumes::Status status = ProbeVolumes::Status::NotGenerated;
    };
    struct ProbeOverlay
    {
        int frame = -1000;
        bool valid = false;
        std::array<float, 3> size{};
        std::string objectID;
    };
    void DrawObjectProbeVolumeControls(IDemoScene& scene, const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    bool CommitObjectProbeVolume(IDemoScene& scene, const SceneObjectMetadata& selection, const std::optional<ProbeVolumeComponent>& before,
        const std::optional<ProbeVolumeComponent>& after);
    void RefreshProbeView(const FileBackedDemoScene& owner, const IDemoScene& scene, const SceneObjectDocument& object, const ProjectPaths& paths,
        const ProjectAssetRegistry& registry);
    // `parent`, when given, is the bake that needs these probes; otherwise the generation is a root task of its own.
    bool GenerateProbes(IDemoScene& scene, const FileBackedDemoScene& owner, const SceneObjectDocument& object,
        BackgroundTaskId parent = InvalidBackgroundTaskId);
    void PollProbeGeneration(IDemoScene& scene);
    void UpdateProbeGenerationTask();
    // Whether a Probe Volume edit would be accepted now (CommitObjectProbeVolume's own preconditions).
    bool CanEditProbeVolumes(const IDemoScene& scene) const;
    void DrawProbeGenerationProgress();
    void DrawProbeCleanup(const FileBackedDemoScene& owner, const ProjectPaths& paths, const ProjectAssetRegistry& registry, bool blocked);
    // What undo, redo and the open document can still bring back.
    std::set<std::string> CollectLiveProbeReferences(const FileBackedDemoScene& owner) const;
    struct ProbeGeneration
    {
        std::shared_ptr<Audio::ProbeGenerationJob> job;
        std::string objectID;
        std::uint64_t fingerprint = 0;
        std::vector<std::string> skippedObjects;
        SceneHistorySession session;
        BackgroundTaskHandle task;
    };
    std::optional<ProbeGeneration> m_probeGeneration;
    // A generation's task stays open until Engine has stored the probes and the reference is committed.
    BackgroundTaskHandle m_probePublishTask;
    std::optional<ProbeAssetCleanup::Report> m_probeCleanupReport;
    std::optional<ProbeCleanupRequest> m_probeCleanupRequest;
    void DrawProbeVolumeOverlay(const IDemoScene& scene, ImDrawList& draw, const Matrix4& viewProjection, ImVec2 origin, ImVec2 size) const;
    void DrawPathingOverlay(const FileBackedDemoScene& owner, ImDrawList& draw, const Matrix4& viewProjection, ImVec2 origin, ImVec2 size) const;
    std::string m_probeEditStatus;
    ProbeVolumeComponent m_probeDraft;
    ProbeVolumeComponent m_probeDraftBase;
    std::string m_probeDraftObject;
    ProbeView m_probeView;
    ProbeOverlay m_probeOverlay;
    std::optional<ProbeBatchRequest> m_probeBatchRequest;
    // M8.3 reflection baking (ReflectionBakeControls.cpp). One run at a time: Bake All, or one volume. Both go through the same
    // per-volume steps, so they share every check. A run processes its volumes one after another and continues past a volume that
    // fails; cancelling the run stops it after the current volume's job has stopped.
    struct ReflectionBakeVolume
    {
        std::string objectID;
        std::string name;
        // Bake All only: the volume's child task. A single-volume run reports on the run's own task.
        BackgroundTaskHandle task;
        std::vector<BackgroundTaskHandle> layers;
        std::shared_ptr<Audio::ReflectionBakeJob> job;
        ReflectionBakeJobs::Expectation expected;
        std::size_t reused = 0;
        // What the finished bake ran on, for the report.
        Audio::SteamAudioRayTracer rayTracer = Audio::SteamAudioRayTracer::Default;
        bool probesRequested = false;
        bool waitingForProbes = false;
        // The validated result, waiting for the scene to accept an edit.
        std::vector<std::byte> encoded;
        bool storing = false;
    };
    struct ReflectionBakeRun
    {
        bool all = false;
        bool force = false;
        SceneHistorySession session;
        BackgroundTaskHandle task;
        std::deque<std::string> pending;
        std::optional<ReflectionBakeVolume> current;
        std::size_t total = 0;
        std::size_t baked = 0;
        std::size_t upToDate = 0;
        std::size_t failed = 0;
        std::size_t cancelled = 0;
        std::string firstFailure;
        // Set once the run must end: cancelled, Play started or the scene went. The current job is stopped first.
        std::string stopReason;
    };
    enum class BakeVolumeOutcome
    {
        Baked,
        UpToDate,
        Failed,
        Cancelled
    };
    ReflectionBakeStatus::Host MakeReflectionBakeHost(const FileBackedDemoScene& owner, const IDemoScene& scene, const ProjectPaths& paths,
        const ProjectAssetRegistry& registry) const;
    void StartReflectionBake(IDemoScene& scene, std::vector<std::string> objectIDs, bool all, bool force, const std::string& title);
    void StartBakeAll(IDemoScene& scene, bool force);
    void PollReflectionBake(IDemoScene& scene, const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    void BeginBakeVolume(IDemoScene& scene, FileBackedDemoScene& owner, const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    void UpdateBakeVolumeTasks();
    void FinishBakedJob();
    void FinishBakeVolume(BakeVolumeOutcome outcome, const std::string& message);
    void FinishReflectionBakeRun();
    BackgroundTaskHandle& BakeVolumeTask();
    void DrawReflectionBakeControls(IDemoScene& scene, const SceneObjectMetadata& selection, const SceneObjectDocument& object, bool blocked);
    // The hierarchy's warning: the volume has baked data that no longer fully matches the scene. Null when it has none.
    const char* ReflectionBakeWarning(std::string_view objectID) const;
    std::optional<ReflectionBakeRun> m_reflectionBake;
    std::optional<ReflectionBakeStoreRequest> m_reflectionBakeStoreRequest;
    // The last attempt's error per volume, until it bakes successfully or the scene changes. Never touches published data.
    std::map<std::string, std::string, std::less<>> m_reflectionBakeFailures;
    ReflectionBakeStatus::Cache m_reflectionBakeStatus;
    std::chrono::steady_clock::time_point m_reflectionBakeStatusNext{};
    // The bounded fallback: the acoustic scene is recollected at least this often while editing.
    std::chrono::steady_clock::time_point m_reflectionBakeRecollectNext{};
    // From the Build menu, which has no mutable scene: Bake All (false) or Force Rebake All (true), started at the next poll.
    std::optional<bool> m_bakeAllRequest;
    std::string m_reflectionBakeMessage;
    // M9.6 pathing baking (PathingBakeControls.cpp). One volume at a time, never alongside another acoustic job. The SDK call cannot be
    // interrupted (Steam Audio 4.8.1 crashes on iplPathBakerCancelBake), so a cancel or Play waits for it to return, then discards it.
    struct PathingBakeRun
    {
        std::string objectID;
        std::string name;
        SceneHistorySession session;
        BackgroundTaskHandle task;
        std::shared_ptr<Audio::PathingBakeJob> job;
        // Exactly what was baked: the volume as it was, its probes, and the inputs that fingerprint them.
        ProbeVolumeComponent volume;
        ProbeBatchAssets::Data probes;
        PathingBakes::Inputs inputs;
        std::vector<std::byte> encoded;
        bool storing = false;
        std::string stopReason;
    };
    struct PathingStatusEntry
    {
        PathingBakes::Status status;
        std::size_t probes = 0;
        std::chrono::steady_clock::time_point next{};
    };
    PathingBakes::Inputs CurrentPathingInputs(const FileBackedDemoScene& owner, const IDemoScene& scene, const SceneObjectDocument& object,
        const ProjectPaths& paths, const ProjectAssetRegistry& registry, std::optional<ProbeBatchAssets::Data>& probes,
        std::vector<Audio::ProbeGeometryObject>* geometry) const;
    const PathingStatusEntry& RefreshPathingStatus(const IDemoScene& scene, const SceneObjectDocument& object, const ProjectPaths& paths,
        const ProjectAssetRegistry& registry);
    void StartPathingBake(IDemoScene& scene, const SceneObjectDocument& object, const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    void PollPathingBake(IDemoScene& scene, const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    void FinishPathingBake(bool success, bool failed, const std::string& message);
    void DrawPathingControls(IDemoScene& scene, const ProjectPaths& paths, const ProjectAssetRegistry& registry, const SceneObjectMetadata& selection, const SceneObjectDocument& object, bool blocked);
    void DrawAudioSourcePathingStatus(const IDemoScene& scene, const SceneObjectDocument& object);
    std::optional<PathingBakeRun> m_pathingBake;
    std::optional<ReflectionBakeStoreRequest> m_pathingStoreRequest;
    std::map<std::string, PathingStatusEntry, std::less<>> m_pathingStatus;
    std::string m_pathingMessage;
    PathingStatusProvider m_pathingStatusProvider;
    // Whether the viewport shows pathing (probes, coverage and the SDK's walked segments).
    bool m_pathingOverlay = false;
    std::string m_acousticEditStatus;
    std::array<char, 128> m_acousticMaterialSearch{};
    enum class AudioComponentKind
    {
        Source,
        Listener
    };
    void DrawObjectAudioControls(IDemoScene& scene);
    void DrawAudioSourceControls(IDemoScene& scene, const SceneObjectMetadata& selection,
        const SceneObjectDocument& object, bool blocked);
    void DrawAudioListenerControls(IDemoScene& scene, const SceneObjectMetadata& selection,
        const SceneObjectDocument& object, bool blocked);
    // Opens the "..." header menu of one audio component; returns true when it removed the component.
    bool DrawAudioComponentMenu(IDemoScene& scene, const SceneObjectMetadata& selection, AudioComponentKind kind,
        bool blocked);
    void DrawAudioEventPicker(IDemoScene& scene, const SceneObjectMetadata& selection, const SceneObjectDocument& object);
    bool CommitObjectAudio(IDemoScene& scene, const SceneObjectMetadata& selection, const AudioComponentState& before,
        const AudioComponentState& after);
    static size_t AudioEditPayloadBytes(const AudioComponentsEdit& edit);
    // What the Inspector shows for an Audio Source's event, judged against the loaded banks.
    struct AudioEventDescription
    {
        std::string text;
        std::string status;
        bool flat = false;
    };
    static AudioEventDescription DescribeAudioEvent(const AudioSourceComponent& source, const Audio::AudioSystem* audio);
    // Gameplay's rule: the first enabled Audio Listener in scene order; nullptr means the Main Camera hears.
    static const SceneObjectDocument* FindActiveAudioListener(std::span<const SceneObjectDocument> objects);
    void ValidateAudioManagementPopup(IDemoScene& scene);
    void DismissAudioManagementPopup();
    std::optional<SceneObjectMetadata> m_audioInspection;
    AudioComponentState m_audioInspectedValue;
    std::uint64_t m_audioInspectionEpoch = 0;
    std::string m_audioEditStatus;
    std::uint64_t m_audioControlsRevision = 0;
    std::array<char, 128> m_audioEventSearch{};
    AudioComponentKind m_audioManagementKind = AudioComponentKind::Source;
    Audio::AudioSystem* m_audioSystem = nullptr;
    // CM.9: read-only runtime diagnostics and the explicitly activated diagnostic drive.
    bool ViewportOwnsKeyboard() const;
    void DisableCharacterDrive(std::string reason);
    void UpdateCharacterDriveInput(const IDemoScene& scene);
    void DrawCharacterDriveToggle();
    void DrawCharacterControllerDiagnostics(const SceneObjectMetadata& selection,
        const Physics::CharacterControllerComponent& component);
    struct CharacterDriveCandidate
    {
        std::string objectID;
        float maximumSpeed = 0.0f;
    };
    struct CharacterDrive
    {
        std::string objectID;
        std::uint64_t sessionGeneration = 0;
        float maximumSpeed = 0.0f;
        bool jumpPending = false;
        Vector3 velocity;
    };
    // Copy of the latest published frame; never a reference into the simulation.
    std::optional<Physics::CharacterDiagnosticsFrame> m_characterDiagnostics;
    // Controller shown by the Inspector this frame; the Play-bar toggle reads it next frame.
    std::optional<CharacterDriveCandidate> m_characterDriveCandidate;
    std::optional<CharacterDrive> m_characterDrive;
    std::string m_characterDriveStatus;
    struct ComponentPopupTarget
    {
        SceneHistorySession session;
        SceneObjectMetadata selection;
        std::uint64_t inspectionEpoch = 0;
        ImGuiID popupID = 0;
    };
    static void CloseComponentPopup(ImGuiID popupID);
    // Top-left screen position for the Add Component popup: centered on the content area,
    // directly below the button when it fits in the work area, otherwise directly above it.
    static ImVec2 CalculateAddComponentPopupPosition(ImVec2 buttonMinimum, ImVec2 buttonMaximum,
        float contentCenterX, ImVec2 popupSize, ImVec2 workMinimum, ImVec2 workMaximum, float gap);
    void DrawAddComponentControls(IDemoScene& scene);
    std::optional<SceneObjectMetadata> ResolveComponentEditTarget(IDemoScene& scene);
    // The same check against a selection this frame already read, before any widget could edit it.
    std::optional<SceneObjectMetadata> ResolveComponentEditTarget(IDemoScene& scene,
        const SceneObjectMetadata& current);
    // The scene, session and pending-operation conditions under which component edits are allowed.
    bool ComponentEditAllowed(IDemoScene& scene);
    // Whether `current` is still the object the Inspector was opened on. Requires ComponentEditAllowed.
    bool MatchesComponentEditIdentity(IDemoScene& scene, const SceneObjectMetadata& current) const;
    void ValidateAddComponentPopup(IDemoScene& scene);
    void DismissAddComponentPopup();
    bool ComponentPopupMatches(IDemoScene& scene, const ComponentPopupTarget& target);
    void ValidateTriggerManagementPopup(IDemoScene& scene);
    void DismissTriggerManagementPopup();
    void ValidateControllerManagementPopup(IDemoScene& scene);
    void DismissControllerManagementPopup();
    bool ApplyObjectTransformEdit(IDemoScene& scene, RenderObjectID index, const Transform& transform);
    bool ApplyObjectTransformEdits(IDemoScene& scene, std::span<const RenderObjectTransformUpdate> updates);
    std::string m_transformEditStatus;
    std::optional<ComponentPopupTarget> m_addComponentPopup;
    std::optional<ComponentPopupTarget> m_triggerManagementPopup;
    std::optional<ComponentPopupTarget> m_controllerManagementPopup;
    std::optional<ComponentPopupTarget> m_cameraManagementPopup;
    std::optional<ComponentPopupTarget> m_lightManagementPopup;
    std::optional<ComponentPopupTarget> m_airMotorManagementPopup;
    std::optional<ComponentPopupTarget> m_mistEmitterManagementPopup;
    std::optional<ComponentPopupTarget> m_scriptManagementPopup;
    std::optional<ComponentPopupTarget> m_audioManagementPopup;
    std::array<char, 128> m_componentSearch{};
    bool m_focusComponentSearch = false;
    struct RenameObject
    {
        SceneHistorySession session;
        std::string objectID;
        std::string before;
        std::string after;
    };
    struct ReorderObjects
    {
        SceneHistorySession session;
        std::vector<std::string> before;
        std::vector<std::string> after;
    };
    struct MaterialBindingsEdit
    {
        SceneHistorySession session;
        std::string objectID;
        std::string meshReference;
        std::string fallbackReference;
        MeshMetadata layout;
        std::map<std::string, std::string> before;
        std::map<std::string, std::string> after;
    };
    void CommitObjectReorder(IDemoScene& scene, const HierarchyReorderRequest& request);
    std::string m_reorderStatus;

    struct RenameIntent
    {
        SceneHistorySession session;
        SceneObjectMetadata target;
    };
    std::optional<RenameIntent> m_renameIntent;
    std::string m_renameStatus;

    void QueueObjectRename(const IDemoScene& scene, const SceneObjectMetadata& target);
    void CancelObjectRename();
    void CommitObjectRename(IDemoScene& scene, std::string name);
    bool RenameIntentMatches(const IDemoScene& scene) const;
    bool RenameSessionMatches(const IDemoScene& scene, const SceneHistorySession& session) const;
    using SceneHistoryCommand = std::variant<std::unique_ptr<PhysicsComponentEdit>,
        std::unique_ptr<TriggerColliderEdit>, std::unique_ptr<CharacterControllerEdit>, TransformEdit, std::unique_ptr<GroupTransformEdit>,
        std::unique_ptr<ObjectDeletion>, std::unique_ptr<RenameObject>, std::unique_ptr<ReorderObjects>,
        std::unique_ptr<MaterialBindingsEdit>, std::unique_ptr<CameraEdit>, std::unique_ptr<LightEdit>,
        std::unique_ptr<ScriptEdit>, std::unique_ptr<AudioComponentsEdit>, std::unique_ptr<AcousticSettingsEdit>, std::unique_ptr<ProbeVolumeEdit>,
        std::unique_ptr<VoxelSettingsEdit>, std::unique_ptr<VoxelBakeEdit>, std::unique_ptr<AirMotorEdit>, std::unique_ptr<MistEmitterEdit>,
        std::unique_ptr<MistRenderingEdit>, std::unique_ptr<AirEnvironmentEdit>, std::unique_ptr<EnvironmentalHazeEdit>,
        std::unique_ptr<AirSimulationEdit>, std::unique_ptr<EnvironmentOverrideEdit>, std::unique_ptr<SkyEdit>>;
    struct SceneHistoryEntry
    {
        SceneHistoryCommand command;
        std::uint64_t sessionEpoch = 0;
        size_t payloadBytes = 0;
    };
    struct PendingHistoryReplay
    {
        HistoryReplayIdentity identity;
        size_t cursor = 0;
    };
    struct PendingMaterialBinding
    {
        std::uint64_t operationID = 0;
        std::uint64_t epoch = 0;
        size_t cursor = 0;
        std::optional<bool> redo;
        std::optional<SceneHistoryEntry> forward;
    };
    std::optional<PendingMaterialBinding> m_pendingMaterialBinding;
    void QueueMaterialBindingReplay(IDemoScene& scene, const MaterialBindingsEdit& edit, bool redo);
    static size_t MaterialBindingPayloadBytes(const MaterialBindingsEdit& edit);
    struct StagedDeletionHistory
    {
        SceneHistoryEntry entry;
        std::uint64_t operationID = 0;
    };
    std::optional<PendingHistoryReplay> m_pendingHistoryReplay;
    std::optional<HistoryReplayRequest> m_historyReplayRequest;
    std::optional<HistoryReplayRequest> m_duplicationRequest;
    std::optional<StagedDeletionHistory> m_stagedDeletionHistory;
    std::uint64_t m_historyOperationSerial = 0;
    void AppendSceneHistoryEntry(SceneHistoryEntry entry) noexcept;
    static size_t ObjectDeletionPayloadBytes(const ObjectDeletion& deletion);

    static constexpr size_t kSceneHistoryCapacity = 256;
    static constexpr size_t kSceneHistoryPayloadBytes = 4 * 1024 * 1024;
    std::vector<SceneHistoryEntry> m_sceneHistory;
    std::optional<SceneHistorySession> m_historySession;
    // History entries keep their original path token; only this authorized current location
    // follows a successful Save As. Arbitrary path changes still invalidate the session.
    std::filesystem::path m_historyCurrentScenePath;
    std::optional<HistoryTarget> m_gizmoHistoryTarget;
    std::optional<HistoryTarget> m_fieldHistoryTarget;
    std::string m_sceneHistoryStatus;

    size_t m_historyCursor = 0;
    size_t m_historyPayloadBytes = 0;
    std::uint64_t m_historyEpoch = 0;
    std::uint64_t m_historyRuntimeIdentity = 0;
    std::uint64_t m_historyStructureRevision = 0;
    std::uint64_t m_gizmoHistoryEpoch = 0;
    std::uint64_t m_fieldHistoryEpoch = 0;
    // Identity-only borrow outside command entries; never dereferenced after scene replacement.
    const Scene* m_historyObservedScene = nullptr;

    bool UpdateSceneHistorySession(const IDemoScene& scene, const ProjectPaths& paths, const std::string& name);
    std::optional<HistoryTarget> CaptureHistoryTarget(const IDemoScene& scene, size_t index);
    std::optional<size_t> ResolveHistoryTarget(const IDemoScene& scene, const HistoryTarget& target);
    void RecordTransformEdit(HistoryTarget target, std::uint64_t epoch, const Transform& before, const Transform& after);
    void TruncateHistoryRedo();
    void ReplaySceneHistory(IDemoScene& scene, bool redo);
    bool HistoryReplayBlocked() const;
    void ReportSceneHistoryFailure(const std::string& error);
    void ResetTransformGestures();
    void ClearSceneHistory();

    bool m_gizmoWasUsing = false;
    // ImGuizmo retains hover geometry even when no enabled gizmo was drawn this frame.
    bool m_viewportGizmoInteractive = false;

    size_t m_gizmoUndoObject = InvalidObjectSelection;

    Transform m_gizmoUndoStart;

    bool m_transformFieldEditing = false;

    size_t m_transformFieldUndoObject = InvalidObjectSelection;

    Transform m_transformFieldUndoStart;
    const Scene* m_transformFieldScene = nullptr;
    std::uint64_t m_transformFieldRevision = 0;
    void DrawSelectedObjectTransformControls(IDemoScene& demoScene);
    void FinishTransformFieldEdit(IDemoScene& demoScene);

    void DrawTransformGizmo(
        IDemoScene& demoScene,
        float viewportWidth,
        float viewportHeight);
    void HandleViewportSelection(
        IDemoScene& demoScene,
        float viewportWidth,
        float viewportHeight,
        bool viewportWasFocused = true);
    void HandleTransformUndo(IDemoScene& demoScene);
    void HandleTransformRedo(IDemoScene& demoScene);
    void UpdateCameraLook(
        IDemoScene& demoScene,
        float viewportWidth,
        float viewportHeight);
    void UpdateCameraMovement(
        IDemoScene& demoScene,
        float deltaTime);
    void DrawObjectControls(IDemoScene& demoScene);
    void DrawStressTestControls(StressTestScene& demoScene);
    void DrawSphereGridControls(SphereGridScene& demoScene);
    void DrawDirectionalLight(IDemoScene& demoScene, const DebugUI& debugUI);
    void DrawCamera(IDemoScene& demoScene);

    size_t m_selectedObject = InvalidObjectSelection;

    uint64_t m_selectionStructureRevision = 0;

    std::string m_selectionSceneName;
    const Scene* m_selectionScene = nullptr;
    std::uint64_t m_selectionRuntimeIdentity = 0;
};
