#include "Project/ProjectWorkspace.h"

#include "assets/ProjectAssetRegistry.h"

#include <exception>
#include <system_error>
#include <utility>

namespace
{
    std::string PathToUTF8(const std::filesystem::path& path)
    {
        const auto utf8 = path.generic_u8string();
        return std::string(utf8.begin(), utf8.end());
    }

    std::filesystem::path PathFromUTF8(const std::string& text)
    {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    bool IsSceneFileName(const std::filesystem::path& path)
    {
        return ProjectAssetRegistry::PathsEqual(path.extension(), ".scene");
    }

    // Canonical scene file inside the canonical project root, or nullopt with a reason.
    std::optional<std::filesystem::path> ResolveSceneInsideProject(
        const ProjectContext& project,
        const std::filesystem::path& sceneFile,
        std::string& errorMessage)
    {
        std::error_code error;
        const auto canonicalRoot = project.ResolveCanonicalRoot(error);

        if(error)
        {
            errorMessage = "The project directory cannot be resolved: " + error.message();
            return std::nullopt;
        }

        if(!std::filesystem::is_regular_file(sceneFile, error) || error)
        {
            errorMessage = "The scene file does not exist: " + PathToUTF8(sceneFile);
            return std::nullopt;
        }

        const auto canonicalScene = std::filesystem::canonical(sceneFile, error);

        if(error || !IsPathWithin(canonicalScene, canonicalRoot))
        {
            errorMessage = "The scene file resolves outside the project: " + PathToUTF8(sceneFile);
            return std::nullopt;
        }

        return canonicalScene;
    }
}

std::filesystem::path ProjectWorkspace::FilePath(const ProjectContext& project)
{
    return project.GetRoot() / ".engine" / "Workspace.json";
}

std::string ProjectWorkspace::IdentityOf(const ProjectContext& project)
{
    if(const auto* manifest = project.GetManifest())
    {
        return manifest->GetProjectID().ToString();
    }

    return std::string(kManifestlessIdentity);
}

ProjectWorkspace::ProjectWorkspace(std::string identity)
    : m_identity(std::move(identity))
{
}

ProjectWorkspace ProjectWorkspace::Load(const ProjectContext& project, LoadResult& result)
{
    std::string text;
    std::string error;
    result = {};
    result.status = PersonalSettingsFile::Read(FilePath(project), text, error);

    if(result.status != PersonalSettingsFile::Status::Loaded)
    {
        result.message = error;
        return ProjectWorkspace(IdentityOf(project));
    }

    return Parse(text, IdentityOf(project), result);
}

ProjectWorkspace ProjectWorkspace::Parse(std::string_view text, const std::string& expectedIdentity, LoadResult& result)
{
    ProjectWorkspace empty(expectedIdentity);
    result.status = PersonalSettingsFile::Status::Loaded;
    result.message.clear();
    nlohmann::json document;

    try
    {
        document = nlohmann::json::parse(text);
    }
    catch(const std::exception& exception)
    {
        result.status = PersonalSettingsFile::Status::Malformed;
        result.message = std::string("Workspace is not valid JSON: ") + exception.what();
        return empty;
    }

    const auto format = document.is_object() ? document.find("format") : document.end();
    const auto version = document.is_object() ? document.find("formatVersion") : document.end();
    const bool formatValid = format != document.end() && format->is_string() && format->get<std::string>() == kFormatName;

    if(!document.is_object() || !formatValid || version == document.end() || !version->is_number_integer())
    {
        result.status = PersonalSettingsFile::Status::Malformed;
        result.message = "Workspace lacks a valid format and formatVersion.";
        return empty;
    }

    if(version->get<long long>() != kCurrentFormatVersion)
    {
        const bool newer = version->get<long long>() > kCurrentFormatVersion;
        result.status = newer ? PersonalSettingsFile::Status::UnsupportedVersion : PersonalSettingsFile::Status::Malformed;
        result.message = "Workspace format version " + std::to_string(version->get<long long>()) + " is not supported.";
        return empty;
    }

    const auto identity = document.find("projectId");

    if(identity == document.end() || !identity->is_string() || identity->get<std::string>() != expectedIdentity)
    {
        result.status = PersonalSettingsFile::Status::IdentityMismatch;
        result.message = "Workspace belongs to a different project; it is ignored.";
        return empty;
    }

    ProjectWorkspace workspace(expectedIdentity);
    workspace.m_document = document;
    const auto lastScene = document.find("lastScene");

    if(lastScene != document.end() && lastScene->is_string())
    {
        const auto path = PathFromUTF8(lastScene->get<std::string>());

        if(IsContainedRelativePath(path) && IsSceneFileName(path))
        {
            workspace.m_lastScene = path.lexically_normal();
        }
        else
        {
            result.message = "Ignored an invalid lastScene path.";
        }
    }

    return workspace;
}

std::string ProjectWorkspace::Serialize() const
{
    nlohmann::json document = m_document;
    document["format"] = kFormatName;
    document["formatVersion"] = kCurrentFormatVersion;
    document["projectId"] = m_identity;

    if(m_lastScene.empty())
    {
        document["lastScene"] = nullptr;
    }
    else
    {
        document["lastScene"] = PathToUTF8(m_lastScene);
    }

    // Reserved for future project-specific Editor state (camera, panels, selection).
    if(!document.contains("editor") || !document["editor"].is_object())
    {
        document["editor"] = nlohmann::json::object();
    }

    return document.dump(4) + "\n";
}

const std::string& ProjectWorkspace::GetIdentity() const noexcept
{
    return m_identity;
}

const std::filesystem::path& ProjectWorkspace::GetLastScene() const noexcept
{
    return m_lastScene;
}

bool ProjectWorkspace::RecordLastScene(
    const ProjectContext& project,
    const std::filesystem::path& sceneFile,
    bool& changed,
    std::string& errorMessage)
{
    changed = false;
    errorMessage.clear();

    if(IdentityOf(project) != m_identity)
    {
        errorMessage = "The workspace belongs to a different project.";
        return false;
    }

    if(!IsSceneFileName(sceneFile))
    {
        errorMessage = "Only .scene files can be recorded as the last edited scene.";
        return false;
    }

    const auto canonicalScene = ResolveSceneInsideProject(project, sceneFile, errorMessage);

    if(!canonicalScene)
    {
        return false;
    }

    std::error_code error;
    const auto canonicalRoot = project.ResolveCanonicalRoot(error);
    const auto relative = canonicalScene->lexically_relative(canonicalRoot).lexically_normal();

    if(error || !IsContainedRelativePath(relative))
    {
        errorMessage = "The scene path cannot be expressed relative to the project.";
        return false;
    }

    changed = relative.native() != m_lastScene.native();
    m_lastScene = relative;
    return true;
}

std::optional<std::filesystem::path> ProjectWorkspace::ResolveLastScene(
    const ProjectContext& project,
    std::string& errorMessage) const
{
    errorMessage.clear();

    if(m_lastScene.empty())
    {
        errorMessage = "No last edited scene is recorded.";
        return std::nullopt;
    }

    if(IdentityOf(project) != m_identity)
    {
        errorMessage = "The workspace belongs to a different project.";
        return std::nullopt;
    }

    if(!IsContainedRelativePath(m_lastScene))
    {
        errorMessage = "The recorded scene path escapes the project.";
        return std::nullopt;
    }

    return ResolveSceneInsideProject(project, project.GetRoot() / m_lastScene, errorMessage);
}
