#include "Engine/Engine.h"
#include "Core/Log.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Platform/Windows/EditorFileDialogs.h"
#include "Project/ProjectCreation.h"
#include "Scene/SceneFileCreation.h"
#include "Scene/SceneSerializer.h"
#include "assets/ProjectCompositeWorld.h"
#include "assets/ProjectSceneMembership.h"
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

    // W5: a composite World is saved as its source documents (Save = Save All), never as a scene of its own; Save As is refused
    // before any dialog could open.
    if(scene.GetCompositeWorld())
    {
        if(saveAs)
        {
            m_sceneOperationError = "A World cannot be saved as a new scene: it is composed from the World and its Local Levels. Use "
                "Save (all changed documents) or save one document from the World panel.";
            m_editor.ReportLifecycleMessage(m_sceneOperationError);
            return SceneSaveResult::Failed;
        }

        return SaveCompositeWorld(scene, std::nullopt);
    }

    // W2: Save As of a saved scene creates a new, independent asset (new scene and entity identities) from the open document,
    // then switches the Editor to it. It is refused for an assigned Local Level (the copy would claim its World membership) and
    // for a World. The first save of a new, never-saved scene keeps its identity (it is not a copy).
    const bool saveAsNewAsset = saveAs && !scene.IsUnsaved();

    if(saveAsNewAsset)
    {
        const auto* document = scene.GetSceneDocument();

        if(!document || document->kind == SceneKind::World)
        {
            m_sceneOperationError = "Save As is not available for a World.";
            return SceneSaveResult::Failed;
        }

        if(!document->owningWorld.empty())
        {
            m_sceneOperationError = "Save As is not available for a Local Level assigned to a World: the copy would share its "
                "World membership. Use Duplicate Local Level in the Asset Browser.";
            return SceneSaveResult::Failed;
        }
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

    if(saveAsNewAsset)
    {
        // A new asset never replaces a file: that file is another scene asset with its own identity.
        if(exists)
        {
            m_sceneOperationError = "Save As creates a new scene and cannot replace the existing file " + resolved.filename().string() +
                ". Choose a new file name.";
            return SceneSaveResult::Failed;
        }

        const auto relative = resolved.lexically_relative(GetActiveProject().GetPaths().GetRoot());
        ProjectSceneMembership::Result result;
        // Exactly what Save would write now: the open document with its live (unsaved) edits captured.
        SceneDocument snapshot;

        if(!scene.CaptureDocumentForSave(snapshot, &m_sceneOperationError))
        {
            return SceneSaveResult::Failed;
        }

        // One transaction: the new file and its registry record. The open scene's file is untouched.
        if(!ProjectSceneMembership::SaveLocalAsNew(GetActiveProject().GetPaths(), m_projectAssetRegistry, snapshot,
            relative, result, m_sceneOperationError))
        {
            return SceneSaveResult::Failed;
        }

        // Continue editing the new asset: open it as a fresh scene (new entity identities need a new runtime and history). The
        // edits are in the new file; if the open fails, the current scene stays open with its edits.
        std::string requestError;

        if(!m_demoSceneManager.RequestFileScene(resolved, &requestError))
        {
            m_sceneOperationError = "Saved as " + resolved.filename().string() + ", but it could not be opened: " + requestError;
            m_editor.InvalidateSceneFiles();
            return SceneSaveResult::Failed;
        }

        Log::Info("Saved as a new Local Level " + relative.generic_string() + " (" + result.local->ToString() + ").");
        m_editor.InvalidateSceneFiles();
        m_sceneOperationError.clear();
        return SceneSaveResult::Saved;
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
                m_pendingCompositeWorld.reset();

                const bool readable = ResolveSceneLocation(GetActiveProject(), operation.path, true, resolved, m_sceneOperationError) &&
                    LoadSceneDocument(resolved, document, &m_sceneOperationError);

                // W4: a World opens as a read-only composite of all its Local Levels. It is composed and validated completely
                // here, before the current scene is touched, so an invalid World leaves the active scene in place.
                if(readable && document.kind == SceneKind::World)
                {
                    const auto composeStart = std::chrono::steady_clock::now();
                    auto composite = std::make_shared<CompositeWorld>();

                    if(ProjectCompositeWorld::Load(GetActiveProject().GetPaths(), m_projectAssetRegistry, resolved, *composite,
                        m_sceneOperationError))
                    {
                        for(const auto& problem : composite->unverifiedScenes)
                        {
                            Log::Warning("World '" + composite->worldName + "': membership not verified against an unreadable scene: " +
                                problem);
                        }

                        m_pendingCompositeWorld = std::move(composite);
                        m_compositeWorldTiming = {};
                        m_compositeWorldTiming.composeMilliseconds =
                            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - composeStart).count();
                        m_compositeWorldTiming.requested = std::chrono::steady_clock::now();
                        m_compositeWorldTiming.reported = false;
                    }
                }

                // A World that could not be composed keeps the composition error (it is not a "World cannot be opened" case).
                const bool isWorld = readable && document.kind == SceneKind::World;
                const bool openable = readable && (isWorld ? bool(m_pendingCompositeWorld) : ValidateSceneKindOpenable(document, &m_sceneOperationError));

                if(!openable)
                {
                    // The dialog is not the only record: a failed open is also logged, so unattended runs can see why.
                    Log::Error("Could not open scene " + operation.path.string() + ": " + m_sceneOperationError);
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
        std::filesystem::path path;

        // S0.7B: the document was validated when the operation was queued, and staging parses it again (its Open step
        // reports a failure and preserves the current scene), so it is not parsed a third time here.
        if(m_pendingCompositeWorld)
        {
            // W4: the composite was built and validated when the operation was queued.
            m_demoSceneManager.RequestCompositeWorld(std::move(m_pendingCompositeWorld), &error);
            m_pendingCompositeWorld.reset();
        }
        else if(ResolveSceneLocation(GetActiveProject(), request.path, true, path, error))
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

// W4: logs a World open once its composite scene is active: composition time, request-to-activation latency and its size.
void Engine::ReportCompositeWorldActivation(const FileBackedDemoScene* scene)
{
    const CompositeWorld* composite = scene ? scene->GetCompositeWorld() : nullptr;

    if(m_compositeWorldTiming.reported || !composite)
    {
        return;
    }

    m_compositeWorldTiming.reported = true;
    const double activationMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_compositeWorldTiming.requested).count();
    Log::Info("World open: '" + composite->worldName + "', " + std::to_string(composite->levels.size()) + " Local Level(s), " +
        std::to_string(composite->document.objects.size()) + " composite objects; compose " +
        std::to_string(m_compositeWorldTiming.composeMilliseconds) + " ms, request to activation " +
        std::to_string(activationMilliseconds) + " ms.");
}

// W5: saves the composite World's changed source documents (all, or one) in one transaction. Nothing is written, and no baseline
// changes, unless every document validates and still matches the bytes it was read from.
Engine::SceneSaveResult Engine::SaveCompositeWorld(FileBackedDemoScene& scene, std::optional<std::size_t> onlyDocument)
{
    std::size_t saved = 0;
    m_sceneOperationError.clear();

    if(!scene.SaveCompositeDocuments(GetActiveProject().GetPaths(), m_projectAssetRegistry, onlyDocument, saved, &m_sceneOperationError))
    {
        Log::Error("World save failed: " + m_sceneOperationError);
        m_editor.ReportLifecycleMessage(m_sceneOperationError);
        return SceneSaveResult::Failed;
    }

    Log::Info("World '" + scene.GetCompositeWorld()->worldName + "': saved " + std::to_string(saved) + " document(s).");
    m_editor.InvalidateSceneFiles();
    return SceneSaveResult::Saved;
}

// W5: a Save or Save All request from the World panel.
void Engine::ProcessCompositeSaveRequest(FileBackedDemoScene* scene)
{
    const auto request = m_editor.TakeCompositeSaveRequest();

    if(!request || !scene || !scene->GetCompositeWorld())
    {
        return;
    }

    std::string error;

    if(!CanPerformSceneLifecycle(error))
    {
        m_editor.ReportLifecycleMessage(error);
        return;
    }

    SaveCompositeWorld(*scene, *request);
}
