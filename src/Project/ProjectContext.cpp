#include "Project/ProjectContext.h"

#include <utility>

ProjectContext::ProjectContext(std::filesystem::path projectRoot)
    : m_paths(std::move(projectRoot))
{
}

ProjectContext::ProjectContext(std::filesystem::path projectRoot, ProjectManifest manifest)
    : m_paths(std::move(projectRoot))
    , m_manifest(std::move(manifest))
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

std::filesystem::path ProjectContext::ResolveCanonicalRoot(std::error_code& error) const
{
    return std::filesystem::canonical(m_paths.GetRoot(), error);
}
