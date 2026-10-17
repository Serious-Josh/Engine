#include "Engine/PhysicsSimulationController.h"
#include "Scene/SceneDocumentAdapter.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <type_traits>

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
        PhysicsPresentationLease lease;
        static_assert(std::is_nothrow_swappable_v<decltype(m_registeredDocument)>);
        static_assert(std::is_nothrow_swappable_v<decltype(m_association)>);
        static_assert(std::is_nothrow_swappable_v<decltype(m_snapshot)>);
        if (!runtime.ApplyPhysicsPresentation(renderer, *association, *snapshot, error, &lease))
        {
            return {Status::Failed, 0, error};
        }
        m_session = std::move(session);
        m_registeredDocument.swap(registeredDocument);
        m_association.swap(association);
        m_snapshot.swap(snapshot);
        m_lease = lease;
        m_accumulatedTicks = 0.0;
        m_totalSteps = 0;
        m_lastUpdateSteps = 0;
        m_lastUpdateContactEvents.clear();
        m_lastUpdateContactNotifications.clear();
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
        if (registered.physics && (!sameVector(registered.transform.position, current.transform.position) ||
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
    try
    {
        std::string error;
        const auto readiness = runtime.CheckPhysicsPresentation(renderer, *m_association, *m_lease, error);
        if (readiness == PhysicsPresentationReadiness::Stale || readiness == PhysicsPresentationReadiness::Failed)
        {
            return FailAndStop(runtime, renderer, error);
        }
        if (!ValidateAuthoredPhysicsTransforms(runtime, error))
        {
            return FailAndStop(runtime, renderer, error);
        }
        m_lastUpdateSteps = 0;
        m_lastUpdateContactEvents.clear();
        m_lastUpdateContactNotifications.clear();

        if (elapsedSeconds == 0.0)
        {
            return {Status::Updated};
        }
        if (m_totalSteps > (std::numeric_limits<std::uint64_t>::max)() - m_maximumStepsPerUpdate)
        {
            return FailAndStop(runtime, renderer, "Simulated step counter is exhausted.");
        }

        // Tick units make common 60/120/30 Hz inputs exactly integral/half-integral.
        // A tiny boundary tolerance absorbs roundoff; accepted catch-up debt stays.
        constexpr double boundaryToleranceTicks = 1.0e-9;
        m_accumulatedTicks += elapsedSeconds * 60.0;
        while (m_accumulatedTicks >= 1.0 - boundaryToleranceTicks && m_lastUpdateSteps < m_maximumStepsPerUpdate)
        {
            m_session->Step();
            m_accumulatedTicks -= 1.0;
            if (m_accumulatedTicks < 0.0) m_accumulatedTicks = 0.0;
            ++m_lastUpdateSteps;
            ++m_totalSteps;
        }
        if (m_lastUpdateSteps != 0)
        {
            // Every step's events, each tagged with its own fixed step, after all steps finished.
            m_lastUpdateContactEvents = m_session->TakeContactEvents();
            m_lastUpdateContactNotifications = m_session->SelectContactNotifications(m_lastUpdateContactEvents);

            auto snapshot = m_session->CapturePoses(&error);
            if (!snapshot)
            {
                return FailAndStop(runtime, renderer, error);
            }
            m_snapshot.swap(snapshot);
            m_pendingPresentation = true;
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
            if (!runtime.ApplyPhysicsPresentation(renderer, *m_association, *m_snapshot, error, &acceptedLease))
            {
                return FailAndStop(runtime, renderer, error);
            }
            m_lease = acceptedLease;
            m_pendingPresentation = false;
        }
        return {Status::Published};
    }
    catch (const std::exception& exception)
    {
        return FailAndStop(runtime, renderer, exception.what());
    }
}

void PhysicsSimulationController::ReleaseSimulation() noexcept
{
    m_session.reset();
    m_snapshot.reset();
    m_lastUpdateContactEvents.clear();
    m_lastUpdateContactNotifications.clear();
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
