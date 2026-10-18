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
// The context is immutable once constructed: project switching is not supported yet, so
// references handed out at initialization stay valid for the Engine's lifetime.
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

    ProjectContext(std::filesystem::path projectRoot, ProjectManifest manifest);

    const std::filesystem::path& GetRoot() const noexcept;
    const ProjectPaths& GetPaths() const noexcept;

    // Null for the legacy DefaultProject.
    const ProjectManifest* GetManifest() const noexcept;

    // Root with links and junctions resolved. Scene history and material revision tracking
    // use this as the project's identity. Fails when the root does not exist.
    std::filesystem::path ResolveCanonicalRoot(std::error_code& error) const;

private:
    ProjectPaths m_paths;
    std::optional<ProjectManifest> m_manifest;
};
