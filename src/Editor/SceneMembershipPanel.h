#pragma once

#include "assets/ProjectSceneMembership.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class ProjectPaths;
struct EnvironmentAsset;
class ProjectAssetRegistry;

// W2 Editor workflows for World/Local membership: World creation, assignment, unassignment, placement and Local duplication.
// The panel only shows state and queues one request; Editor::ProcessSceneMembership performs it through ProjectSceneMembership
// (the one transactional path) and reports back. Scene state is read from the scene files and refreshed after every change.
class SceneMembershipPanel
{
public:
    SceneMembershipPanel() = default;
    SceneMembershipPanel(const SceneMembershipPanel&) = delete;
    SceneMembershipPanel& operator=(const SceneMembershipPanel&) = delete;
    // Stops and joins a scene read in flight.
    ~SceneMembershipPanel();

    enum class Action
    {
        CreateWorld,
        Assign,
        CreateAndAssign,
        Unassign,
        Placement,
        Duplicate,
        // W3: the World's own environment fields.
        WorldEnvironment
    };

    struct Request
    {
        Action action = Action::CreateWorld;
        std::optional<AssetID> world;
        std::optional<AssetID> local;
        // Project-relative paths of new scenes (World, new Local Level, duplicate), and their names.
        std::filesystem::path worldPath;
        std::string worldName;
        std::filesystem::path localPath;
        std::string localName;
        ProjectSceneMembership::EnvironmentSource environment = ProjectSceneMembership::EnvironmentSource::Default;
        ScenePlacement placement;
        // WorldEnvironment: the values the window showed (stale detection) and the edited ones.
        EffectiveSceneEnvironment expectedEnvironment;
        EffectiveSceneEnvironment worldEnvironment;
        // WorldEnvironment: a built-in HDRI (absolute path) to import into the project first; its reference then becomes the
        // World's sky. Empty when the sky reference is already a project asset, the neutral sky or none.
        std::filesystem::path worldBuiltinEnvironment;
    };

    // One HDRI the World environment editor offers, as the Editor's HDRI list shows it. A built-in HDRI has no project
    // reference until it is imported.
    struct EnvironmentChoice
    {
        std::string displayName;
        std::string reference;
        std::filesystem::path builtinSource;
    };

    // Re-reads every scene when stale or when the project changed. Startup work (4.3): on a thread of its own (below normal
    // priority, over copies of the paths and registry), so opening a project with large scenes never stalls a frame; the
    // previous state stays shown until the read finishes and is adopted by a later call.
    void Refresh(const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    void Invalidate() noexcept { m_stale = true; }

    // One line for an Asset Browser tooltip: World with its Local Level count, or Local Level with its World or "no World".
    std::string Describe(const std::filesystem::path& absolutePath) const;
    // Items for a scene entry's context menu.
    void DrawSceneContextItems(const std::filesystem::path& absolutePath, bool enabled);
    // "World..." in a folder's Create menu.
    void DrawCreateWorldMenuItem(const std::filesystem::path& directory, bool enabled);

    // Dialogs and the World Membership window; `locked` disables every action (scene editing locked or edits pending).
    void Draw(bool locked);

    // The HDRIs the World environment editor offers: the Renderer's list, with built-in files under `builtinRoot`. Copied only
    // when the list changed, so calling it every frame does not allocate.
    void SyncEnvironmentChoices(const std::vector<EnvironmentAsset>& environments, const std::filesystem::path& builtinRoot);

    std::optional<Request> TakeRequest();
    void OnCompleted(const std::string& error, const std::string& message);

private:
    friend class SceneMembershipEditorVerification;
    friend class CompositeWorldEditorVerification;

    const ProjectSceneMembership::SceneInfo* FindByPath(const std::filesystem::path& absolutePath) const;
    const ProjectSceneMembership::SceneInfo* FindByID(const std::string& id) const;
    std::filesystem::path Relative(const std::filesystem::path& absolutePath) const;
    void DrawCreateWorldModal(bool locked);
    void DrawDuplicateModal(bool locked);
    void DrawWorldWindow(bool locked);
    void Submit(Request request);

    std::filesystem::path m_root;
    bool m_stale = true;

    // Startup work (4.3): the read in flight, written only by its thread until `done`.
    struct PendingRead
    {
        std::filesystem::path root;
        std::vector<ProjectSceneMembership::SceneInfo> scenes;
        std::vector<std::string> problems;
        std::atomic<bool> done{false};
        std::atomic<bool> cancel{false};
    };
    std::shared_ptr<PendingRead> m_read;
    std::thread m_readThread;
    std::vector<ProjectSceneMembership::SceneInfo> m_scenes;
    std::vector<std::string> m_problems;

    std::optional<Request> m_request;
    bool m_pending = false;
    std::string m_status;
    bool m_statusIsError = false;

    // Create World dialog.
    bool m_openCreateWorld = false;
    std::filesystem::path m_createDirectory;
    std::array<char, 128> m_worldName{};
    std::array<char, 128> m_newLocalName{};
    int m_initialLocal = -1; // index into the unassigned Local Levels; -1 = a new Local Level
    int m_environment = 1;   // 0 Import, 1 Default

    // Duplicate dialog.
    bool m_openDuplicate = false;
    std::optional<AssetID> m_duplicateSource;
    std::filesystem::path m_duplicateDirectory;
    std::array<char, 128> m_duplicateName{};

    // World Membership window.
    bool m_showWorld = false;
    std::optional<AssetID> m_world;
    std::array<char, 128> m_addLocalName{};
    int m_assignLocal = -1;
    std::vector<std::array<float, 6>> m_placementDrafts;
    // W3: the World environment being edited (reset with the window and after every refresh).
    std::optional<EffectiveSceneEnvironment> m_worldEnvironmentDraft;
    int m_copyEnvironmentFrom = -1;
    // The built-in HDRI chosen for the draft's sky (imported on Apply); empty for a project, neutral or none sky.
    std::filesystem::path m_worldBuiltinEnvironmentDraft;
    std::vector<EnvironmentChoice> m_environmentChoices;
    void DrawWorldEnvironment(const ProjectSceneMembership::SceneInfo& world, bool locked);
    void DrawWorldSky(EffectiveSceneEnvironment& draft);
    void DrawWorldMist(EffectiveSceneEnvironment& draft);
};
