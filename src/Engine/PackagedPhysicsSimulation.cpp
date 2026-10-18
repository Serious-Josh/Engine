#include "Engine/Engine.h"

bool Engine::StartPackagedPhysics(FileSceneRuntime& runtime, std::string& error)
{
    if(m_startup.mode != EngineStartup::Mode::PackagedRuntime || !runtime.IsArchiveActivation())
    {
        error = "Packaged physics requires an archive-only runtime scene.";
        return false;
    }

    const auto started = m_physicsSimulation.Start(runtime, m_renderer);

    if(started.status != PhysicsSimulationController::Status::Started)
    {
        error = started.message;
        return false;
    }

    m_packagedPhysicsRuntimeIdentity = runtime.GetRuntimeIdentity();
    error.clear();
    return true;
}

bool Engine::AdvancePackagedPhysics(FileSceneRuntime& runtime, double elapsedSeconds, std::string& error)
{
    if(m_startup.mode != EngineStartup::Mode::PackagedRuntime || !runtime.IsArchiveActivation() ||
        runtime.GetRuntimeIdentity() != m_packagedPhysicsRuntimeIdentity || m_renderer.IsCommandRecordingActive())
    {
        error = "Packaged physics lost its archive scene or idle renderer boundary.";
        return false;
    }

    // The shared controller owns the only clock and command consumption order.
    // A deferred publication never causes the elapsed interval to be submitted twice.
    const auto updated = m_physicsSimulation.Update(runtime, m_renderer, elapsedSeconds);

    if(updated.status != PhysicsSimulationController::Status::Updated)
    {
        error = updated.message;
        return false;
    }

    const auto published = m_physicsSimulation.PublishAtBoundary(runtime, m_renderer);

    if(published.status != PhysicsSimulationController::Status::Published &&
        published.status != PhysicsSimulationController::Status::Pending)
    {
        error = published.message;
        return false;
    }

    error.clear();
    return true;
}

bool Engine::SubmitCharacterMovement(std::uint64_t sessionGeneration, std::string_view objectID,
    std::uint64_t fixedStep, Vector3 velocity, std::string& error)
{
    return m_physicsSimulation.SubmitCharacterMovement(sessionGeneration, objectID, fixedStep, velocity, error);
}

bool Engine::SubmitCharacterJump(std::uint64_t sessionGeneration, std::string_view objectID,
    std::uint64_t fixedStep, std::string& error)
{
    return m_physicsSimulation.SubmitCharacterJump(sessionGeneration, objectID, fixedStep, error);
}
