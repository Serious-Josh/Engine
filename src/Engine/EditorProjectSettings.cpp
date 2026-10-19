#include "Engine/Engine.h"
#include "Core/Log.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Scene/SceneSerializer.h"

// Shared project settings (PR.9): the Build Game startup scene stored in the .engineproj manifest.
// Personal workspace state (the last edited scene) is separate and never written here.

namespace
{
    std::string PathText(const std::filesystem::path& path)
    {
        const auto text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    }
}

bool Engine::ResolveBuildStartupScene(BuildStartupScene& startup, std::string& error)
{
    startup = {};
    const auto* manifest = GetActiveProject().GetManifest();
    const auto configured = manifest ? manifest->GetStartupScene() : std::filesystem::path{};

    auto* active = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    const bool activeSceneSaved = active && !active->IsArchiveActivation() && !active->IsUnsaved();

    if(!configured.empty())
    {
        // A configured scene that cannot be used fails the build. Falling back to another scene
        // would silently package a game that starts somewhere the project did not choose.
        std::string reason;

        if(!ResolveEditorSceneLocation(GetActiveProject().GetRoot() / configured, true, startup.path, reason))
        {
            error = "The configured game startup scene " + PathText(configured) + " cannot be used: " + reason +
                " Choose another scene or clear the setting in File > Project Settings.";
            return false;
        }

        startup.configured = true;
    }
    else
    {
        if(!activeSceneSaved)
        {
            error = "No game startup scene is configured and the active scene has not been saved. Save the scene, "
                "or choose a startup scene in File > Project Settings.";
            return false;
        }

        std::string reason;

        if(!ResolveEditorSceneLocation(active->GetScenePath(), true, startup.path, reason))
        {
            error = "The active scene cannot be used as the game startup scene: " + reason;
            return false;
        }
    }

    if(activeSceneSaved)
    {
        std::filesystem::path activePath;
        std::string ignored;

        if(ResolveEditorSceneLocation(active->GetScenePath(), true, activePath, ignored) &&
            ProjectAssetRegistry::PathsEqual(activePath, startup.path))
        {
            startup.activeOwner = active;
        }
    }

    return true;
}

bool Engine::SetProjectStartupScene(const std::filesystem::path& scene, std::string& message)
{
    message.clear();

    if(m_startup.mode != EngineStartup::Mode::Editor || !m_activeProject)
    {
        message = "Project settings are available only in the Editor with an open project.";
        return false;
    }

    const auto* manifest = GetActiveProject().GetManifest();

    if(m_projectOwnershipLost)
    {
        message = "Project ownership could not be verified; shared settings cannot be saved.";
        return false;
    }

    if(!manifest || !m_projectLock.IsHeld())
    {
        message = "This project was not opened from a locked .engineproj manifest, so shared settings cannot be saved.";
        return false;
    }

    std::filesystem::path relativeScene;

    if(!scene.empty())
    {
        // The same containment and document validation as File > Open Scene.
        std::filesystem::path resolved;
        SceneDocument document;
        std::string reason;

        if(!ResolveEditorSceneLocation(scene, true, resolved, reason) ||
            !LoadSceneDocument(resolved, document, &reason))
        {
            message = "Cannot use " + PathText(scene) + " as the game startup scene: " + reason;
            return false;
        }

        std::error_code rootError;
        const auto root = GetActiveProject().ResolveCanonicalRoot(rootError);
        relativeScene = resolved.lexically_relative(root);

        if(rootError || !IsContainedRelativePath(relativeScene))
        {
            message = "The game startup scene must be inside the project: " + PathText(resolved);
            return false;
        }
    }

    auto updated = manifest->WithStartupScene(relativeScene, message);

    if(!updated)
    {
        return false;
    }

    if(updated->GetStartupScene() == manifest->GetStartupScene())
    {
        message = "The game startup scene is unchanged.";
        return true;
    }

    // Prove the exact bytes to be written pass the strict loader rules before touching the file.
    const auto text = updated->ToUpdateText();
    std::string parseError;
    auto written = ProjectManifest::Parse(text, parseError);

    if(!written || written->GetProjectID() != manifest->GetProjectID() ||
        written->GetStartupScene() != updated->GetStartupScene())
    {
        message = "The updated project manifest failed validation and was not written: " + parseError;
        return false;
    }

    const auto publication = m_projectLock.ReplaceManifest(text, message);

    if(publication == ProjectLock::Publication::NotPublished)
    {
        message = "The game startup scene was not changed. " + message;
        Log::Warning(message);

        // The original manifest is intact, but its pin may not have been restored.
        if(!m_projectLock.IsFullyOwned())
        {
            FailClosedProjectOwnership(message);
        }

        return false;
    }

    // Disk changed (also when unprotected): memory must match it. The lock is still held, on the
    // replacement manifest.
    if(!m_activeProject->UpdateManifest(std::move(*written)))
    {
        message = "The project manifest was written but the in-memory project could not be updated.";
        FailClosedProjectOwnership(message);
        return false;
    }

    if(publication == ProjectLock::Publication::PublishedUnprotected)
    {
        message = "The game startup scene was saved, but project ownership could not be re-verified. " + message;
        FailClosedProjectOwnership(message);
        return false;
    }

    message = relativeScene.empty() ?
        "Game startup scene cleared; Build Game uses the active saved scene." :
        "Game startup scene set to " + PathText(relativeScene) + ".";
    Log::Info(message);
    return true;
}

void Engine::FailClosedProjectOwnership(const std::string& reason)
{
    m_projectOwnershipLost = true;
    Log::Error("Project ownership could not be verified; project changes are disabled: " + reason);
    m_editor.SetProjectOwnershipLost(reason);
}

void Engine::ProcessProjectOwnership()
{
    if(!m_projectOwnershipLost)
    {
        return;
    }

    switch(m_editor.TakeOwnershipAction())
    {
    case Editor::OwnershipAction::Retry:
    {
        std::string error;

        if(m_projectLock.RestoreOwnership(error))
        {
            m_projectOwnershipLost = false;
            m_editor.SetProjectOwnershipLost({});
            Log::Info("Project ownership re-verified; project changes are enabled again.");
        }
        else
        {
            m_editor.SetProjectOwnershipLost("Ownership is still unverified: " + error);
        }

        break;
    }
    case Editor::OwnershipAction::Close:
        // Controlled shutdown; the lock is released last, and project writes are skipped.
        m_window.Close();
        break;
    case Editor::OwnershipAction::None:
        break;
    }
}
