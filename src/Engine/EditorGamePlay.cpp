#include "Engine/Engine.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "assets/ProjectScriptAssets.h"
#include "Core/Log.h"

#include <cmath>

bool Engine::IsEditorGameOwningSimulation() const noexcept
{
    // A Stop whose presentation cleanup is still retrying keeps the simulation in Game Play's name.
    return m_gameRuntime.IsActive() ||
        (m_gameStopping && m_physicsSimulation.GetState() != PhysicsSimulationController::State::Inactive);
}

bool Engine::IsEditorSceneStreamingSettled() const
{
    // Mesh and texture publications replace the renderer state a running simulation presents
    // into, which stops it. Like the packaged runtime, Play waits until the scene is resident.
    const auto* scene = dynamic_cast<const FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    const bool meshesSettled = !m_automaticMeshResidency.enabled || m_automaticMeshResidency.settled;
    return meshesSettled && !m_residencyRequestSelection && !m_meshResidencyPublication.HasCandidate() &&
        (!scene || scene->IsTextureStreamingSettled());
}

void Engine::RefreshEditorGameStatus(bool compatibleScene)
{
    if(m_physicsSimulation.GetState() == PhysicsSimulationController::State::Inactive)
    {
        m_gameStopping = false;
    }

    const auto state = m_gameRuntime.IsActive() ? Editor::GameState::Playing :
        IsEditorGameOwningSimulation() ? Editor::GameState::Stopping : Editor::GameState::Editing;
    m_editor.SetGameStatus(state, compatibleScene && CanStartEditorPhysics() && IsEditorSceneStreamingSettled(),
        m_gameMessage);
}

void Engine::ProcessEditorGamePlay(FileSceneRuntime* runtime, double elapsedSeconds, bool compatibleScene,
    std::uint32_t viewWidth, std::uint32_t viewHeight)
{
    const auto request = m_editor.TakeGameRequest();

    // Losing the runtime owner is handled by the shared simulation path, which shuts down safely.
    if(!runtime)
    {
        UpdateGameCursor(std::nullopt, false);
        RefreshEditorGameStatus(false);
        return;
    }

    if(m_gameRuntime.IsActive())
    {
        const bool leavingScene = m_pendingEditorSceneOperation.has_value() || m_demoSceneManager.HasPendingSceneChange();
        std::string error;

        if(request == Editor::GameRequest::Stop || leavingScene || m_window.ShouldClose())
        {
            if(request == Editor::GameRequest::Stop)
            {
                m_gameMessage.clear();
            }

            // A pending presentation clear is retried by ProcessEditorPhysicsSimulation.
            m_gameStopping = !m_gameRuntime.Stop(*runtime, m_renderer);

            if(!m_gameRuntime.GetScriptFault().empty())
            {
                m_gameMessage = m_gameRuntime.GetScriptFault();
            }
        }
        else
        {
            // The cursor lock covers only the game view image; ImGui coordinates are client pixels.
            std::optional<WindowsWindow::CursorRectangle> area;

            if(const auto image = m_editor.GetGameplayCursorArea())
            {
                area = WindowsWindow::CursorRectangle{static_cast<int>(std::ceil(image->position.x)),
                    static_cast<int>(std::ceil(image->position.y)),
                    static_cast<int>(std::floor(image->position.x + image->size.x)),
                    static_cast<int>(std::floor(image->position.y + image->size.y))};
            }

            const bool captured = UpdateGameCursor(area, m_editor.WasGameViewClicked());
            // A released lock suppresses gameplay input until the game view is clicked again.
            const bool suppressed = m_gameRuntime.WantsCursorLock() && !captured;

            if(captured)
            {
                Editor::HideGameplayCursor();
            }

            if(!m_gameRuntime.Update(*runtime, m_renderer,
                {elapsedSeconds, area && !suppressed ? &m_input : nullptr, captured}, error))
            {
                m_gameMessage = "Game Play stopped: " + error;
                Log::Warning(m_gameMessage);
                m_gameStopping = !m_gameRuntime.Stop(*runtime, m_renderer);
            }
        }
    }
    else if(request == Editor::GameRequest::Play)
    {
        m_gameMessage.clear();
        std::string error;

        if(!compatibleScene || !CanStartEditorPhysics() || !IsEditorSceneStreamingSettled())
        {
            m_gameMessage = !compatibleScene ? "Game Play requires an editable file-backed scene." :
                !CanStartEditorPhysics() ? "Finish the current editor operation before playing." :
                "Wait for the scene to finish streaming before playing.";
        }
        else
        {
            // Editor Play compiles scripts from the project's saved source files.
            const auto scripts = [this](Scripting::ScriptHost& host, const AssetID& asset, Scripting::ModuleID& module)
            {
                return ProjectScriptAssets::Build(host, GetActiveProject().GetPaths(), m_projectAssetRegistry, asset, module);
            };

            if(!m_gameRuntime.Start(*runtime, m_renderer, true, viewWidth, viewHeight, error, scripts))
            {
                m_gameMessage = "Cannot play: " + error;
                Log::Warning(m_gameMessage);
            }
            else
            {
                m_gameStopping = false;
                m_gameMessage = m_gameRuntime.GetScriptFault();
                Log::Info("Game Play started.");
            }
        }
        // As in the packaged runtime, the first elapsed interval is the next frame's.
    }

    // Stop, a failed frame or a replaced scene leaves no confined or hidden cursor behind.
    if(!m_gameRuntime.IsActive())
    {
        UpdateGameCursor(std::nullopt, false);
    }

    RefreshEditorGameStatus(compatibleScene);
}
