#include "Engine/Engine.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Platform/Windows/EditorDistributionPaths.h"
#include "Core/Log.h"
#include "imgui.h"

#include <Windows.h>

#include <chrono>
#include <exception>
#include <format>
#include <string>
#include <system_error>

// PR.5 personal state. Three scopes stay separate:
// - shared project settings: the .engineproj manifest (never written here);
// - per-project personal workspace: <ProjectRoot>/.engine/Workspace.json;
// - global personal preferences and ImGui layout: the Editor settings directory.
// Every failure here is recoverable: it is logged and the Editor continues with defaults.

namespace
{
    std::string PathText(const std::filesystem::path& path)
    {
        const auto utf8 = path.u8string();
        return std::string(utf8.begin(), utf8.end());
    }

    // Projects under the user temporary directory are scratch or test projects, not recents.
    bool IsInsideTemporaryDirectory(const std::filesystem::path& path)
    {
        wchar_t temporary[MAX_PATH + 1]{};
        const DWORD length = GetTempPathW(MAX_PATH + 1, temporary);

        if(length == 0 || length > MAX_PATH)
        {
            return false;
        }

        std::error_code error;
        const auto canonicalTemporary = std::filesystem::canonical(temporary, error);
        const auto canonicalPath = std::filesystem::canonical(path, error);
        return !error && IsPathWithin(canonicalPath, canonicalTemporary);
    }
}

void Engine::LoadEditorPreferences()
{
    m_personalSettingsActive = m_startup.persistPersonalSettings && m_startup.mode == EngineStartup::Mode::Editor;

    if(!m_personalSettingsActive)
    {
        return;
    }

    try
    {
        m_personalSettingsDirectory = m_startup.personalSettingsDirectory.empty() ?
            EditorDistributionPaths::EditorSettingsDirectory() : m_startup.personalSettingsDirectory;
    }
    catch(const std::exception& error)
    {
        Log::Warning(std::string("Editor preferences disabled: ") + error.what());
        m_personalSettingsActive = false;
        return;
    }

    const auto path = m_personalSettingsDirectory / "Preferences.json";
    EditorPreferences::LoadResult result;
    m_editorPreferences = EditorPreferences::Load(path, result);
    m_editorPreferencesStatus = result.status;

    // A file that is unchanged after loading is never rewritten.
    m_editorPreferencesSaved = m_editorPreferences.Serialize();

    if(result.status == PersonalSettingsFile::Status::Loaded)
    {
        Log::Info("Loaded Editor preferences: " + PathText(path) + (result.message.empty() ? "" : " (" + result.message + ")"));
    }
    else if(result.status != PersonalSettingsFile::Status::Missing)
    {
        Log::Warning(std::string("Editor preferences ") + PersonalSettingsFile::Describe(result.status) +
            "; using defaults: " + result.message);
    }

    m_editor.ApplyViewportPreferences(m_editorPreferences.GetViewport());
}

// The development Editor used to keep its layout in the working directory's imgui.ini. Personal
// layout now lives in the Editor settings directory. The first time, an existing working-
// directory imgui.ini is read once as the starting layout; it is never modified or deleted.
void Engine::ConfigureImGuiSettingsLocation()
{
    if(!m_personalSettingsActive)
    {
        return;
    }

    const auto settings = m_personalSettingsDirectory / "imgui.ini";
    std::error_code error;

    if(!std::filesystem::exists(settings, error))
    {
        const auto legacy = std::filesystem::current_path(error) / "imgui.ini";

        if(!error && std::filesystem::is_regular_file(legacy, error))
        {
            const auto legacyText = legacy.u8string();
            ImGui::LoadIniSettingsFromDisk(reinterpret_cast<const char*>(legacyText.c_str()));
            Log::Info("Editor layout: starting from " + PathText(legacy) + " (left unchanged); saving to " +
                PathText(settings));
        }
    }

    const auto settingsText = settings.u8string();
    m_imguiSettingsPath.assign(settingsText.begin(), settingsText.end());
    ImGui::GetIO().IniFilename = m_imguiSettingsPath.c_str();
}

void Engine::LoadProjectWorkspace()
{
    if(!m_personalSettingsActive || !m_activeProject)
    {
        return;
    }

    ProjectWorkspace::LoadResult result;
    m_projectWorkspace = ProjectWorkspace::Load(*m_activeProject, result);
    m_projectWorkspaceStatus = result.status;
    m_projectWorkspaceSaved = m_projectWorkspace->Serialize();

    if(m_startup.projectBrowserWorkflow &&
        (result.status != PersonalSettingsFile::Status::Missing &&
         (result.status != PersonalSettingsFile::Status::Loaded || !result.message.empty())))
    {
        m_projectNotice = "Workspace state could not be fully restored. " + result.message;
        Log::Warning(m_projectNotice);
    }

    if(result.status == PersonalSettingsFile::Status::Loaded)
    {
        const auto scene = m_projectWorkspace->GetLastScene();
        Log::Info("Loaded project workspace" + (scene.empty() ? std::string() : "; last edited scene: " + PathText(scene)) +
            (result.message.empty() ? "" : " (" + result.message + ")"));
    }
    else if(result.status != PersonalSettingsFile::Status::Missing)
    {
        Log::Warning(std::string("Project workspace ") + PersonalSettingsFile::Describe(result.status) +
            "; starting empty: " + result.message);
    }
}

void Engine::RecordRecentProject()
{
    if(!m_personalSettingsActive || !m_activeProject)
    {
        return;
    }

    const auto* manifest = m_activeProject->GetManifest();

    if(!manifest)
    {
        return;
    }

    const auto manifestPath = m_activeProject->GetManifestPath();

    if(IsInsideTemporaryDirectory(manifestPath))
    {
        Log::Info("Project under the temporary directory is not added to recent projects.");
        return;
    }

    RecentProject recent;
    recent.manifestPath = manifestPath;
    recent.projectID = manifest->GetProjectID().ToString();
    recent.displayName = manifest->GetDisplayName();
    recent.lastOpenedUtc = std::format("{:%FT%TZ}",
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));

    m_editorPreferences.RecordRecentProject(std::move(recent));
    SaveEditorPreferences();
}

void Engine::RecordLastEditedScene()
{
    if(!m_personalSettingsActive || !m_projectWorkspace || !m_activeProject)
    {
        return;
    }

    // Only a saved, editable file scene is recorded; an unsaved clean scene or an archive leaves
    // the previous reference intact.
    const auto* scene = dynamic_cast<const FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());

    if(!scene || scene->IsArchiveActivation() || scene->IsUnsaved())
    {
        return;
    }

    bool changed = false;
    std::string error;

    if(!m_projectWorkspace->RecordLastScene(*m_activeProject, scene->GetScenePath(), changed, error))
    {
        Log::Warning("Last edited scene not recorded: " + error);
        return;
    }

    if(changed)
    {
        SaveProjectWorkspace();
    }
}

void Engine::SaveEditorPreferences()
{
    if(!m_personalSettingsActive)
    {
        return;
    }

    m_editorPreferences.SetViewport(m_editor.GetViewportPreferences());
    const auto serialized = m_editorPreferences.Serialize();

    if(serialized == m_editorPreferencesSaved)
    {
        return;
    }

    std::string error;

    if(!PersonalSettingsFile::Save(m_personalSettingsDirectory / "Preferences.json", m_editorPreferencesStatus, serialized, error))
    {
        Log::Warning("Editor preferences not saved: " + error);
        return;
    }

    m_editorPreferencesSaved = serialized;
    m_editorPreferencesStatus = PersonalSettingsFile::Status::Loaded;
}

void Engine::SaveProjectWorkspace()
{
    if(!m_personalSettingsActive || !m_projectWorkspace || !m_activeProject)
    {
        return;
    }

    const auto serialized = m_projectWorkspace->Serialize();

    if(serialized == m_projectWorkspaceSaved)
    {
        return;
    }

    std::string error;

    if(!PersonalSettingsFile::Save(ProjectWorkspace::FilePath(*m_activeProject), m_projectWorkspaceStatus, serialized, error))
    {
        Log::Warning("Project workspace not saved: " + error);
        return;
    }

    m_projectWorkspaceSaved = serialized;
    m_projectWorkspaceStatus = PersonalSettingsFile::Status::Loaded;
}

std::optional<std::filesystem::path> Engine::ResolveLastEditedScene(std::string& errorMessage) const
{
    errorMessage.clear();

    if(!m_projectWorkspace || !m_activeProject)
    {
        errorMessage = "No project workspace is loaded.";
        return std::nullopt;
    }

    return m_projectWorkspace->ResolveLastScene(*m_activeProject, errorMessage);
}
