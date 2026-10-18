#pragma once

#include "Physics/ScenePhysicsSession.h"
#include "Scene/FileSceneRuntime.h"

#include <cstdint>
#include <optional>
#include <thread>

// Engine orchestration only. EnginePhysics never depends on Scene or Renderer.
class PhysicsSimulationController
{
public:
    enum class State { Inactive, Active, Stopping };
    enum class Status { Started, Updated, Published, Stopped, Pending, InvalidInput, Failed, Superseded };

    struct Result
    {
        Status status;
        std::size_t stepsExecuted = 0;
        std::string message;
    };

    explicit PhysicsSimulationController(std::size_t maximumStepsPerUpdate = 8);
    // Call Stop with the live context before destruction. Destruction releases CPU
    // physics only; Scene owns copied presentation values, never dangling bodies.
    ~PhysicsSimulationController() = default;
    PhysicsSimulationController(const PhysicsSimulationController&) = delete;
    PhysicsSimulationController& operator=(const PhysicsSimulationController&) = delete;
    PhysicsSimulationController(PhysicsSimulationController&&) = delete;
    PhysicsSimulationController& operator=(PhysicsSimulationController&&) = delete;

    Result Start(FileSceneRuntime& runtime, Renderer& renderer);
    // CPU simulation/capture only. Publication is an explicit second phase.
    Result Update(FileSceneRuntime& runtime, Renderer& renderer, double elapsedSeconds);
    Result PublishAtBoundary(FileSceneRuntime& runtime, Renderer& renderer);
    // Releases bodies immediately, even if clearing must retry at an idle boundary.
    Result Stop(FileSceneRuntime& runtime, Renderer& renderer);

    State GetState() const noexcept { return m_state; }
    bool HasPendingPresentation() const noexcept { return m_pendingPresentation; }
    std::uint64_t GetTotalSimulatedSteps() const noexcept { return m_totalSteps; }
    std::size_t GetLastUpdateSteps() const noexcept { return m_lastUpdateSteps; }
    double GetAccumulatedSeconds() const noexcept { return m_accumulatedTicks / 60.0; }
    const Physics::RuntimePoseSnapshot* GetLatestSnapshot() const noexcept;

    // Identifies the active session; 0 while inactive. Each successful Start uses a new
    // value, so commands prepared for an earlier session can never reach its replacement.
    std::uint64_t GetSessionGeneration() const noexcept { return m_session ? m_sessionGeneration : 0; }
    // Fixed step number that the next simulated tick will run (completed steps + 1).
    std::uint64_t GetNextFixedStep() const noexcept { return m_totalSteps + 1; }
    // Forwards one Character Controller movement command (see ScenePhysicsSession::
    // SubmitCharacterMovement) to the active session. Requires the owning thread, an
    // Active state and the current session generation. Commands are input-device and
    // frame-rate independent: each names the fixed step it applies to.
    bool SubmitCharacterMovement(std::uint64_t sessionGeneration, std::string_view objectID,
        std::uint64_t fixedStep, Vector3 desiredHorizontalVelocity, std::string& error);
    // Forwards one jump request (see ScenePhysicsSession::SubmitCharacterJump) with the same
    // thread, state and session-generation checks as movement.
    bool SubmitCharacterJump(std::uint64_t sessionGeneration, std::string_view objectID, std::uint64_t fixedStep,
        std::string& error);

    // Number of fixed steps the next Update(elapsedSeconds) will run, from the same due-tick
    // arithmetic and per-Update cap; 0 when inactive or for input Update would reject. Lets a
    // caller address exactly the steps a frame will execute (CM.9 diagnostic drive).
    std::size_t PredictStepsForUpdate(double elapsedSeconds) const noexcept;

    // Controller diagnostics captured with the currently published presentation (never a
    // newer, unpublished step). nullopt unless Active; cleared by Stop, failure and Start.
    const Physics::CharacterDiagnosticsFrame* GetPublishedCharacterDiagnostics() const noexcept
    {
        return m_state == State::Active && m_publishedCharacterDiagnostics ? &*m_publishedCharacterDiagnostics : nullptr;
    }

    // Contact events from the fixed steps run by the most recent Update, in
    // ContactEventPrecedes order with per-event step numbers. Empty after an Update that
    // ran no step, and after Start, Stop or failure. Owned by the controller; the
    // reference is valid until the next Start, Update or Stop.
    const std::vector<Physics::ContactEvent>& GetLastUpdateContactEvents() const noexcept
    {
        return m_lastUpdateContactEvents;
    }

    // Gameplay-facing subset of GetLastUpdateContactEvents: events in which at least one
    // participant has generateContactEvents, with per-participant opt-in flags. Same
    // order, step numbers, clearing and lifetime as the raw events.
    const std::vector<Physics::ContactNotification>& GetLastUpdateContactNotifications() const noexcept
    {
        return m_lastUpdateContactNotifications;
    }

    // Directed events from every fixed step of the latest accepted Update, ordered by
    // TriggerEventPrecedes. Empty after accepted zero-step updates, Start, Stop or failure.
    // InvalidInput leaves the previous accepted batch intact, matching contact retrieval.
    // Copy to retain values beyond the next accepted Update or lifecycle operation.
    const std::vector<Physics::TriggerEvent>& GetLastUpdateTriggerEvents() const noexcept
    {
        return m_lastUpdateTriggerEvents;
    }

    static constexpr double FixedStepSeconds = 1.0 / 60.0;
    // Inputs/debt beyond one day are rejected, never silently truncated.
    static constexpr double MaximumAccumulatedSeconds = 86400.0;

private:
    bool IsOwningThread() const noexcept;
    bool ValidateAuthoredPhysicsTransforms(FileSceneRuntime& runtime, std::string& error) const;
    Result FailAndStop(FileSceneRuntime& runtime, Renderer& renderer, std::string message);
    void ReleaseSimulation() noexcept;
    // Consumes due ticks from accumulatedTicks, at most maximumSteps; shared by Update and
    // PredictStepsForUpdate so prediction can never diverge from execution.
    static std::size_t ConsumeDueTicks(double& accumulatedTicks, std::size_t maximumSteps) noexcept;
    Physics::CharacterDiagnosticsFrame CaptureCharacterDiagnostics() const;

    const std::thread::id m_ownerThread = std::this_thread::get_id();
    const std::size_t m_maximumStepsPerUpdate;
    State m_state = State::Inactive;
    std::unique_ptr<Physics::ScenePhysicsSession> m_session;
    std::optional<SceneDocument> m_registeredDocument;
    std::optional<PhysicsPresentationAssociation> m_association;
    std::optional<PhysicsPresentationLease> m_lease;
    std::optional<Physics::RuntimePoseSnapshot> m_snapshot;
    bool m_pendingPresentation = false;
    double m_accumulatedTicks = 0.0;
    std::uint64_t m_totalSteps = 0;
    std::uint64_t m_sessionGeneration = 0;
    std::size_t m_lastUpdateSteps = 0;
    std::vector<Physics::ContactEvent> m_lastUpdateContactEvents;
    std::vector<Physics::ContactNotification> m_lastUpdateContactNotifications;
    std::vector<Physics::TriggerEvent> m_lastUpdateTriggerEvents;
    // Captured with each snapshot; promoted to published only when that snapshot publishes.
    std::optional<Physics::CharacterDiagnosticsFrame> m_pendingCharacterDiagnostics;
    std::optional<Physics::CharacterDiagnosticsFrame> m_publishedCharacterDiagnostics;
};
