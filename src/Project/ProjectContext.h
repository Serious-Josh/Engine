#pragma once

#include "Project/ProjectLock.h"
#include "Project/ProjectManifest.h"
#include "Project/ProjectPaths.h"
#include "Project/ProjectSettings.h"

#include <filesystem>
#include <optional>
#include <system_error>

// Authoritative description of the project the Editor is working in.
//
// Engine owns the single active instance. Other subsystems receive a reference to it, or
// ProjectPaths values derived from it, instead of resolving a project root themselves.
// The root and paths are immutable once constructed: project switching is not supported yet, so
// references handed out at initialization stay valid for the Engine's lifetime. The only change
// is UpdateManifest, after Engine has persisted a shared setting to the locked manifest.
//
// The context owns no registry, scene, cache or GPU state. Every project path is derived
// from the root, so relocating the whole project directory relocates every derived path.
//
// A project opened from a .engineproj carries its manifest (stable identity, name, settings);
// the legacy manifest-less DefaultProject has none. The manifest never supplies the root: the
// root is the directory where the manifest was found, validated by ProjectLoader.
class ProjectContext
{
public:
    // Legacy DefaultProject bootstrap: no manifest.
    explicit ProjectContext(std::filesystem::path projectRoot);

    // manifestFileName is the manifest's file name inside projectRoot (no directories).
    ProjectContext(std::filesystem::path projectRoot, ProjectManifest manifest, std::filesystem::path manifestFileName);

    const std::filesystem::path& GetRoot() const noexcept;
    const ProjectPaths& GetPaths() const noexcept;

    // Null for the legacy DefaultProject.
    const ProjectManifest* GetManifest() const noexcept;

    // Replaces the in-memory manifest after the same content was written to disk. Requires an
    // existing manifest with the same project identity; returns false and changes nothing otherwise.
    bool UpdateManifest(ProjectManifest manifest);

    // Absolute path of the manifest file; empty for the legacy DefaultProject.
    std::filesystem::path GetManifestPath() const;

    // Root with links and junctions resolved. Scene history and material revision tracking
    // use this as the project's identity. Fails when the root does not exist.
    std::filesystem::path ResolveCanonicalRoot(std::error_code& error) const;

    // PS.1 shared project configuration (ProjectSettings.json). Defaults until LoadProjectSettings,
    // which whoever installs the context (ProjectLoader, the legacy bootstrap) calls once. Loading
    // only reads: a missing, malformed or newer file is never created, repaired or replaced here.
    const LoadedProjectSettings& GetProjectSettings() const noexcept;
    void LoadProjectSettings();

    // Explicit save. Only a project opened from its manifest may write, and only with full ownership of
    // that manifest's lock (ProjectLock::IsLockFor); read-only states (newer version, unreadable file)
    // are refused unchanged. On failure the in-memory settings are unchanged.
    bool SaveProjectSettings(const ProjectLock& lock, const ProjectSettings& settings, std::string& errorMessage);

private:
    ProjectPaths m_paths;
    std::optional<ProjectManifest> m_manifest;
    std::filesystem::path m_manifestFileName;
    LoadedProjectSettings m_projectSettings;
};
