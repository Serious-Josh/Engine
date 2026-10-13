#pragma once

#include "Editor/TextureThumbnailCache.h"
#include <filesystem>
#include <map>
#include <optional>
#include <array>
#include <string>
#include <vector>

struct EditorWorkspaceRect;
class ProjectPaths;
class ProjectAssetRegistry;
class Renderer;

class AssetBrowser
{
public:
    AssetBrowser();

    const std::filesystem::path& GetSelection() const noexcept
    {
        return m_selection;
    }

    void RestoreSelection(const std::filesystem::path& selection)
    {
        m_selection = selection;
    }

    void ConfigureThumbnails(
        ID3D12Device* device,
        ResourceUploader& uploader,
        DescriptorHeap& heap)
    {
        m_thumbnails.Configure(device, uploader, heap);
    }
    void SetThumbnailCompletedFence(std::uint64_t fence)
    {
        m_thumbnails.SetCompletedFence(fence);
    }

    void ConfirmThumbnailSubmission(std::uint64_t fence)
    {
        m_thumbnails.ConfirmSubmission(fence);
    }

    bool NeedsThumbnailReset() const
    {
        return m_thumbnailResetRequested;
    }
    void ClearThumbnailsAfterGPUDrain();
    std::vector<Texture*> GetThumbnailTextures()
    {
        return m_thumbnails.GetTextures();
    }
    // Borrowed renderer service; Renderer owns all material thumbnail GPU resources.
    void ConfigureMaterialThumbnails(Renderer& renderer)
    {
        m_materialRenderer = &renderer;
    }
    void PauseMaterialThumbnails();
    void InvalidateMaterialSource(const std::filesystem::path& path);
    void SetThumbnailProject(const ProjectPaths& paths, const ProjectAssetRegistry& registry);
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

private:
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

    TextureThumbnailCache m_thumbnails;
    std::optional<ProjectPaths> m_thumbnailPaths;
    const ProjectAssetRegistry* m_thumbnailRegistry = nullptr;
    std::filesystem::path m_thumbnailProjectRoot;
    std::map<std::filesystem::path, AssetRecord> m_textureRecords;
    std::map<std::filesystem::path, AssetRecord> m_materialRecords;
    Renderer* m_materialRenderer = nullptr;
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

    std::filesystem::path m_root;
    std::filesystem::path m_directory;
    std::filesystem::path m_selection;

    std::map<std::filesystem::path, Listing> m_listings;

    std::string m_error;

    std::optional<MaterialCreationRequest> m_materialRequest;

    bool m_materialCreationPending = false;
    bool m_openMaterialModal = false;

    std::filesystem::path m_materialDirectory;

    std::array<char, 256> m_materialName{};

    std::string m_materialError;
};
