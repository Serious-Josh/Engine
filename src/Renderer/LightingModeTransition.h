#pragma once

#include "RendererLightingMode.h"
#include "Shader/ShaderLibraryCompileWorker.h"

#include <chrono>
#include <functional>
#include <string>

// Async DXR preparation, Gate 2: the requested lighting mode (what the application asks for each frame)
// versus the active one (what the last recorded frame rendered). While a requested DXR mode's libraries
// compile on the worker, the active mode keeps rendering; the requested mode is activated only on a frame
// whose render-thread readiness checks pass. Renderer-owned, render thread only, no D3D12 access.
enum class LightingModeTransitionStage
{
    Idle,
    Queued,
    Compiling,
    // Render thread, within one recording: state objects, root signatures and shader tables.
    CreatingPipeline,
    // Render thread, within one recording: accumulation, RTXDI and descriptor allocation.
    InitializingResources,
    // Render thread: everything is created; the recording's mode gates decide activation.
    ReadyToActivate,
    Failed
};

struct LightingModeTransitionStatus
{
    RendererLightingMode requested = RendererLightingMode::Legacy;
    RendererLightingMode active = RendererLightingMode::Legacy;
    LightingModeTransitionStage stage = LightingModeTransitionStage::Idle;
    std::string diagnostic;
};

class LightingModeTransition
{
    public:
    using Clock = std::chrono::steady_clock;

    // A failed request is attempted again at most this often while it stays requested, so an unchanged
    // failing source is not rechecked every frame and a temporary scene incompatibility never sticks.
    static constexpr Clock::duration FailedRetryInterval = std::chrono::seconds(1);

    // Chooses the mode this recording attempts. Without asynchronous preparation (packaged runtime, or no
    // worker) the requested mode is attempted directly, exactly as before. Otherwise a requested DXR mode
    // that is not ready polls prepareLibraries and, until they are all ready, the active mode is attempted.
    RendererLightingMode SelectCandidate(
        RendererLightingMode requested,
        bool asynchronous,
        Clock::time_point now,
        const std::function<bool()>& isRequestedReady,
        const std::function<ShaderLibraryPreparation()>& prepareLibraries);

    // Render-thread activation stages between SelectCandidate and Resolve.
    void SetActivationStage(LightingModeTransitionStage stage);

    // After the candidate's gates were evaluated: the mode to record. A requested mode that failed to
    // activate leaves the active mode rendering when it is still runnable, otherwise Legacy.
    RendererLightingMode Resolve(
        RendererLightingMode candidate,
        bool candidateRunnable,
        const std::string& candidateFailure,
        Clock::time_point now,
        const std::function<bool(RendererLightingMode)>& isRunnable);

    // The mode the recording actually rendered: the candidate when it ran, Legacy otherwise.
    void CommitRendered(RendererLightingMode rendered);

    void Reset();

    const LightingModeTransitionStatus& GetStatus() const noexcept
    {
        return m_status;
    }

    private:
    void Fail(const std::string& diagnostic, Clock::time_point now);

    LightingModeTransitionStatus m_status;
    bool m_asynchronous = false;
    RendererLightingMode m_failedMode = RendererLightingMode::Legacy;
    Clock::time_point m_failedAt{};
};
