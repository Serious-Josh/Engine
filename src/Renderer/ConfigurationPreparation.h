#pragma once

#include "Renderer/RendererLightingMode.h"
#include "Renderer/RenderingConfiguration.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

// M0c.4 Stage 4.1/4.2: whole-configuration preparation and atomic switching.
//
// The renderer presents one complete configuration at a time. When the requested configuration's identity (what its
// preparation depends on) differs from the presented one's, the presented configuration keeps rendering, unchanged,
// while everything the request needs is prepared in the background through the same preparation functions the
// renderer's per-feature gates use. Only when all of it is ready does the request become the presented configuration,
// in one frame: every gate then passes in that frame, and the only scene resize is the one that frame applies.
//
// Values outside the identity (exposure, tone mapping, sample counts, debug views, AIR quality) need no preparation and
// apply on the next frame whenever the identity is unchanged.

// What preparation depends on, with values that do not apply to the mode normalized (so a change the mode ignores is
// not a new identity).
struct ConfigurationIdentity
{
    RendererLightingMode lightingMode = RendererLightingMode::Legacy;
    // Raster-primary path-traced modes only; Off otherwise.
    PathTraceDenoiserMode denoiser = PathTraceDenoiserMode::Off;
    // Raster-primary path-traced modes only.
    bool sharc = false;
    // ReSTIR DI only: 0 none, 1 single bounce, 2 two-bounce (as RecordRenderFrame selects it).
    std::uint32_t psrChain = 0;
    // Renderer::RenderScale index and the DLSS request (applied by the resize in the switching frame).
    int renderScaleIndex = 0;
    bool dlssRequested = false;

    bool operator==(const ConfigurationIdentity&) const = default;
};

ConfigurationIdentity MakeConfigurationIdentity(const RenderingConfiguration& configuration);
std::string DescribeConfigurationIdentity(const ConfigurationIdentity& identity);

enum class ConfigurationPreparationState : std::uint8_t
{
    // The presented configuration is the requested one.
    Presenting,
    // A different request is being prepared; the presented configuration keeps rendering.
    Preparing,
    // The request could not be prepared; the presented configuration keeps rendering until the request changes.
    Failed
};

const char* ConfigurationPreparationStateName(ConfigurationPreparationState state) noexcept;

// The outcome of one non-blocking preparation step for a pending identity.
enum class PreparationStep : std::uint8_t
{
    Ready,
    Preparing,
    Failed,
    // The renderer cannot provide this configuration at all (no DXR device, no material tables): it is passed to the
    // existing gates, which report it and keep their established fallback, rather than held forever.
    Unsupported
};

struct ConfigurationPreparationStatus
{
    ConfigurationPreparationState state = ConfigurationPreparationState::Presenting;
    ConfigurationIdentity presented;
    std::optional<ConfigurationIdentity> pending;
    // The current request's generation: increases whenever the pending identity changes.
    std::uint64_t generation = 0;
    // What the pending configuration is waiting for, or why it failed.
    std::string detail;
    std::uint64_t switches = 0;
    // Switches whose request needed no preparation (presented in the frame it was requested).
    std::uint64_t immediateSwitches = 0;
    std::uint64_t supersededRequests = 0;
    std::uint64_t failures = 0;
    // Frames presented while a different configuration was pending.
    std::uint64_t heldFrames = 0;
    double lastPreparationSeconds = 0.0;
    // M0c.4 Stage 4.3: the first request is still preparing and nothing is lit (no configuration is active yet).
    bool startupPreparing = false;
    // The pending request's preparation progress in [0, 1] (accepted libraries and ready preparation sets).
    float progress = 0.0f;
};

// The renderer-owned state: the presented configuration, the pending identity and their timing. Preparation itself is
// done by the Renderer (Renderer::SelectPresentedConfiguration), which owns every resource and worker involved.
class ConfigurationPreparation
{
public:
    using Clock = std::chrono::steady_clock;

    // Before any configuration has been presented, Legacy renders (the established startup presentation until stage 4.3).
    bool HasPresented() const noexcept { return m_hasPresented; }
    const RenderingConfiguration& GetPresented() const noexcept { return m_presented; }
    const ConfigurationPreparationStatus& GetStatus() const noexcept { return m_status; }

    // The startup configuration presented before the first request is prepared: nothing lit (Stage 4.3), with the
    // request's per-frame values and render scale; Ray Tracing only after an actual failure.
    void PresentInitial(const RenderingConfiguration& requested);

    void SetProgress(float progress) noexcept
    {
        m_status.progress = progress;
    }

    // The request's identity matches the presented one: present it as is (its per-frame values may differ).
    void PresentSameIdentity(const RenderingConfiguration& requested);

    // Records the pending identity. Returns true when it differs from the previous pending one (a new generation); a
    // previous pending request that differs is superseded (its compiled work stays cached for reuse).
    bool Request(const ConfigurationIdentity& identity, Clock::time_point now);

    // Advances the pending request with the outcome of this frame's preparation step.
    void Prepared(const RenderingConfiguration& requested, PreparationStep step, const std::string& detail,
        Clock::time_point now);

    // The switching frame did not render the pending mode (its activation failed in the renderer): the previously
    // presented configuration is restored and the request is reported as failed until it changes.
    void ActivationFailed(const std::string& reason, Clock::time_point now);

    // Whether the presented configuration became presented this frame and has not rendered yet.
    bool IsSwitchPendingFirstFrame() const noexcept { return m_switchPendingFirstFrame; }
    void FirstFrameRendered() noexcept { m_switchPendingFirstFrame = false; }

    // The configuration presented before the last switch (restored by ActivationFailed).
    const RenderingConfiguration& GetPreviousPresented() const noexcept { return m_previousPresented; }

    // A failed request is attempted again after this interval while it stays requested (as LightingModeTransition retries a
    // failed mode), or at once when the request changes.
    static constexpr Clock::duration FailedRetryInterval = std::chrono::seconds(1);
    bool IsRetryDue(Clock::time_point now) const noexcept
    {
        return m_status.state == ConfigurationPreparationState::Failed && now - m_failedAt >= FailedRetryInterval;
    }

    void Reset();

private:
    void EndStartupPreparing(const char* reason);
    void Switch(const RenderingConfiguration& requested, Clock::time_point now);

    bool m_hasPresented = false;
    bool m_initialPresentation = false;
    RenderingConfiguration m_presented;
    RenderingConfiguration m_previousPresented;
    ConfigurationPreparationStatus m_status;
    Clock::time_point m_pendingSince{};
    bool m_switchPendingFirstFrame = false;
    bool m_failureLogged = false;
    Clock::time_point m_failedAt{};
};
