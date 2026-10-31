#include "LightingModeTransition.h"

RendererLightingMode LightingModeTransition::SelectCandidate(
    RendererLightingMode requested,
    bool asynchronous,
    Clock::time_point now,
    const std::function<bool()>& isRequestedReady,
    const std::function<ShaderLibraryPreparation()>& prepareLibraries)
{
    m_asynchronous = asynchronous;
    m_status.requested = requested;
    m_preparationFailed = false;

    // Synchronous preparation, Legacy and the mode already rendering are attempted directly; the
    // recording's own gates decide, as before.
    if(!asynchronous || requested == RendererLightingMode::Legacy || requested == m_status.active)
    {
        m_status.stage = LightingModeTransitionStage::Idle;
        m_status.diagnostic.clear();
        return requested;
    }

    const bool retryPending = m_status.stage == LightingModeTransitionStage::Failed && m_failedMode == requested &&
        now - m_failedAt < FailedRetryInterval;

    if(retryPending)
    {
        return m_status.active;
    }

    // Already created and current (an earlier activation): the existing fast transition.
    if(isRequestedReady())
    {
        m_status.stage = LightingModeTransitionStage::ReadyToActivate;
        m_status.diagnostic.clear();
        return requested;
    }

    const ShaderLibraryPreparation preparation = prepareLibraries();

    switch(preparation.state)
    {
    case ShaderLibraryPreparationState::Ready:
        m_status.stage = LightingModeTransitionStage::CreatingPipeline;
        m_status.diagnostic.clear();
        return requested;
    case ShaderLibraryPreparationState::Queued:
        m_status.stage = LightingModeTransitionStage::Queued;
        m_status.diagnostic.clear();
        return m_status.active;
    case ShaderLibraryPreparationState::Compiling:
        m_status.stage = LightingModeTransitionStage::Compiling;
        m_status.diagnostic.clear();
        return m_status.active;
    case ShaderLibraryPreparationState::PreparingPipelines:
        m_status.stage = LightingModeTransitionStage::PreparingPipelines;
        m_status.diagnostic.clear();
        return m_status.active;
    case ShaderLibraryPreparationState::Failed:
        Fail(preparation.diagnostic, now);
        m_preparationFailed = true;
        return m_status.active;
    }

    return m_status.active;
}

void LightingModeTransition::SetActivationStage(LightingModeTransitionStage stage)
{
    m_status.stage = stage;
}

RendererLightingMode LightingModeTransition::Resolve(
    RendererLightingMode candidate,
    bool candidateRunnable,
    const std::string& candidateFailure,
    Clock::time_point now,
    const std::function<bool(RendererLightingMode)>& isRunnable)
{
    if(candidateRunnable)
    {
        return candidate;
    }

    // Unchanged synchronous behaviour: the recording renders Legacy and the status says why.
    if(!m_asynchronous)
    {
        Fail(candidateFailure, now);
        return candidate;
    }

    const bool activationFailed = candidate == m_status.requested && candidate != m_status.active;

    if(activationFailed)
    {
        Fail(candidateFailure, now);
        const RendererLightingMode previous = m_status.active;

        if(previous != RendererLightingMode::Legacy && isRunnable(previous))
        {
            return previous;
        }

        return RendererLightingMode::Legacy;
    }

    // The active mode itself is no longer runnable (for example after a scene change): the existing
    // Legacy fallback. It is reattempted like any other request on later frames.
    if(candidate == m_status.requested)
    {
        Fail(candidateFailure, now);
    }

    return candidate;
}

void LightingModeTransition::CommitRendered(RendererLightingMode rendered)
{
    m_status.active = rendered;

    if(rendered == m_status.requested)
    {
        m_status.stage = LightingModeTransitionStage::Idle;
        m_status.diagnostic.clear();
    }
}

void LightingModeTransition::Reset()
{
    m_status = {};
    m_asynchronous = false;
    m_preparationFailed = false;
    m_failedMode = RendererLightingMode::Legacy;
    m_failedAt = {};
}

void LightingModeTransition::Fail(const std::string& diagnostic, Clock::time_point now)
{
    m_status.stage = LightingModeTransitionStage::Failed;
    m_status.diagnostic = diagnostic;
    m_failedMode = m_status.requested;
    m_failedAt = now;
}
