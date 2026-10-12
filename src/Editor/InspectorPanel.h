#pragma once

#include "assets/AssetRecord.h"
#include "assets/MaterialAssetData.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

struct EditorWorkspaceRect;
class ProjectPaths;
class ProjectAssetRegistry;

class InspectorPanel
{
public:
    void Draw(
        const EditorWorkspaceRect& rectangle,
        const ProjectPaths& paths,
        const ProjectAssetRegistry& registry,
        bool protectedAction = false);

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
    void LoadSelection(
        const std::filesystem::path& selection,
        const ProjectPaths& paths,
        const ProjectAssetRegistry& registry);

private:
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
    std::array<char, 128> m_textureSearch{};
    std::vector<AssetRecord> m_texturePickerRegistry;
    std::vector<AssetRecord> m_availableTextures;
    bool m_refreshTextureChoices = true;

    std::filesystem::path m_projectRoot;

    std::optional<AssetRecord> m_record;
    std::optional<MaterialAssetData> m_material;

    std::string m_status = "No asset or scene object selected.";
};
