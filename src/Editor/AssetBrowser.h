#pragma once

#include "Editor/TextureThumbnailCache.h"
#include "Editor/MeshDragPayload.h"
#include "Editor/MaterialDragPayload.h"
#include "Editor/ThumbnailAdmission.h"
#include <filesystem>
#include <map>
#include <optional>
#include <array>
#include <string>
#include <vector>
#include <chrono>

struct EditorWorkspaceRect;
class ProjectPaths;
class ProjectAssetRegistry;
class Renderer;

class AssetBrowser
{
public:
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
    void ConfigureMaterialThumbnails(Renderer& renderer);

    void PauseMaterialThumbnails();
    void InvalidateMaterialSource(const std::filesystem::path& path);
    void SetThumbnailProject(const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    void InvalidateMeshDragIdentity() noexcept;
    std::optional<MaterialDragPayload> MakeMaterialDragPayload(const AssetID& id) const;
    std::optional<AssetRecord> ValidateMaterialDrag(const MaterialDragPayload& payload,
        const ProjectPaths& paths, const ProjectAssetRegistry& registry, std::string& error) const;
    std::optional<AssetRecord> ValidateMeshDrag(const MeshDragPayload& payload,
        const ProjectPaths& paths, const ProjectAssetRegistry& registry, std::string& error) const;
    void Draw(const EditorWorkspaceRect& rectangle);
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
#if defined(ENGINE_HAS_VERIFICATION)
    friend class MaterialLocateVerification;
    friend class ThumbnailSchedulingVerification;
#endif
    struct MovePayload
    {
        std::uint64_t projectIdentity = 0;
        std::array<char, 43> reference{};
    };
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
    };
    std::map<std::string, MeshThumbnailRequest> m_meshThumbnailRequests;
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
    };

    struct Listing
    {
        std::vector<Entry> entries;

        std::string error;
    };

    bool CanBrowse(
        const std::filesystem::path& path,
        std::string& error) const;
    const Listing& GetListing(const std::filesystem::path& path);
    void DrawTree(
        const std::filesystem::path& path,
        const std::string& label);
    void Navigate(const std::filesystem::path& path);
    void DrawContents();
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
