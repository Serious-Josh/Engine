#pragma once

#include "assets/AssetRecord.h"
#include "assets/MaterialSavedRevision.h"
#include "Scene/Transform.h"
#include "Scene/FileSceneRuntime.h"
#include "assets/MaterialAssetData.h"
#include "Renderer/Preview/MaterialPreviewRequest.h"

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <chrono>

struct EditorWorkspaceRect;
class ProjectPaths;
class ProjectAssetRegistry;
class Renderer;
class EditorIcons;

struct SceneMaterialSlotInspectorView
{
    std::string id;
    std::string name;
    std::string materialName;
    std::string materialReference;

    bool explicitBinding = false;
    bool available = false;
};

struct SceneObjectInspectorView
{
    std::string name;

    Transform transform;

    std::string mesh;
    std::string material;

    std::optional<SceneObjectMetadata> metadata;
    std::string meshReference;
    std::vector<SceneMaterialSlotInspectorView> materialSlots;

    // Derived presentation snapshot only; absent for demos without authored references.
    std::optional<SceneMeshResidency> meshResidency;
    bool materialSlotMetadataAvailable = false;
};

class InspectorPanel
{
public:
    void SetIcons(const EditorIcons& icons) { m_icons = &icons; }
    const std::optional<MaterialPreviewRequest>& GetPreviewRequest();
    std::optional<std::string> TakeMaterialLocateRequest();
    void SetMaterialLocateStatus(std::string status) { m_materialLocateStatus=std::move(status); }
    void Draw(
        const EditorWorkspaceRect& rectangle,
        const ProjectPaths& paths,
        const ProjectAssetRegistry& registry,
        const std::optional<MaterialPreviewImage>& preview,
        bool protectedAction = false);

    void DrawSceneObject(const EditorWorkspaceRect& rectangle, const SceneObjectInspectorView& object,
        const std::function<void()>& drawTransformControls,
        const ProjectPaths& paths, const ProjectAssetRegistry& registry,
        bool assignmentPending, const std::string& assignmentStatus,
        const std::function<void()>& drawComponentControls = {});
    struct SlotAssignmentRequest
    {
        SceneObjectMetadata selection;
        std::string slotID;
        std::optional<AssetID> materialID;
    };
    std::optional<SlotAssignmentRequest> TakeSlotAssignmentRequest();
    void OnSlotAssignmentCompleted(const SceneObjectMetadata& selection, const std::string& slotID,
        bool succeeded, const std::string& message);
    void DismissSceneMaterialPicker();
    void DismissTexturePicker();
    const std::string& GetSlotPickerStatus() const noexcept
    {
        return m_materialPickerStatus;
    }
    void ConfigureThumbnails(Renderer& renderer)
    {
        m_thumbnailRenderer = &renderer;
    }
    void ReleaseSceneObjectPreview();

    enum class LeaveDecision
    {
        Pending,
        Proceed,
        Cancel
    };

    bool HasUnsavedChanges() const;
    LeaveDecision DrawUnsavedConfirmation(
        const ProjectPaths& paths,
        const ProjectAssetRegistry& registry);
    bool TakeRefreshRequest();
    std::optional<std::filesystem::path> TakeSavedMaterialSource();
    std::vector<MaterialSavedRevision> TakeSavedMaterialRevisions();
    std::vector<MaterialWorkingRevision> TakeWorkingMaterialRevisions(bool active);
    void ResetWorkingMaterialNotifications();
    void LoadSelection(
        const std::filesystem::path& selection,
        const ProjectPaths& paths,
        const ProjectAssetRegistry& registry);

private:
#if defined(ENGINE_HAS_VERIFICATION)
    friend class MaterialLocateVerification;
#endif
    void HandleMaterialLocateItem(std::string_view reference);
    std::optional<std::string> m_materialLocateRequest;
    std::string m_materialLocateStatus;
    Renderer* m_thumbnailRenderer = nullptr;
    std::filesystem::path m_meshThumbnailProject;
    std::string m_meshThumbnailReference;
    std::uint64_t m_meshThumbnailTicket = 0;
    std::chrono::steady_clock::time_point m_meshThumbnailValidation{};
    std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> GetSceneMeshThumbnail(
        const ProjectPaths& paths, const ProjectAssetRegistry& registry, const std::string& reference);
    void DrawSceneAssetTiles(const SceneObjectInspectorView& object,
        const ProjectPaths& paths, const ProjectAssetRegistry& registry, bool assignmentPending);
    std::optional<std::string> DrawCompactMaterialSlots(const SceneObjectInspectorView& object, bool interactive);
    void OpenSceneMaterialPicker(const SceneObjectInspectorView& object, const ProjectPaths& paths,
        const std::string& slotID);

    void DrawSceneMaterialPicker(
        const SceneObjectInspectorView& object,
        const ProjectPaths& paths,
        const ProjectAssetRegistry& registry,
        bool assignmentPending);
    bool MaterialPickerMatches(const SceneObjectInspectorView& object, const ProjectPaths& paths) const;
    void RequestSlotMaterial(const SceneObjectInspectorView& object, const ProjectPaths& paths,
        const ProjectAssetRegistry& registry, const std::optional<AssetID>& materialID);
    void DrawSlotMaterialGrid(const SceneObjectInspectorView& object, const SceneMaterialSlotInspectorView& slot,
        const ProjectPaths& paths, const ProjectAssetRegistry& registry);

    std::array<char, 128> m_materialSearch{};

    std::vector<AssetRecord> m_materialChoices;

    std::optional<SceneObjectMetadata> m_materialPickerObject;

    std::filesystem::path m_materialPickerProject;
    std::string m_materialPickerSlot;
    std::string m_materialPickerStatus;
    std::optional<SlotAssignmentRequest> m_slotAssignmentRequest;
    bool m_slotAssignmentPending = false;
    bool m_closeSlotPicker = false;

    std::optional<std::filesystem::path> m_savedThumbnailSource;

    std::vector<MaterialSavedRevision> m_savedMaterialRevisions;

    std::uint64_t m_saveSequence = 0;
    void EndWorkingMaterial();
    std::vector<MaterialWorkingRevision> m_workingMaterialRevisions;

    std::optional<MaterialAssetData> m_lastWorkingDraft;
    std::optional<MaterialAssetData> m_lastWorkingBaseline;

    std::uint64_t m_workingSequence = 0;
    std::uint64_t m_lastWorkingSaveSequence = 0;

    bool m_workingWasActive = false;

    std::optional<MaterialAssetData> m_previewDraft;

    std::optional<MaterialPreviewRequest> m_previewRequest;

    std::string m_previewIdentity;

    std::filesystem::path m_previewProject;

    std::uint64_t m_previewRevision = 0;

    float m_previewYaw = 0.0f;
    float m_previewPitch = 0.0f;

    bool m_previewOrientationDirty = false;

    bool Save(
        const ProjectPaths& paths,
        const ProjectAssetRegistry& registry,
        bool overwriteConflict = false);
    bool WriteMaterial(
        const ProjectPaths& paths,
        const ProjectAssetRegistry& registry,
        bool overwriteConflict);
    void DrawMaterial(const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    enum class TextureSlot { BaseColor, Normal, Roughness, Metallic, ARM, AO, Height };
    struct TexturePickerIdentity
    {
        AssetRecord material;
        std::filesystem::path project;
        TextureSlot slot;
        unsigned int generation = 0;
    };
    struct TexturePickerRequest
    {
        std::uint64_t ticket = 0;
        bool visible = false;
    };

    void DrawTextureReference(
        const char* label,
        TextureSlot slot,
        std::optional<std::string>& reference,
        const ProjectPaths& paths,
        const ProjectAssetRegistry& registry,
        bool selectionValid);

    void DrawTexturePickerContents(
        TextureSlot slot,
        const ProjectPaths& paths,
        const ProjectAssetRegistry& registry);

    bool TexturePickerMatches(TextureSlot slot, const ProjectPaths& paths,
        const ProjectAssetRegistry& registry) const;
    std::optional<std::string>* GetTextureSlot(TextureSlot slot);
    bool ApplyTextureSelection(const AssetRecord* texture, const ProjectPaths& paths,
        const ProjectAssetRegistry& registry);
    void RefreshTextureChoices(const ProjectAssetRegistry& registry);
    void PruneTexturePickerRequests(bool all = false);
    void DrawTextureGrid(TextureSlot slot, const ProjectPaths& paths,
        const ProjectAssetRegistry& registry);
    bool DrawTextureTile(const AssetRecord& texture, float extent, bool selected,
        const ProjectPaths& paths, const ProjectAssetRegistry& registry);

    void Revert();

    std::optional<MaterialAssetData> m_savedMaterial;

    std::string m_savedBytes;
    std::string m_conflictBytes;

    enum class PersistenceState
    {
        Saved,
        Waiting,
        Saving,
        Failed
    };

    PersistenceState m_persistenceState = PersistenceState::Saved;

    double m_lastPropertyChangeTime = 0.0;

    bool m_saveScheduled = false;
    bool m_leaveFlushAttempted = false;
    bool m_showConflictConfirmation = false;

    bool m_refreshRequested = false;
    bool m_externalConflict = false;

    // Popup identity changes on every material load, including Refresh.
    unsigned int m_texturePickerGeneration = 0;
    unsigned int m_previewTextureRefresh = 0;
    std::array<char, 128> m_textureSearch{};

    std::vector<AssetRecord> m_texturePickerRegistry;
    std::vector<AssetRecord> m_textureChoices;
    std::optional<TexturePickerIdentity> m_texturePicker;
    std::map<std::string, TexturePickerRequest> m_texturePickerRequests;
    std::string m_texturePickerError;
    std::filesystem::path m_texturePickerFolder;
    const EditorIcons* m_icons = nullptr;

    bool m_refreshTextureChoices = true;
    bool m_texturePickerDrawn = false;

    std::filesystem::path m_projectRoot;

    std::optional<AssetRecord> m_record;

    std::optional<MaterialAssetData> m_material;

    std::string m_status = "No asset or scene object selected.";
};
