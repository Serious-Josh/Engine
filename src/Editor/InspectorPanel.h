#pragma once

#include "assets/AssetRecord.h"
#include "assets/MaterialSavedRevision.h"
#include "Scene/Transform.h"
#include "Scene/FileSceneRuntime.h"
#include "assets/MaterialAssetData.h"
#include "Renderer/Preview/MaterialPreviewRequest.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct EditorWorkspaceRect;
class ProjectPaths;
class ProjectAssetRegistry;

struct SceneObjectInspectorView
{
    std::string name;

    Transform transform;

    std::string mesh;
    std::string material;

    std::optional<SceneObjectMetadata> metadata;
};

class InspectorPanel
{
public:
    const std::optional<MaterialPreviewRequest>& GetPreviewRequest();
    void Draw(
        const EditorWorkspaceRect& rectangle,
        const ProjectPaths& paths,
        const ProjectAssetRegistry& registry,
        const std::optional<MaterialPreviewImage>& preview,
        bool protectedAction = false);

    void DrawSceneObject(const EditorWorkspaceRect& rectangle, const SceneObjectInspectorView& object,
        const std::function<void()>& drawTransformControls,
        const ProjectPaths& paths, const ProjectAssetRegistry& registry, bool assignmentPending,
        const std::string& assignmentStatus, const std::function<void(std::string)>& requestAssignment);

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
    void DrawSceneMaterialPicker(
        const SceneObjectInspectorView& object,
        const ProjectPaths& paths,
        const ProjectAssetRegistry& registry,
        bool assignmentPending,
        const std::function<void(std::string)>& requestAssignment);

    std::array<char, 128> m_materialSearch{};

    std::vector<AssetRecord> m_materialChoices;

    std::optional<SceneObjectMetadata> m_materialPickerObject;

    std::filesystem::path m_materialPickerProject;

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
    void DrawTextureReference(
        const char* label,
        std::optional<std::string>& reference,
        const ProjectPaths& paths,
        const ProjectAssetRegistry& registry,
        bool selectionValid);

    void DrawTexturePickerContents(
        std::optional<std::string>& reference,
        const ProjectPaths& paths,
        const ProjectAssetRegistry& registry);

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
    std::vector<AssetRecord> m_availableTextures;

    bool m_refreshTextureChoices = true;

    std::filesystem::path m_projectRoot;

    std::optional<AssetRecord> m_record;

    std::optional<MaterialAssetData> m_material;

    std::string m_status = "No asset or scene object selected.";
};
