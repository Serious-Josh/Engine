#include "Project/ProjectContext.h"

#include <utility>

ProjectContext::ProjectContext(std::filesystem::path projectRoot)
    : m_paths(std::move(projectRoot))
{
}

ProjectContext::ProjectContext(std::filesystem::path projectRoot, ProjectManifest manifest, std::filesystem::path manifestFileName)
    : m_paths(std::move(projectRoot))
    , m_manifest(std::move(manifest))
    , m_manifestFileName(std::move(manifestFileName))
{
}

const std::filesystem::path& ProjectContext::GetRoot() const noexcept
{
    return m_paths.GetRoot();
}

const ProjectPaths& ProjectContext::GetPaths() const noexcept
{
    return m_paths;
}

const ProjectManifest* ProjectContext::GetManifest() const noexcept
{
    return m_manifest ? &*m_manifest : nullptr;
}

bool ProjectContext::UpdateManifest(ProjectManifest manifest)
{
    if(!m_manifest || manifest.GetProjectID() != m_manifest->GetProjectID())
    {
        return false;
    }

    m_manifest = std::move(manifest);
    return true;
}

std::filesystem::path ProjectContext::GetManifestPath() const
{
    if(!m_manifest)
    {
        return {};
    }

    return m_paths.GetRoot() / m_manifestFileName;
}

std::filesystem::path ProjectContext::ResolveCanonicalRoot(std::error_code& error) const
{
    return std::filesystem::canonical(m_paths.GetRoot(), error);
}

const LoadedProjectSettings& ProjectContext::GetProjectSettings() const noexcept
{
    return m_projectSettings;
}

void ProjectContext::LoadProjectSettings()
{
    m_projectSettings = ProjectSettingsFile::Load(m_paths.GetProjectSettingsPath());
}

bool ProjectContext::SaveProjectSettings(const ProjectLock& lock, const ProjectSettings& settings,
    std::string& errorMessage)
{
    errorMessage.clear();

    if(!m_manifest || !lock.IsLockFor(GetManifestPath()))
    {
        errorMessage = "Project settings can be saved only in a project opened from its locked .engineproj manifest.";
        return false;
    }

    // Save updates the copy only after the file was replaced, so a failure leaves memory and disk agreeing.
    LoadedProjectSettings candidate = m_projectSettings;

    if(!ProjectSettingsFile::Save(m_paths.GetProjectSettingsPath(), candidate, settings, errorMessage))
    {
        return false;
    }

    m_projectSettings = std::move(candidate);
    return true;
}
