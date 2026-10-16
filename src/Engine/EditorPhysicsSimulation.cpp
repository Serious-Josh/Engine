#include "Engine/Engine.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Core/Log.h"

bool Engine::PhysicsBlocksSceneEdits() const noexcept
{
    return m_physicsSimulation.GetState() != PhysicsSimulationController::State::Inactive;
}

bool Engine::CanStartEditorPhysics() const
{
    return m_startup.mode == EngineStartup::Mode::Editor && !PhysicsBlocksSceneEdits() &&
        !m_pendingEditorMembership && !m_pendingSceneObjectCreation && !m_pendingSceneObjectRestoration &&
        !m_pendingSceneObjectDeletion && !m_pendingMaterialAssignment && !m_pendingViewportMaterialBinding &&
        !m_pendingEditorSceneOperation && !m_demoSceneManager.HasPendingSceneChange() && m_editor.CanStartPhysics();
}

void Engine::RefreshEditorPhysicsStatus(bool compatibleScene)
{
    using State = PhysicsSimulationController::State;
    const auto state = m_physicsSimulation.GetState();
    const auto editorState = state == State::Active ? Editor::PhysicsState::Playing :
        state == State::Stopping ? Editor::PhysicsState::Stopping : Editor::PhysicsState::Stopped;
    m_editor.SetPhysicsStatus(editorState, compatibleScene && CanStartEditorPhysics(), m_physicsMessage);
}

bool Engine::StopEditorPhysicsSimulation(FileSceneRuntime& runtime)
{
    const auto result = m_physicsSimulation.Stop(runtime, m_renderer);
    if (result.status == PhysicsSimulationController::Status::Failed && result.message != m_physicsMessage)
    {
        m_physicsMessage = result.message;
        Log::Warning("Physics cleanup: " + result.message);
    }
    return m_physicsSimulation.GetState() == PhysicsSimulationController::State::Inactive;
}

void Engine::ProcessEditorPhysicsSimulation(FileSceneRuntime* runtime, double elapsedSeconds, bool compatibleScene)
{
    using State = PhysicsSimulationController::State;
    using Status = PhysicsSimulationController::Status;
    const auto request = m_editor.TakePhysicsRequest();
    if (!runtime)
    {
        // All supported scene replacements retire the controller before destroying
        // the old runtime. An unexpected loss of that owner cannot authorize rebind.
        if (PhysicsBlocksSceneEdits())
        {
            m_physicsMessage = "Physics scene ownership was lost; shutting down safely.";
            Log::Error(m_physicsMessage);
            m_window.Close();
        }
        RefreshEditorPhysicsStatus(false);
        return;
    }
    const bool leavingScene = m_pendingEditorSceneOperation.has_value() || m_demoSceneManager.HasPendingSceneChange();
    if (request == Editor::PhysicsRequest::Stop || leavingScene ||
        m_physicsSimulation.GetState() == State::Stopping || m_window.ShouldClose())
    {
        if (request == Editor::PhysicsRequest::Stop)
        {
            m_physicsMessage.clear();
        }
        if (PhysicsBlocksSceneEdits())
        {
            StopEditorPhysicsSimulation(*runtime);
        }
    }
    else if (request == Editor::PhysicsRequest::Play)
    {
        m_physicsMessage.clear();
        if (!compatibleScene || !CanStartEditorPhysics())
        {
            m_physicsMessage = !compatibleScene ? "Physics Play requires an editable file-backed scene." :
                "Finish the current editor operation before starting physics.";
        }
        else
        {
            const auto result = m_physicsSimulation.Start(*runtime, m_renderer);
            if (result.status != Status::Started)
            {
                m_physicsMessage = "Cannot play: " + result.message;
                Log::Warning(m_physicsMessage);
            }
        }
        // Initial presentation is rendered before the first elapsed-time update.
    }
    else if (m_physicsSimulation.GetState() == State::Active)
    {
        if (m_renderer.IsCommandRecordingActive())
        {
            m_physicsMessage = "Physics stopped: the renderer is still recording commands.";
            StopEditorPhysicsSimulation(*runtime);
            RefreshEditorPhysicsStatus(compatibleScene);
            return;
        }
        // This seam is called once per editor frame, outside all command recording.
        // A publication retry never reuses this frame's elapsed time.
        const auto update = m_physicsSimulation.Update(*runtime, m_renderer, elapsedSeconds);
        if (update.status != Status::Updated)
        {
            m_physicsMessage = "Physics stopped: " + update.message;
            Log::Warning(m_physicsMessage);
            if (PhysicsBlocksSceneEdits())
            {
                StopEditorPhysicsSimulation(*runtime);
            }
        }
        else
        {
            const auto publication = m_physicsSimulation.PublishAtBoundary(*runtime, m_renderer);
            if (publication.status == Status::Failed)
            {
                m_physicsMessage = "Physics stopped: " + publication.message;
                Log::Warning(m_physicsMessage);
            }
            // Pending retains the latest completed snapshot; history remains owned
            // by the existing successful-render path in Engine::Frame.
        }
    }
    RefreshEditorPhysicsStatus(compatibleScene);
}
