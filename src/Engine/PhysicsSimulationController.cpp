#include "Engine/PhysicsSimulationController.h"
#include "Scene/MovingSolidParticipants.h"
#include "Scene/SceneDocumentAdapter.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <algorithm>
#include <utility>
#include <type_traits>
#include <vector>

namespace
{
// Diagnostics: milliseconds since `start`; measures the latest Update/Publish stages.
double ElapsedMilliseconds(std::chrono::steady_clock::time_point start) noexcept
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// Records the whole call's duration however it returns.
struct TimedScope
{
    double& target;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    ~TimedScope()
    {
        target = ElapsedMilliseconds(start);
    }
};
}

PhysicsSimulationController::PhysicsSimulationController(std::size_t maximumStepsPerUpdate)
    : m_maximumStepsPerUpdate(maximumStepsPerUpdate)
{
    if (maximumStepsPerUpdate == 0 || maximumStepsPerUpdate > 1024)
    {
        throw std::invalid_argument("Physics update step limit must be between 1 and 1024.");
    }
}

bool PhysicsSimulationController::IsOwningThread() const noexcept
{
    return std::this_thread::get_id() == m_ownerThread;
}

const Physics::RuntimePoseSnapshot* PhysicsSimulationController::GetLatestSnapshot() const noexcept
{
    return m_snapshot ? &*m_snapshot : nullptr;
}

PhysicsSimulationController::Result PhysicsSimulationController::Start(FileSceneRuntime& runtime, Renderer& renderer)
{
    if (!IsOwningThread() || m_state != State::Inactive)
    {
        return {Status::Failed, 0, "Start requires the owning thread and an inactive controller."};
    }
    if (runtime.GetScene().HasPhysicsPresentation())
    {
        return {Status::Failed, 0, "Clear the existing presentation before starting another simulation owner."};
    }
    try
    {
        std::string error;
        SceneDocument document;
        if (!CaptureSceneDocument(runtime.GetFileScene().GetDocument(), runtime.GetScene(),
            runtime.GetObjectMapping(), document, &error))
        {
            return {Status::Failed, 0, error};
        }
        auto association = runtime.AssociatePhysicsPresentation(renderer, document, error);
        if (!association)
        {
            return {Status::Failed, 0, error};
        }
        auto session = Physics::ScenePhysicsSession::Create(document, &error);
        if (!session)
        {
            return {Status::Failed, 0, error};
        }
        auto snapshot = session->CapturePoses(&error);
        if (!snapshot)
        {
            return {Status::Failed, 0, error};
        }

        // Prepare owned values before publishing. The exact same document creates
        // the world and authorizes presentation; no external snapshot is accepted.
        std::optional<SceneDocument> registeredDocument(std::move(document));
        Physics::CharacterDiagnosticsFrame initialFrame;
        initialFrame.sessionGeneration = m_sessionGeneration + 1;
        initialFrame.completedSteps = 0;
        initialFrame.controllers = session->CaptureCharacterDiagnostics();
        std::optional<Physics::CharacterDiagnosticsFrame> initialDiagnostics(std::move(initialFrame));
        std::string movingSolidError;
        auto initialMovingSolids = session->CaptureMovingSolids(m_sessionGeneration + 1, &movingSolidError);

        if(!initialMovingSolids)
        {
            return {Status::Failed, 0, movingSolidError};
        }

        auto publishedMovingSolids = std::make_shared<const Physics::MovingSolidSnapshot>(std::move(*initialMovingSolids));
        PhysicsPresentationLease lease;
        static_assert(std::is_nothrow_swappable_v<decltype(m_registeredDocument)>);
        static_assert(std::is_nothrow_swappable_v<decltype(m_association)>);
        static_assert(std::is_nothrow_swappable_v<decltype(m_snapshot)>);
        static_assert(std::is_nothrow_swappable_v<decltype(m_publishedCharacterDiagnostics)>);
        if (!runtime.ApplyPhysicsPresentation(renderer, *association, *snapshot, error, &lease))
        {
            return {Status::Failed, 0, error};
        }
        m_session = std::move(session);
        m_registeredDocument.swap(registeredDocument);
        m_association.swap(association);
        m_snapshot.swap(snapshot);
        m_lease = lease;
        ++m_sessionGeneration;
        m_pendingCharacterDiagnostics.reset();
        // Initial diagnostics describe the new session at step zero. Their allocations
        // precede publication, so a capture failure cannot leave a partially started owner.
        m_publishedCharacterDiagnostics.swap(initialDiagnostics);
        m_pendingMovingSolids.reset();
        m_publishedMovingSolids = std::move(publishedMovingSolids);
        m_accumulatedTicks = 0.0;
        m_totalSteps = 0;
        m_lastUpdateSteps = 0;
        m_lastUpdateContactEvents.clear();
        m_lastUpdateContactNotifications.clear();
        m_lastUpdateTriggerEvents.clear();
        m_pendingPresentation = false;
        m_state = State::Active;
        return {Status::Started};
    }
    catch (const std::exception& exception)
    {
        return {Status::Failed, 0, exception.what()};
    }
}

bool PhysicsSimulationController::ValidateAuthoredPhysicsTransforms(FileSceneRuntime& runtime, std::string& error) const
{
    SceneDocument live;
    if (!CaptureSceneDocument(runtime.GetFileScene().GetDocument(), runtime.GetScene(),
        runtime.GetObjectMapping(), live, &error))
    {
        return false;
    }
    const auto sameVector = [](const Vector3& left, const Vector3& right)
    {
        return left.x == right.x && left.y == right.y && left.z == right.z;
    };
    for (std::size_t index = 0; index < live.objects.size(); ++index)
    {
        const auto& registered = m_registeredDocument->objects[index];
        const auto& current = live.objects[index];

        if((registered.physics || registered.triggerCollider || registered.characterController) &&
            (!sameVector(registered.transform.position, current.transform.position) ||
             !sameVector(registered.transform.rotation, current.transform.rotation)))
        {
            error = "Authored physics position or rotation changed; start a new simulation session.";
            return false;
        }
    }
    return true;
}

PhysicsSimulationController::Result PhysicsSimulationController::Update(
    FileSceneRuntime& runtime, Renderer& renderer, double elapsedSeconds)
{
    if (!IsOwningThread() || m_state != State::Active)
    {
        return {Status::Failed, 0, "Update requires the owning thread and an active simulation."};
    }
    if (!std::isfinite(elapsedSeconds) || elapsedSeconds < 0.0 ||
        elapsedSeconds > MaximumAccumulatedSeconds ||
        m_accumulatedTicks + elapsedSeconds * 60.0 > MaximumAccumulatedSeconds * 60.0)
    {
        return {Status::InvalidInput, 0, "Elapsed time must be finite, nonnegative and keep accumulated debt within one day."};
    }
    m_lastFrameTiming.updateReadiness = 0.0;
    m_lastFrameTiming.updateSteps = 0.0;
    m_lastFrameTiming.updateCapture = 0.0;
    TimedScope updateScope{m_lastFrameTiming.update};
    try
    {
        std::string error;
        const auto readinessStart = std::chrono::steady_clock::now();
        const auto readiness = runtime.CheckPhysicsPresentation(renderer, *m_association, *m_lease, error);
        if (readiness == PhysicsPresentationReadiness::Stale || readiness == PhysicsPresentationReadiness::Failed)
        {
            return FailAndStop(runtime, renderer, error);
        }
        if (!ValidateAuthoredPhysicsTransforms(runtime, error))
        {
            return FailAndStop(runtime, renderer, error);
        }
        m_lastFrameTiming.updateReadiness = ElapsedMilliseconds(readinessStart);
        m_lastUpdateSteps = 0;
        m_lastUpdateContactEvents.clear();
        m_lastUpdateContactNotifications.clear();
        m_lastUpdateTriggerEvents.clear();

        if (elapsedSeconds == 0.0)
        {
            return {Status::Updated};
        }
        if (m_totalSteps > (std::numeric_limits<std::uint64_t>::max)() - m_maximumStepsPerUpdate)
        {
            return FailAndStop(runtime, renderer, "Simulated step counter is exhausted.");
        }

        // Due ticks use the shared tolerance and cap (ConsumeDueTicks); catch-up debt stays.
        m_accumulatedTicks += elapsedSeconds * 60.0;
        const std::size_t dueSteps = ConsumeDueTicks(m_accumulatedTicks, m_maximumStepsPerUpdate);
        const auto stepsStart = std::chrono::steady_clock::now();
        while (m_lastUpdateSteps < dueSteps)
        {
            m_session->Step();
            ++m_lastUpdateSteps;
            ++m_totalSteps;
        }
        m_lastFrameTiming.updateSteps = ElapsedMilliseconds(stepsStart);

        if (m_lastUpdateSteps != 0)
        {
            const auto captureStart = std::chrono::steady_clock::now();
            // Every step's events, each tagged with its own fixed step, after all steps finished.
            m_lastUpdateContactEvents = m_session->TakeContactEvents();
            m_lastUpdateContactNotifications = m_session->SelectContactNotifications(m_lastUpdateContactEvents);
            m_lastUpdateTriggerEvents = m_session->TakeTriggerEvents();

            auto snapshot = m_session->CapturePoses(&error);
            if (!snapshot)
            {
                return FailAndStop(runtime, renderer, error);
            }
            m_snapshot.swap(snapshot);
            m_pendingCharacterDiagnostics = CaptureCharacterDiagnostics();
            m_pendingMovingSolids = CaptureMovingSolids(m_sessionGeneration);
            m_pendingPresentation = true;
            m_lastFrameTiming.updateCapture = ElapsedMilliseconds(captureStart);
        }
        return {Status::Updated, m_lastUpdateSteps};
    }
    catch (const std::exception& exception)
    {
        return FailAndStop(runtime, renderer, exception.what());
    }
}

PhysicsSimulationController::Result PhysicsSimulationController::PublishAtBoundary(FileSceneRuntime& runtime, Renderer& renderer)
{
    if (!IsOwningThread() || m_state != State::Active)
    {
        return {Status::Failed, 0, "Publication requires the owning thread and an active simulation."};
    }
    m_lastFrameTiming.publishApply = 0.0;
    TimedScope publishScope{m_lastFrameTiming.publish};
    std::string error;
    const auto readiness = runtime.CheckPhysicsPresentation(renderer, *m_association, *m_lease, error);
    if (readiness == PhysicsPresentationReadiness::Retry)
    {
        return {Status::Pending, 0, error};
    }
    if (readiness != PhysicsPresentationReadiness::Ready)
    {
        return FailAndStop(runtime, renderer, error);
    }
    try
    {
        if (!ValidateAuthoredPhysicsTransforms(runtime, error))
        {
            return FailAndStop(runtime, renderer, error);
        }
        if (m_pendingPresentation)
        {
            PhysicsPresentationLease acceptedLease;
            const auto applyStart = std::chrono::steady_clock::now();
            if (!runtime.ApplyPhysicsPresentation(renderer, *m_association, *m_snapshot, error, &acceptedLease))
            {
                return FailAndStop(runtime, renderer, error);
            }
            m_lastFrameTiming.publishApply = ElapsedMilliseconds(applyStart);
            m_lease = acceptedLease;
            m_pendingPresentation = false;

            // Diagnostics become visible exactly when the poses they describe are presented.
            if (m_pendingCharacterDiagnostics)
            {
                m_publishedCharacterDiagnostics.swap(m_pendingCharacterDiagnostics);
                m_pendingCharacterDiagnostics.reset();
            }

            if(m_pendingMovingSolids)
            {
                m_publishedMovingSolids = std::move(m_pendingMovingSolids);
            }
        }
        return {Status::Published};
    }
    catch (const std::exception& exception)
    {
        return FailAndStop(runtime, renderer, exception.what());
    }
}

bool PhysicsSimulationController::SubmitCharacterMovement(std::uint64_t sessionGeneration, std::string_view objectID,
    std::uint64_t fixedStep, Vector3 desiredHorizontalVelocity, std::string& error)
{
    if(!IsOwningThread() || m_state != State::Active || !m_session)
    {
        error = "Character movement requires the owning thread and an active simulation.";
        return false;
    }

    if(sessionGeneration == 0 || sessionGeneration != m_sessionGeneration)
    {
        error = "Character movement belongs to a stopped or replaced simulation session.";
        return false;
    }

    return m_session->SubmitCharacterMovement(objectID, fixedStep, desiredHorizontalVelocity, &error);
}

bool PhysicsSimulationController::SubmitCharacterJump(std::uint64_t sessionGeneration, std::string_view objectID,
    std::uint64_t fixedStep, std::string& error)
{
    if(!IsOwningThread() || m_state != State::Active || !m_session)
    {
        error = "Character jump requires the owning thread and an active simulation.";
        return false;
    }

    if(sessionGeneration == 0 || sessionGeneration != m_sessionGeneration)
    {
        error = "Character jump belongs to a stopped or replaced simulation session.";
        return false;
    }

    return m_session->SubmitCharacterJump(objectID, fixedStep, &error);
}

bool PhysicsSimulationController::SetCharacterYaw(std::uint64_t sessionGeneration, std::string_view objectID, float yaw,
    std::string& error)
{
    if(!IsOwningThread() || m_state != State::Active || !m_session)
    {
        error = "Character yaw requires the owning thread and an active simulation.";
        return false;
    }

    if(sessionGeneration == 0 || sessionGeneration != m_sessionGeneration)
    {
        error = "Character yaw belongs to a stopped or replaced simulation session.";
        return false;
    }

    if(!m_session->SetCharacterYaw(objectID, yaw, &error))
    {
        return false;
    }

    // The rotated pose is presented at the next publication boundary, with or without a step.
    return CaptureForNextPublication(error);
}

std::optional<float> PhysicsSimulationController::GetCharacterYaw(std::string_view objectID) const
{
    return m_state == State::Active && m_session ? m_session->GetCharacterYaw(objectID) : std::nullopt;
}

std::size_t PhysicsSimulationController::ConsumeDueTicks(double& accumulatedTicks, std::size_t maximumSteps) noexcept
{
    // Tick units make common 60/120/30 Hz inputs exactly integral/half-integral.
    // A tiny boundary tolerance absorbs roundoff; accepted catch-up debt stays.
    constexpr double boundaryToleranceTicks = 1.0e-9;
    std::size_t steps = 0;

    while (accumulatedTicks >= 1.0 - boundaryToleranceTicks && steps < maximumSteps)
    {
        accumulatedTicks -= 1.0;

        if (accumulatedTicks < 0.0)
        {
            accumulatedTicks = 0.0;
        }

        ++steps;
    }

    return steps;
}

std::size_t PhysicsSimulationController::PredictStepsForUpdate(double elapsedSeconds) const noexcept
{
    // Mirrors Update's admission: inactive, invalid or zero elapsed time runs no step.
    if (!IsOwningThread() || m_state != State::Active || !std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0 ||
        elapsedSeconds > MaximumAccumulatedSeconds ||
        m_accumulatedTicks + elapsedSeconds * 60.0 > MaximumAccumulatedSeconds * 60.0)
    {
        return 0;
    }

    double accumulatedTicks = m_accumulatedTicks + elapsedSeconds * 60.0;
    return ConsumeDueTicks(accumulatedTicks, m_maximumStepsPerUpdate);
}

Physics::CharacterDiagnosticsFrame PhysicsSimulationController::CaptureCharacterDiagnostics() const
{
    Physics::CharacterDiagnosticsFrame frame;
    frame.sessionGeneration = m_sessionGeneration;
    frame.completedSteps = m_totalSteps;
    frame.controllers = m_session->CaptureCharacterDiagnostics();
    return frame;
}

std::shared_ptr<const Physics::MovingSolidSnapshot> PhysicsSimulationController::CaptureMovingSolids(
    std::uint64_t sessionGeneration) const
{
    std::string error;
    auto snapshot = m_session->CaptureMovingSolids(sessionGeneration, &error);

    if(!snapshot)
    {
        throw std::runtime_error(error);
    }

    return std::make_shared<const Physics::MovingSolidSnapshot>(std::move(*snapshot));
}

bool PhysicsSimulationController::CaptureForNextPublication(std::string& error)
{
    auto snapshot = m_session->CapturePoses(&error);

    if(!snapshot)
    {
        return false;
    }

    auto movingSolids = m_session->CaptureMovingSolids(m_sessionGeneration, &error);

    if(!movingSolids)
    {
        return false;
    }

    m_snapshot.swap(snapshot);
    m_pendingCharacterDiagnostics = CaptureCharacterDiagnostics();
    m_pendingMovingSolids = std::make_shared<const Physics::MovingSolidSnapshot>(std::move(*movingSolids));
    m_pendingPresentation = true;
    return true;
}

bool PhysicsSimulationController::CheckCommandSession(std::uint64_t sessionGeneration, const char* operation,
    std::string& error) const
{
    if(!IsOwningThread() || m_state != State::Active || !m_session)
    {
        error = std::string(operation) + " requires the owning thread and an active simulation.";
        return false;
    }

    if(sessionGeneration == 0 || sessionGeneration != m_sessionGeneration)
    {
        error = std::string(operation) + " belongs to a stopped or replaced simulation session.";
        return false;
    }

    return true;
}

bool PhysicsSimulationController::SubmitKinematicTarget(std::uint64_t sessionGeneration, std::string_view objectID,
    std::uint64_t fixedStep, const Physics::Pose& target, std::string& error)
{
    if(!CheckCommandSession(sessionGeneration, "Kinematic motion", error))
    {
        return false;
    }

    return m_session->SubmitKinematicTarget(objectID, fixedStep, target, &error);
}

bool PhysicsSimulationController::TeleportKinematicBody(std::uint64_t sessionGeneration, std::string_view objectID,
    const Physics::Pose& pose, std::string& error)
{
    if(!CheckCommandSession(sessionGeneration, "Kinematic teleport", error))
    {
        return false;
    }

    if(!m_session->TeleportKinematicBody(objectID, pose, &error))
    {
        return false;
    }

    return CaptureForNextPublication(error);
}

bool PhysicsSimulationController::SetBodyLinearVelocity(std::uint64_t sessionGeneration, std::string_view objectID,
    const Vector3& velocity, std::string& error)
{
    if(!CheckCommandSession(sessionGeneration, "Rigid-body velocity", error))
    {
        return false;
    }

    return m_session->SetBodyLinearVelocity(objectID, velocity, &error);
}

std::optional<Vector3> PhysicsSimulationController::GetBodyLinearVelocity(std::uint64_t sessionGeneration,
    std::string_view objectID) const
{
    if(!IsOwningThread() || m_state != State::Active || !m_session || sessionGeneration != m_sessionGeneration)
    {
        return std::nullopt;
    }

    return m_session->GetBodyLinearVelocity(objectID);
}

bool PhysicsSimulationController::AreObjectsTouching(std::uint64_t sessionGeneration, std::string_view objectA,
    std::string_view objectB) const
{
    return IsOwningThread() && m_state == State::Active && m_session && sessionGeneration == m_sessionGeneration &&
        m_session->AreObjectsTouching(objectA, objectB);
}

bool PhysicsSimulationController::SetAllMovableBodiesParticipating(std::uint64_t sessionGeneration, std::string& error)
{
    if(!CheckCommandSession(sessionGeneration, "Moving-solid participation", error))
    {
        return false;
    }

    for(const std::string& objectID : m_session->GetMovableBodyIDs())
    {
        if(!m_session->SetMovingSolidParticipation(objectID, true, &error))
        {
            return false;
        }
    }

    return CaptureForNextPublication(error);
}

bool PhysicsSimulationController::ApplyMovingSolidParticipationPolicy(std::uint64_t sessionGeneration,
    std::vector<std::string>& participants, std::string& error)
{
    participants.clear();

    if(!CheckCommandSession(sessionGeneration, "Moving-solid participation", error))
    {
        return false;
    }

    if(!m_registeredDocument)
    {
        error = "Moving-solid participation requires the session's registered scene.";
        return false;
    }

    // The shared rule, asked with the feature on (the caller applies it only while the scene's moving solids are enabled), on the
    // document this session registered. Every admitted body must be one of the session's movable bodies: the rule never admits a
    // Static body, and every Dynamic or Kinematic PhysicsComponent is registered.
    AirSimulationSettings enabled;
    enabled.enabled = true;
    enabled.movingSolids = true;
    const std::vector<std::string> admitted = ResolveMovingSolidParticipants(*m_registeredDocument, enabled);
    const std::vector<std::string> movable = m_session->GetMovableBodyIDs();

    for(const std::string& objectID : admitted)
    {
        if(!std::binary_search(movable.begin(), movable.end(), objectID))
        {
            error = "Moving-solid participant " + objectID + " is not a movable body of the session.";
            return false;
        }

        if(!m_session->SetMovingSolidParticipation(objectID, true, &error))
        {
            return false;
        }

        participants.push_back(objectID);
    }

    return CaptureForNextPublication(error);
}

bool PhysicsSimulationController::SetMovingSolidParticipation(std::uint64_t sessionGeneration, std::string_view objectID,
    bool participates, std::string& error)
{
    if(!CheckCommandSession(sessionGeneration, "Moving-solid participation", error))
    {
        return false;
    }

    if(!m_session->SetMovingSolidParticipation(objectID, participates, &error))
    {
        return false;
    }

    return CaptureForNextPublication(error);
}

void PhysicsSimulationController::ReleaseSimulation() noexcept
{
    m_pendingCharacterDiagnostics.reset();
    m_publishedCharacterDiagnostics.reset();
    m_pendingMovingSolids.reset();
    m_publishedMovingSolids.reset();
    m_session.reset();
    m_snapshot.reset();
    m_lastUpdateContactEvents.clear();
    m_lastUpdateContactNotifications.clear();
    m_lastUpdateTriggerEvents.clear();
    m_registeredDocument.reset();
    m_association.reset();
    m_pendingPresentation = false;
    m_accumulatedTicks = 0.0;
    m_lastUpdateSteps = 0;
}

PhysicsSimulationController::Result PhysicsSimulationController::Stop(FileSceneRuntime& runtime, Renderer& renderer)
{
    if (!IsOwningThread())
    {
        return {Status::Failed, 0, "Stop requires the owning thread."};
    }
    ReleaseSimulation();
    if (!m_lease)
    {
        m_state = State::Inactive;
        return {Status::Stopped};
    }
    m_state = State::Stopping;
    std::string error;
    const auto readiness = runtime.ClearOwnedPhysicsPresentation(renderer, *m_lease, error);
    if (readiness == PhysicsPresentationReadiness::Retry)
    {
        return {Status::Pending, 0, error};
    }
    if (readiness == PhysicsPresentationReadiness::Failed)
    {
        return {Status::Failed, 0, error};
    }
    m_lease.reset();
    m_state = State::Inactive;
    return {readiness == PhysicsPresentationReadiness::Ready ? Status::Stopped : Status::Superseded, 0, error};
}

PhysicsSimulationController::Result PhysicsSimulationController::FailAndStop(
    FileSceneRuntime& runtime, Renderer& renderer, std::string message)
{
    const auto cleanup = Stop(runtime, renderer);
    if (m_state == State::Stopping) message += " Cleanup requires another Stop at a safe boundary: " + cleanup.message;
    return {Status::Failed, 0, std::move(message)};
}
