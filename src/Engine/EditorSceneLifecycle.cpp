#include "Engine/Engine.h"
#include "Core/Log.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Platform/Windows/EditorFileDialogs.h"
#include "Project/ProjectCreation.h"
#include "Scene/SceneFileCreation.h"
#include "Scene/SceneSerializer.h"
#include "imgui.h"

namespace
{
    bool ResolveSceneLocation(const ProjectContext& project, const std::filesystem::path& requested,
        bool mustExist, std::filesystem::path& resolved, std::string& error)
    {
        std::error_code filesystemError;
        const auto root = project.ResolveCanonicalRoot(filesystemError);

        if(filesystemError || !requested.is_absolute() ||
            !ProjectAssetRegistry::PathsEqual(requested.extension(), ".scene"))
        {
            error = "Choose an absolute .scene file inside the active project.";
            return false;
        }

        const auto parent = std::filesystem::canonical(requested.parent_path(), filesystemError);

        if(filesystemError || !IsPathWithin(parent, root))
        {
            error = "The scene directory is inaccessible or resolves outside the active project.";
            return false;
        }

        resolved = parent / requested.filename();
        const bool exists = std::filesystem::exists(resolved, filesystemError);

        if(filesystemError || (mustExist && !exists))
        {
            error = "The scene file is missing or inaccessible.";
            return false;
        }

        if(exists)
        {
            resolved = std::filesystem::canonical(resolved, filesystemError);

            if(filesystemError || !IsPathWithin(resolved, root) ||
                !std::filesystem::is_regular_file(resolved, filesystemError) || filesystemError)
            {
                error = "The scene must be a regular file inside the project; links cannot escape it.";
                return false;
            }
        }

        return true;
    }
}

bool Engine::ResolveEditorSceneLocation(const std::filesystem::path& requested, bool mustExist,
    std::filesystem::path& resolved, std::string& error) const
{
    return ResolveSceneLocation(GetActiveProject(), requested, mustExist, resolved, error);
}

bool Engine::CanPerformSceneLifecycle(std::string& error) const
{
    const bool busy = m_pendingSceneObjectCreation || m_pendingSceneObjectRestoration || m_pendingSceneObjectDeletion ||
        m_pendingMaterialAssignment || m_pendingViewportMaterialBinding || m_pendingEditorMembership ||
        m_demoSceneManager.HasPendingSceneChange() || m_editor.HasPendingSceneHistoryOperation() ||
        m_editor.HasBlockingObjectCreation() || m_buildGame.Poll().busy;

    if(m_projectOwnershipLost)
    {
        error = "Project ownership could not be verified; project changes are disabled. Retry or close the Editor.";
        return false;
    }

    if(m_pendingRelaunch)
    {
        error = "A project is being opened in a new Editor; wait until it starts or fails.";
        return false;
    }

    if(m_state != EngineState::Running || m_startup.mode != EngineStartup::Mode::Editor || !m_activeProject ||
        PhysicsBlocksSceneEdits() || busy)
    {
        error = "Finish pending edits/builds and stop physics before using scene or project lifecycle commands.";
        return false;
    }

    return true;
}

Engine::SceneSaveResult Engine::SaveEditorScene(FileBackedDemoScene& scene, bool saveAs,
    const std::filesystem::path& selectedPath, bool overwriteConfirmed)
{
    m_sceneOperationError.clear();
    if(!CanPerformSceneLifecycle(m_sceneOperationError) || scene.IsArchiveActivation())
    {
        return SceneSaveResult::Failed;
    }

    const bool choosingPath = saveAs || scene.IsUnsaved();
    auto path = choosingPath ? selectedPath : scene.GetScenePath();
    const bool suppliedPath = !path.empty();

    if(choosingPath && path.empty())
    {
        std::optional<std::filesystem::path> chosen;
#if defined(ENGINE_HAS_VERIFICATION)
        if(m_sceneDialogForVerification)
        {
            chosen = m_sceneDialogForVerification(true);
        }
        else
#endif
        {
            chosen = EditorFileDialogs::Select(EditorFileDialogs::Kind::SaveScene, m_window.GetNativeHandle(),
                GetActiveProject().GetPaths().GetScenesDirectory(), m_sceneOperationError);
        }

        if(!chosen)
        {
            return m_sceneOperationError.empty() ? SceneSaveResult::Canceled : SceneSaveResult::Failed;
        }

        path = *chosen;
    }

    std::filesystem::path resolved;

    if(!ResolveSceneLocation(GetActiveProject(), path, false, resolved, m_sceneOperationError))
    {
        return SceneSaveResult::Failed;
    }

    std::error_code filesystemError;
    const bool exists = std::filesystem::exists(resolved, filesystemError);

    if(filesystemError)
    {
        m_sceneOperationError = "Cannot inspect the destination scene file.";
        return SceneSaveResult::Failed;
    }

    if(choosingPath && exists && !overwriteConfirmed)
    {
        if(suppliedPath)
        {
            m_sceneOperationError = "Replacing an existing scene requires explicit overwrite confirmation.";
            return SceneSaveResult::Failed;
        }

        bool approved = false;
#if defined(ENGINE_HAS_VERIFICATION)
        if(m_overwriteForVerification)
        {
            approved = m_overwriteForVerification(resolved);
        }
        else
#endif
        {
            approved = EditorFileDialogs::ConfirmOverwrite(m_window.GetNativeHandle(), resolved);
        }

        if(!approved)
        {
            return SceneSaveResult::Canceled;
        }
    }

    // Revalidate the canonical destination after a modal dialog pumps window messages.
    if(!CanPerformSceneLifecycle(m_sceneOperationError) ||
        !ResolveSceneLocation(GetActiveProject(), resolved, false, path, m_sceneOperationError))
    {
        return SceneSaveResult::Failed;
    }

    const auto previousPath = scene.GetScenePath();

    // A previously absent destination is published without replacement, including late collisions.
    if(!scene.SaveAs(path, &m_sceneOperationError, exists))
    {
        return SceneSaveResult::Failed;
    }

    m_editor.RebindSceneHistoryAfterSaveAs(scene, previousPath);
    m_editor.InvalidateSceneFiles();
    RecordLastEditedScene();
    m_sceneOperationError.clear();
    return SceneSaveResult::Saved;
}

void Engine::ProcessEditorSceneRequests(IDemoScene& activeScene)
{
    if(m_projectCreationPending)
    {
        const auto request = m_projectBrowser.Draw(m_editorPreferences, m_window.GetNativeHandle(), true);

        if(request)
        {
            ProcessProjectBrowserRequest(*request);
        }

        return;
    }

    if(const auto request = m_editor.TakeFileRequest())
    {
        m_sceneOperationError.clear();

        if(m_pendingEditorSceneOperation || !CanPerformSceneLifecycle(m_sceneOperationError))
        {
            m_editor.ReportLifecycleMessage(m_sceneOperationError.empty() ? "Another lifecycle operation is pending." : m_sceneOperationError);
        }
        else if(request->type == Editor::FileRequestType::Save || request->type == Editor::FileRequestType::SaveAs)
        {
            m_editor.SetSceneOperationPending(true);
            auto* scene = dynamic_cast<FileBackedDemoScene*>(&activeScene);

            if(!scene)
            {
                m_sceneOperationError = "The active demo has no editable scene document. Create a New Scene first.";
            }
            else
            {
                SaveEditorScene(*scene, request->type == Editor::FileRequestType::SaveAs, request->path);
            }

            if(!m_sceneOperationError.empty())
            {
                m_editor.ReportLifecycleMessage(m_sceneOperationError);
            }
        }
        else if(request->type == Editor::FileRequestType::ProjectSettings)
        {
            // The view is owned by Editor; no manifest writer or lifecycle transition.
        }
        else
        {
            // Native dialogs pump Win32 messages. Reserve this command before showing one,
            // so repeated close/menu requests cannot queue a second destructive transition.
            m_editor.SetSceneOperationPending(true);
            auto operation = *request;

            if(operation.type == Editor::FileRequestType::Load && operation.path.empty())
            {
                std::optional<std::filesystem::path> selected;
#if defined(ENGINE_HAS_VERIFICATION)
                if(m_sceneDialogForVerification)
                {
                    selected = m_sceneDialogForVerification(false);
                }
                else
#endif
                {
                    selected = EditorFileDialogs::Select(EditorFileDialogs::Kind::OpenScene, m_window.GetNativeHandle(),
                        GetActiveProject().GetPaths().GetScenesDirectory(), m_sceneOperationError);
                }

                if(!selected)
                {
                    if(!m_sceneOperationError.empty())
                    {
                        m_editor.ReportLifecycleMessage(m_sceneOperationError);
                    }

                    return;
                }

                operation.path = *selected;
            }

            if(operation.type == Editor::FileRequestType::Load)
            {
                SceneDocument document;
                std::filesystem::path resolved;

                if(!ResolveSceneLocation(GetActiveProject(), operation.path, true, resolved, m_sceneOperationError) ||
                    !LoadSceneDocument(resolved, document, &m_sceneOperationError))
                {
                    m_editor.ReportLifecycleMessage(m_sceneOperationError);
                    return;
                }

                operation.path = resolved;
            }

            bool dirty = false;
            const auto* scene = dynamic_cast<const FileBackedDemoScene*>(&activeScene);

            if(scene && !scene->CheckUnsavedChanges(dirty, &m_sceneOperationError))
            {
                m_editor.ReportLifecycleMessage(m_sceneOperationError);
                return;
            }

            m_pendingEditorSceneOperation = std::move(operation);
            m_sceneDecisionPending = dirty;

            if(!dirty)
            {
                ContinueEditorSceneOperation();
            }
        }
    }

    if(m_pendingEditorSceneOperation && m_sceneDecisionPending)
    {
        ResolveSceneChangeDecision(m_editor.DrawSceneChangeConfirmation(m_sceneOperationError));
    }
}

void Engine::ResolveSceneChangeDecision(Editor::SceneChangeDecision decision)
{
    if(!m_pendingEditorSceneOperation || !m_sceneDecisionPending || decision == Editor::SceneChangeDecision::None)
    {
        return;
    }

    if(decision == Editor::SceneChangeDecision::Cancel)
    {
        m_pendingEditorSceneOperation.reset();
        m_sceneDecisionPending = false;
        m_sceneOperationError.clear();
        return;
    }

    if(!CanPerformSceneLifecycle(m_sceneOperationError))
    {
        return;
    }

    if(decision == Editor::SceneChangeDecision::Save)
    {
        auto* scene = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());

        if(!scene || SaveEditorScene(*scene, false) != SceneSaveResult::Saved)
        {
            if(m_sceneOperationError.empty())
            {
                m_sceneOperationError = "Saving was canceled. The original operation is still pending; retry or Cancel.";
            }

            return;
        }
    }

    m_sceneDecisionPending = false;
    ContinueEditorSceneOperation();
}

void Engine::QueueEditorSceneOperation()
{
    ContinueEditorSceneOperation();
}

void Engine::ContinueEditorSceneOperation()
{
    if(!m_pendingEditorSceneOperation)
    {
        return;
    }

    if(!CanPerformSceneLifecycle(m_sceneOperationError))
    {
        m_editor.ReportLifecycleMessage(m_sceneOperationError);
        m_pendingEditorSceneOperation.reset();
        m_sceneDecisionPending = false;
        return;
    }

    const auto request = *m_pendingEditorSceneOperation;

    if(request.type == Editor::FileRequestType::NewProject)
    {
        m_projectBrowser.message.clear();
        m_projectCreationPending = true;
        return;
    }

    m_pendingEditorSceneOperation.reset();
    std::string error;

    switch(request.type)
    {
    case Editor::FileRequestType::Exit:
        m_window.Close();
        return;
    case Editor::FileRequestType::OpenProject:
    {
        auto path = request.path;

        if(path.empty())
        {
            const auto selected = EditorFileDialogs::Select(EditorFileDialogs::Kind::OpenProject,
                m_window.GetNativeHandle(), GetActiveProject().GetRoot(), error);

            if(!selected)
            {
                break;
            }

            path = *selected;
        }

        RelaunchEditorProject(path);
        return;
    }
    case Editor::FileRequestType::New:
    {
        SceneDocument document;

        if(CreateCleanSceneDocument(document, &error))
        {
            m_demoSceneManager.RequestUnsavedScene(std::move(document), &error);
        }

        break;
    }
    case Editor::FileRequestType::Load:
    {
        SceneDocument document;
        std::filesystem::path path;

        if(ResolveSceneLocation(GetActiveProject(), request.path, true, path, error) &&
            LoadSceneDocument(path, document, &error))
        {
            m_demoSceneManager.RequestFileScene(path, &error);
        }

        break;
    }
    case Editor::FileRequestType::Demo:
        if(!m_demoSceneManager.RequestScene(request.sceneName))
        {
            error = "Cannot request the selected demo scene.";
        }

        break;
    default:
        break;
    }

    if(!error.empty())
    {
        m_editor.ReportLifecycleMessage(error);
    }
}

void Engine::RelaunchEditorProject(const std::filesystem::path& manifest, bool created)
{
    std::string error;
    const auto text = manifest.u8string();
    const std::string prefix = created ?
        "Project was created at " + std::string(text.begin(), text.end()) + " but not opened. " : "";

    if(!CanPerformSceneLifecycle(error))
    {
        m_editor.ReportLifecycleMessage(prefix + error);
        return;
    }

    std::error_code filesystemError;
    const auto root = std::filesystem::canonical(manifest.parent_path(), filesystemError);
    std::error_code activeError;
    const auto activeRoot = GetActiveProject().ResolveCanonicalRoot(activeError);

    if(activeError)
    {
        m_editor.ReportLifecycleMessage(prefix + "Cannot verify the current project's location; relaunch was canceled.");
        return;
    }

    if(!filesystemError && IsPathWithin(root, activeRoot) && IsPathWithin(activeRoot, root))
    {
        m_editor.ReportLifecycleMessage("This project is already active; no second Editor was launched.");
        return;
    }

    auto opened = ProjectLoader::Open(manifest, error);

    if(!opened)
    {
        m_editor.ReportLifecycleMessage(prefix + error);
        return;
    }

    const auto destination = opened->context.GetManifestPath();
    opened.reset(); // Release only the target's preflight lock, never the active project's lock.
    SaveEditorPreferences();
    SaveProjectWorkspace();

    if(m_personalSettingsActive && ImGui::GetIO().IniFilename)
    {
        std::filesystem::create_directories(m_personalSettingsDirectory, filesystemError);

        if(!filesystemError)
        {
            ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
        }
    }

    bool launched = false;
    EditorFileDialogs::ChildEditor child;
#if defined(ENGINE_HAS_VERIFICATION)
    if(m_beforeRelaunchForVerification)
    {
        m_beforeRelaunchForVerification(destination);
    }

    if(m_relaunchForVerification)
    {
        launched = m_relaunchForVerification(destination, error);
    }
    else
#endif
    {
        launched = EditorFileDialogs::Relaunch(destination, error, &child);
    }

    if(!launched)
    {
        m_editor.ReportLifecycleMessage(prefix + error);
        return;
    }

    if(!child.process)
    {
        // Injected verification launch: no process to observe.
        CompleteRelaunch();
        return;
    }

    // Process creation is not proof that the child opened the project; wait for evidence.
    Log::Info("Replacement Editor process created; waiting for it to open the project.");
    m_pendingRelaunch = PendingRelaunch{child, prefix, std::chrono::steady_clock::now()};
    m_editor.ReportLifecycleMessage("Opening the project in a new Editor...");
}

void Engine::ProcessPendingRelaunch()
{
    if(!m_pendingRelaunch)
    {
        return;
    }

    constexpr auto kStartTimeout = std::chrono::minutes(2);
    unsigned long exitCode = 0;
    const auto state = EditorFileDialogs::PollChild(m_pendingRelaunch->child, exitCode);

    if(state == EditorFileDialogs::ChildState::Starting &&
        std::chrono::steady_clock::now() - m_pendingRelaunch->started < kStartTimeout)
    {
        return;
    }

    const auto prefix = m_pendingRelaunch->prefix;
    EditorFileDialogs::CloseChild(m_pendingRelaunch->child);
    m_pendingRelaunch.reset();

    if(state == EditorFileDialogs::ChildState::WindowShown)
    {
        Log::Info("Replacement Editor opened its project; closing this Editor.");
        m_editor.ReportLifecycleMessage({});
        CompleteRelaunch();
        return;
    }

    std::string message = prefix;

    if(state == EditorFileDialogs::ChildState::Exited)
    {
        message += "The new Editor could not open the project (exit code " + std::to_string(exitCode) +
            "). This Editor remains open.";
    }
    else
    {
        message += "The new Editor has not finished starting after two minutes. This Editor remains open; "
            "close whichever Editor you do not need.";
    }

    Log::Warning(message);
    m_editor.ReportLifecycleMessage(message);
}

void Engine::CompleteRelaunch()
{
    // Personal state was flushed before spawning. Do not overwrite the child's newer
    // recent-project list or layout during this process's remaining shutdown work.
    m_editorRelaunchStarted = true;
    ImGui::GetIO().IniFilename = nullptr;
    m_window.Close(); // Current project's lock is released only by normal checked Shutdown.
}
