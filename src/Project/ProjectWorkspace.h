#pragma once

#include "Project/PersonalSettingsFile.h"
#include "Project/ProjectContext.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>

// Personal, per-project Editor state in <ProjectRoot>/.engine/Workspace.json. Not shared project
// configuration (that is the manifest) and not global preferences.
//
// Identity: manifest projects store the manifest's projectId; the manifest-less DefaultProject
// stores the fixed token "manifestless". A workspace whose identity differs from the opened
// project (for example a folder copied from another project) is ignored, never applied.
// Paths are project-relative, so relocating the project keeps the workspace valid.
class ProjectWorkspace
{
public:
    static constexpr std::string_view kFormatName = "EngineProjectWorkspace";
    static constexpr int kCurrentFormatVersion = 1;
    static constexpr std::string_view kManifestlessIdentity = "manifestless";

    struct LoadResult
    {
        PersonalSettingsFile::Status status = PersonalSettingsFile::Status::Missing;
        std::string message;
    };

    static std::filesystem::path FilePath(const ProjectContext& project);
    static std::string IdentityOf(const ProjectContext& project);

    // Never fails: problems yield an empty workspace bound to this project plus a status.
    static ProjectWorkspace Load(const ProjectContext& project, LoadResult& result);
    static ProjectWorkspace Parse(std::string_view text, const std::string& expectedIdentity, LoadResult& result);

    // An empty workspace for project (first run).
    explicit ProjectWorkspace(std::string identity);

    std::string Serialize() const;

    const std::string& GetIdentity() const noexcept;

    // Project-relative .scene path as recorded; not validated against the filesystem.
    const std::filesystem::path& GetLastScene() const noexcept;

    // Records a saved scene file. Rejected (state unchanged) unless sceneFile is an existing
    // .scene file whose canonical path is inside the project's canonical root. Returns whether
    // the stored value changed through changed.
    bool RecordLastScene(
        const ProjectContext& project,
        const std::filesystem::path& sceneFile,
        bool& changed,
        std::string& errorMessage);

    // For future restoration: the absolute path of the recorded scene if it still exists inside
    // the project, else nullopt with the reason. Never substitutes another scene.
    std::optional<std::filesystem::path> ResolveLastScene(
        const ProjectContext& project,
        std::string& errorMessage) const;

private:
    nlohmann::json m_document = nlohmann::json::object();
    std::string m_identity;
    std::filesystem::path m_lastScene;
};
