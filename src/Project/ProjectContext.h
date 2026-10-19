#pragma once

#include "Project/ProjectManifest.h"
#include "Project/ProjectPaths.h"

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

private:
    ProjectPaths m_paths;
    std::optional<ProjectManifest> m_manifest;
    std::filesystem::path m_manifestFileName;
};
