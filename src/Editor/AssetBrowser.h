#pragma once

#include "assets/AcousticMaterial.h"

#include "Editor/TextureThumbnailCache.h"
#include "Editor/MeshDragPayload.h"
#include "Editor/MaterialDragPayload.h"
#include "Editor/ThumbnailAdmission.h"
#include "Debug/EditorPhaseTiming.h"
#include <filesystem>
#include <map>
#include <set>
#include <optional>
#include <array>
#include <string>
#include <vector>
#include <chrono>
#include <utility>
#include "imgui.h"

struct EditorWorkspaceRect;
class ProjectPaths;
class ProjectAssetRegistry;
class Renderer;
class ProjectContext;
class FileBackedDemoScene;
class EditorIcons;
struct MaterialPreviewImage;

class AssetBrowser
{
public:
    void SetIcons(const EditorIcons& icons)
    {
        m_icons = &icons;
    }
    // Diagnostics only: the Editor's phase-timing frame, or null when timing is disabled.
    void SetPhaseTiming(EditorPhaseTiming::Frame* frame) noexcept
    {
        m_phaseTiming = frame;
    }
    bool TakeScriptRefreshRequest()
    {
        return std::exchange(m_scriptRefreshRequested, false);
    }
    AssetBrowser();
    bool RevealMaterial(const AssetID&,const ProjectPaths&,const ProjectAssetRegistry&,std::string& error);
    std::optional<AssetID> GetNavigationHighlight() const noexcept { return m_navigationHighlight; }

    const std::filesystem::path& GetSelection() const noexcept
    {
        return m_selection;
    }

    bool TakeSelectionIntent()
    {
        const bool requested = m_selectionIntent;
        m_selectionIntent = false;
        return requested;
    }

    void RestoreSelection(const std::filesystem::path& selection)
    {
        m_selection = selection;
    }

    void SetThumbnailCompletedFence(std::uint64_t fence)
    {
        m_thumbnails->SetCompletedFence(fence);
    }

    bool NeedsThumbnailReset() const
    {
        return m_thumbnailResetRequested;
    }

    void ResetThumbnailRequests();
    // Borrowed renderer service; Renderer owns all material thumbnail GPU resources.
    // The first call fixes the browsing root to the active project's root.
    void ConfigureMaterialThumbnails(Renderer& renderer, const ProjectPaths& projectPaths);

    void PauseMaterialThumbnails();
    void InvalidateMaterialSource(const std::filesystem::path& path);
    void SetThumbnailProject(const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    void InvalidateMeshDragIdentity() noexcept;
    std::optional<MaterialDragPayload> MakeMaterialDragPayload(const AssetID& id) const;
    std::optional<AssetRecord> ValidateMaterialDrag(const MaterialDragPayload& payload,
        const ProjectPaths& paths, const ProjectAssetRegistry& registry, std::string& error) const;
    std::optional<AssetRecord> ValidateMeshDrag(const MeshDragPayload& payload,
        const ProjectPaths& paths, const ProjectAssetRegistry& registry, std::string& error) const;
    void Draw(const EditorWorkspaceRect& rectangle, const ProjectContext* project = nullptr,
        const FileBackedDemoScene* activeScene = nullptr);
    std::optional<std::filesystem::path> TakeSceneOpenRequest();
    // File listings only: scene saves/activation must not invalidate GPU thumbnails or selection.
    void InvalidateFileListings();
    void ImportFileDrop(
        const std::vector<std::filesystem::path>& files,
        float screenX,
        float screenY,
        const ProjectPaths& paths,
        ProjectAssetRegistry& registry);

    struct RenameRequest
    {
        std::filesystem::path source;

        std::string stem;
    };

    void DrawRenameModal(bool protectedEdits);
    std::optional<RenameRequest> TakeRenameRequest();
    void OnRenameCompleted(const std::filesystem::path& source, const std::filesystem::path& destination,
        const std::string& error);

    struct MoveRequest
    {
        AssetRecord asset;
        std::filesystem::path source;
        std::filesystem::path directory;
    };
    std::optional<MoveRequest> TakeMoveRequest();
    void OnMoveCompleted(const MoveRequest& request, const std::filesystem::path& destination,
        const std::string& error);

    void DrawDeletionConfirmation(bool protectedEdits);
    std::optional<std::filesystem::path> TakeDeletionRequest();
    void OnDeletionCompleted(const std::filesystem::path& path, const std::string& error);

    // The Asset Browser's asset drag payload: a canonical reference of any asset type, which each drop target validates.
    struct MovePayload
    {
        std::uint64_t projectIdentity = 0;
        std::array<char, 43> reference{};
    };

    // M4: a new Acoustic Material, from a preset or by duplicating another.
    struct AcousticMaterialCreationRequest
    {
        std::filesystem::path relativePath;
        AcousticMaterialData material;
    };
    std::optional<AcousticMaterialCreationRequest> TakeAcousticMaterialCreationRequest();

    struct MaterialCreationRequest
    {
        std::filesystem::path relativePath;
    };

    std::optional<MaterialCreationRequest> TakeMaterialCreationRequest();
    void OnMaterialCreationCompleted(
        const std::filesystem::path& relativePath,
        const std::string& error);

    struct MeshImportUnitsRequest
    {
        std::filesystem::path project;
        AssetRecord expected;
        MeshImportUnits units;
    };

    std::optional<MeshImportUnitsRequest> TakeMeshImportUnitsRequest();
    void OnMeshImportUnitsCompleted(const std::string& error);

private:
    const EditorIcons* m_icons = nullptr;
    EditorPhaseTiming::Frame* m_phaseTiming = nullptr;
#if defined(ENGINE_HAS_VERIFICATION)
    friend class MaterialLocateVerification;
    friend class ScriptWorkflowVerification;
    friend class ThumbnailSchedulingVerification;
    friend class AssetBrowserSceneVerification;
#endif
    void DrawAssetMoveSource(const std::filesystem::path& path, const std::string& name);
    void DrawAssetMoveTarget(const std::filesystem::path& directory);
    std::optional<MoveRequest> m_moveRequest;
    std::filesystem::path m_moveSource;

    void DrawMeshDragSource(const std::filesystem::path& path, const std::string& name);
    std::filesystem::path m_meshDragProject;
    std::uint64_t m_meshDragIdentity = 0;

    std::filesystem::path m_renameTarget;

    std::array<char, 1025> m_renameStem{};

    std::optional<RenameRequest> m_renameRequest;

    std::string m_renameError;

    bool m_openRenameModal = false;
    bool m_focusRename = false;
    bool m_renamePending = false;
    bool m_closeRenameModal = false;

    std::filesystem::path m_deleteTarget;

    std::optional<std::filesystem::path> m_deleteRequest;

    bool m_openDeleteModal = false;

    TextureThumbnailCache* m_thumbnails = nullptr;

    std::optional<ProjectPaths> m_thumbnailPaths;
    const ProjectAssetRegistry* m_thumbnailRegistry = nullptr;
    bool m_scriptRefreshRequested = false;
    std::filesystem::path m_thumbnailProjectRoot;

    std::map<std::filesystem::path, AssetRecord> m_textureRecords;
    std::map<std::filesystem::path, AssetRecord> m_materialRecords;
    std::map<std::filesystem::path, AssetRecord> m_meshRecords;

    Renderer* m_materialRenderer = nullptr;

    struct MeshThumbnailRequest
    {
        std::uint64_t ticket = 0;
        std::chrono::steady_clock::time_point nextValidation{};
        std::string error;
        bool visible = false;

        // Set once the tile has displayed its completed image. A resident tile keeps its ticket and
        // image after leaving admission, so generation rotation does not discard finished thumbnails.
        bool resident = false;
        std::chrono::steady_clock::time_point residentSince{};
    };
    std::map<std::string, MeshThumbnailRequest> m_meshThumbnailRequests;

    // Registration and resolution check for a mesh thumbnail source (file-system work).
    bool IsMeshSourceAvailable(const AssetID& id) const;

    // Image of a resident tile, revalidated on the usual cadence; drops residency (returning nothing)
    // when the image was evicted or its source changed, so the tile rejoins admission.
    std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> GetResidentMeshThumbnail(const AssetRecord& record);

    // Residency budget enforcement for visible sets larger than kMeshResidentImageCapacity.
    void YieldResidentMeshThumbnail(std::chrono::steady_clock::time_point now);

    // Material tiles that displayed their thumbnail. A resident tile keeps displaying (and pinning) its
    // image without admission; admission then only schedules tiles that still need one.
    struct ResidentMaterialThumbnail
    {
        std::chrono::steady_clock::time_point since{};
        bool visible = false;

        // Renderer cache handle of the displayed image (MaterialPreviewImage::cacheSlot / cacheSerial).
        size_t cacheSlot = SIZE_MAX;
        std::uint64_t cacheSerial = 0;
    };
    std::map<std::string, ResidentMaterialThumbnail> m_materialResidents;
    std::chrono::steady_clock::time_point m_lastMaterialResidentYield{};

    // Image of a resident material tile. The request keeps the image pinned for this frame and runs the
    // renderer's periodic validation; residency ends (returning nothing) once the image is stale or gone.
    std::optional<MaterialPreviewImage> GetResidentMaterialThumbnail(const std::string& key, const AssetID& id);

    // Forgets residents that left the view, admits only into free residency, and lets the longest resident
    // yield when a visible set larger than kThumbnailResidentCapacity has tiles waiting.
    void ScheduleMaterialThumbnails(std::chrono::steady_clock::time_point now);

    // Diagnostics builds only: counts material tiles that displayed a thumbnail and later drew a status label.
    void RecordMaterialDisplayForDiagnostics(const std::string& key, bool displayed);
#if defined(ENGINE_HAS_VERIFICATION)
    std::set<std::string> m_materialDisplayedForDiagnostics;
#endif
    std::chrono::steady_clock::time_point m_lastResidentYield{};
    void PruneMeshThumbnailRequests(bool all = false);

    // `terminal` reports an unreadable or unsupported source rather than a temporary failure.
    std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> GetVisibleMeshThumbnail(const AssetRecord& record, std::string& error,
        bool& terminal);

    // B.2 fair admission of visible thumbnail tiles, keyed by asset reference. Runs after each
    // Draw: releases tiles that left the view or yielded, then admits the oldest waiting tiles.
    ThumbnailAdmission m_meshAdmission;
    ThumbnailAdmission m_materialAdmission;
    void ScheduleThumbnails();

    // Last failure per asset, shown as a tile tooltip instead of being logged every frame.
    struct ThumbnailFailure
    {
        std::string message;
        bool terminal = false;
    };
    std::map<std::string, ThumbnailFailure> m_thumbnailFailures;
    std::string FindThumbnailFailure(const std::string& key) const;
    void DrawMeshImportUnits();
    std::optional<AssetRecord> m_unitsAsset;
    MeshImportUnits m_unitsDraft;
    float m_unitsManualScale = 1.0f;
    std::string m_unitsSourceInfo;
    bool m_openUnits = false;
    std::optional<MeshImportUnitsRequest> m_unitsRequest;

    bool m_materialRequestsReset = false;
    bool m_materialImagesInvalidated = false;
    bool m_thumbnailResetRequested = false;

    struct DropTarget
    {
        float left, top, right, bottom;
        unsigned int windowID;
        std::filesystem::path directory;
    };

    void CaptureDropTarget(const std::filesystem::path& directory, bool contentArea = false);
    std::vector<DropTarget> m_dropTargets;

    int m_dropFrame = -1;

    std::string m_importFeedback;

    struct Entry
    {
        std::filesystem::path path;

        std::string name;

        bool directory = false;
        bool navigable = false;

        // Captured at enumeration. Listings are rebuilt by Refresh, navigation, scene saves and every
        // browser-initiated file operation, so this only goes stale after an external change; actions
        // re-check the filesystem before they act and never treat this as authorization.
        bool ordinaryFile = false;

        // Canonical path of a .scene entry that passed the project containment checks, or empty when the
        // entry is not a browsable scene. Unset until the entry is first classified against sceneRoot.
        std::optional<std::filesystem::path> sceneCanonicalPath;
    };

    struct Listing
    {
        std::vector<Entry> entries;

        std::string error;

        // The listed directory passed CanBrowse when it was enumerated.
        bool browsable = false;

        // Canonical project root that the entries' scene classifications were resolved against.
        // Recorded per listing so a project switch or Refresh re-resolves it once, not once per frame.
        std::filesystem::path sceneProjectRoot;
        std::filesystem::path sceneRoot;
    };

    // Active-scene identity resolved once per change of its inputs instead of once per tile per frame.
    // Every input is part of the key, so switching scenes, Save As, New and project changes take effect
    // on the next draw without invalidating any listing.
    struct ActiveSceneIdentity
    {
        std::filesystem::path projectRoot;
        std::filesystem::path scenePath;
        std::filesystem::path sceneOwnerRoot;
        bool eligible = false;

        // Canonical active scene path; empty when no listed entry can be the active scene.
        std::filesystem::path canonicalScenePath;
    };

    std::optional<ActiveSceneIdentity> m_activeSceneIdentity;

    bool CanBrowse(
        const std::filesystem::path& path,
        std::string& error) const;
    Listing& GetListing(const std::filesystem::path& path);
    void ClassifySceneEntries(Listing& listing, const ProjectContext& project) const;
    const std::filesystem::path& GetActiveScenePath(const ProjectContext& project,
        const FileBackedDemoScene* activeScene);
    static std::filesystem::path ResolveActiveScenePath(const ProjectContext& project,
        const std::filesystem::path& ownerRoot, const std::filesystem::path& scenePath);
    bool CanRequestFileOperation(const std::filesystem::path& path, std::string& error) const;
    void DrawTree(
        const std::filesystem::path& path,
        const std::string& label);
    void Navigate(const std::filesystem::path& path);
    void DrawContents(const ProjectContext* project, const FileBackedDemoScene* activeScene);
    bool IsSceneEntry(const std::filesystem::path& path, const ProjectContext& project) const;
    bool IsActiveSceneEntry(const std::filesystem::path& path, const ProjectContext& project,
        const FileBackedDemoScene* activeScene) const;
    bool RequestSceneOpen(const std::filesystem::path& path, const ProjectContext& project);
    std::optional<std::filesystem::path> m_sceneOpenRequest;
    ImGuiTextFilter m_filter;
    bool CanCreateMaterial(std::string& error) const;
    void DrawMaterialCreationModal();
    void DrawFolderCreationModal();

    std::filesystem::path m_root;
    std::filesystem::path m_directory;
    std::filesystem::path m_selection;

    bool m_selectionIntent = false;
    std::optional<AssetID> m_navigationHighlight;
    bool m_revealPending = false;

    std::map<std::filesystem::path, Listing> m_listings;

    std::string m_error;

    std::optional<MaterialCreationRequest> m_materialRequest;
    std::optional<AcousticMaterialCreationRequest> m_acousticRequest;
    bool m_openAcousticModal = false;
    std::filesystem::path m_acousticDirectory;
    std::array<char, 256> m_acousticName{};
    std::string m_acousticError;
    int m_acousticPreset = 0;
    void DrawAcousticMaterialCreationModal();
    void RequestAcousticMaterialDuplicate(const std::filesystem::path& source);

    bool m_materialCreationPending = false;
    bool m_openMaterialModal = false;

    std::filesystem::path m_materialDirectory;

    std::array<char, 256> m_materialName{};

    std::string m_materialError;

    bool m_openFolderModal = false;
    std::filesystem::path m_folderDirectory;
    std::array<char, 256> m_folderName{};
    std::string m_folderError;
};
