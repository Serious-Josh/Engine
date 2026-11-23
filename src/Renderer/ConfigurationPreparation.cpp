#include "ConfigurationPreparation.h"

#include "Core/Log.h"

#include <cstdio>

namespace
{
    const char* ModeName(RendererLightingMode mode)
    {
        switch(mode)
        {
        case RendererLightingMode::Legacy:
            return "Ray Tracing";
        case RendererLightingMode::PathTracedReference:
            return "Reference";
        case RendererLightingMode::PathTracedRasterPrimary:
            return "Raster Primary";
        case RendererLightingMode::PathTracedRasterPrimaryReSTIRDI:
            return "Path Tracing (ReSTIR DI)";
        case RendererLightingMode::PathTracedRasterPrimaryReSTIRPT:
            return "Path Tracing (ReSTIR PT Enhanced)";
        }

        return "?";
    }

    const char* DenoiserName(PathTraceDenoiserMode denoiser)
    {
        switch(denoiser)
        {
        case PathTraceDenoiserMode::Off:
            return "Off";
        case PathTraceDenoiserMode::Engine:
            return "Engine";
        case PathTraceDenoiserMode::NrdReblur:
            return "REBLUR";
        case PathTraceDenoiserMode::NrdRelax:
            return "RELAX";
        case PathTraceDenoiserMode::NrdHybrid:
            return "hybrid";
        }

        return "?";
    }

    bool IsRasterPrimary(RendererLightingMode mode)
    {
        return mode == RendererLightingMode::PathTracedRasterPrimary || IsRasterPrimaryReSTIRMode(mode);
    }

    double Seconds(ConfigurationPreparation::Clock::duration duration)
    {
        return std::chrono::duration<double>(duration).count();
    }
}

ConfigurationIdentity MakeConfigurationIdentity(const RenderingConfiguration& configuration)
{
    ConfigurationIdentity identity;
    identity.lightingMode = configuration.lightingMode;
    const bool rasterPrimary = IsRasterPrimary(configuration.lightingMode);
    const bool restirDI = configuration.lightingMode == RendererLightingMode::PathTracedRasterPrimaryReSTIRDI;
    identity.denoiser = rasterPrimary ? configuration.denoiser : PathTraceDenoiserMode::Off;
    // M5.1: ReSTIR PT Enhanced uses neither SHARC nor PSR (restirDI alone decides the PSR chain below).
    identity.sharc = rasterPrimary && configuration.sharc.enabled &&
        configuration.lightingMode != RendererLightingMode::PathTracedRasterPrimaryReSTIRPT;

    // As RecordRenderFrame decides psrRequested and its chain.
    const PathTraceReferenceSettings& pathTrace = configuration.pathTrace;

    if(restirDI && pathTrace.primarySurfaceReplacementInPlace && pathTrace.maxPathDepth > 0u)
    {
        identity.psrChain = pathTrace.primarySurfaceReplacementChain >= 2u && pathTrace.maxPathDepth >= 3u ? 2u : 1u;
    }

    identity.renderScaleIndex = configuration.renderScaleIndex;
    identity.dlssRequested = configuration.dlssRequested;
    return identity;
}

std::string DescribeConfigurationIdentity(const ConfigurationIdentity& identity)
{
    char text[192];
    std::snprintf(text, sizeof(text), "%s, denoiser %s, PSR %u, SHARC %s, scale %d%s", ModeName(identity.lightingMode),
        DenoiserName(identity.denoiser), identity.psrChain, identity.sharc ? "on" : "off", identity.renderScaleIndex,
        identity.dlssRequested ? " (DLSS requested)" : "");
    return text;
}

const char* ConfigurationPreparationStateName(ConfigurationPreparationState state) noexcept
{
    switch(state)
    {
    case ConfigurationPreparationState::Presenting:
        return "presenting";
    case ConfigurationPreparationState::Preparing:
        return "preparing";
    case ConfigurationPreparationState::Failed:
        return "failed";
    }

    return "?";
}

void ConfigurationPreparation::PresentInitial(const RenderingConfiguration& requested)
{
    // M0c.4 Stage 4.3: the startup presentation while the first request prepares. Nothing is lit (the renderer records no
    // lighting and presents its preparation screen, Renderer::IsLightingSuppressed); the configuration only carries the
    // request's per-frame values and its render scale, so a DLSS request's render extent is set up once, here, and the
    // switch needs no second resize. Ray Tracing is the identity only because it needs no preparation: it is lit only if
    // the request fails or is unsupported.
    RenderingConfiguration initial = requested;
    initial.lightingMode = RendererLightingMode::Legacy;
    initial.denoiser = PathTraceDenoiserMode::Off;
    initial.sharc.enabled = false;
    // Only the first presentation starts the unlit state; later calls follow the request's per-frame values, and after a
    // failure fallback they must not hide the Ray Tracing presentation again.
    if(!m_hasPresented)
    {
        m_status.startupPreparing = true;
    }

    m_presented = initial;
    m_previousPresented = initial;
    m_status.presented = MakeConfigurationIdentity(initial);
    m_hasPresented = true;
    m_initialPresentation = true;
}

void ConfigurationPreparation::EndStartupPreparing(const char* reason)
{
    if(m_status.startupPreparing)
    {
        m_status.startupPreparing = false;
        Log::Info(std::string("Startup preparation presentation ended: ") + reason + ".");
    }
}

void ConfigurationPreparation::PresentSameIdentity(const RenderingConfiguration& requested)
{
    if(m_status.pending)
    {
        // Back to the presented identity before the pending one was ready: nothing switches.
        Log::Info("Configuration request #" + std::to_string(m_status.generation) + " cancelled; " +
            DescribeConfigurationIdentity(m_status.presented) + " stays presented.");
        ++m_status.supersededRequests;
        m_status.pending.reset();
    }

    m_presented = requested;
    m_initialPresentation = false;
    m_status.state = ConfigurationPreparationState::Presenting;
    m_status.detail.clear();
    m_failureLogged = false;
    EndStartupPreparing("the request needs no preparation");
}

bool ConfigurationPreparation::Request(const ConfigurationIdentity& identity, Clock::time_point now)
{
    if(m_status.pending && *m_status.pending == identity)
    {
        return false;
    }

    if(m_status.pending)
    {
        Log::Info("Configuration request #" + std::to_string(m_status.generation) + " (" +
            DescribeConfigurationIdentity(*m_status.pending) + ") superseded after " +
            std::to_string(Seconds(now - m_pendingSince)) + " s.");
        ++m_status.supersededRequests;
    }

    m_status.pending = identity;
    ++m_status.generation;
    m_status.state = ConfigurationPreparationState::Preparing;
    m_status.detail.clear();
    m_pendingSince = now;
    m_failureLogged = false;
    Log::Info("Configuration request #" + std::to_string(m_status.generation) + ": " + DescribeConfigurationIdentity(identity) +
        "; presenting " + DescribeConfigurationIdentity(m_status.presented) + " while it prepares.");
    return true;
}

void ConfigurationPreparation::Prepared(const RenderingConfiguration& requested, PreparationStep step,
    const std::string& detail, Clock::time_point now)
{
    switch(step)
    {
    case PreparationStep::Ready:
    case PreparationStep::Unsupported:
        Switch(requested, now);
        return;
    case PreparationStep::Preparing:
        m_status.state = ConfigurationPreparationState::Preparing;
        m_status.detail = detail;
        ++m_status.heldFrames;

        // The startup presentation follows the request's per-frame values while it prepares.
        if(m_initialPresentation)
        {
            PresentInitial(requested);
        }

        return;
    case PreparationStep::Failed:
        m_status.state = ConfigurationPreparationState::Failed;
        m_status.detail = detail;
        ++m_status.heldFrames;

        if(!m_failureLogged)
        {
            m_failureLogged = true;
            m_failedAt = now;
            ++m_status.failures;
            Log::Error("Configuration request #" + std::to_string(m_status.generation) + " (" +
                DescribeConfigurationIdentity(m_status.pending.value_or(ConfigurationIdentity{})) + ") failed to prepare; " +
                DescribeConfigurationIdentity(m_status.presented) + " stays presented: " + detail);
        }

        // At startup an actual failure falls back to Ray Tracing (lit), still retried; a slow compile never does.
        if(m_initialPresentation)
        {
            PresentInitial(requested);
            EndStartupPreparing("the requested configuration failed to prepare; Ray Tracing is presented");
        }

        return;
    }
}

void ConfigurationPreparation::Switch(const RenderingConfiguration& requested, Clock::time_point now)
{
    const ConfigurationIdentity identity = MakeConfigurationIdentity(requested);
    m_status.lastPreparationSeconds = Seconds(now - m_pendingSince);
    Log::Info("Configuration request #" + std::to_string(m_status.generation) + " prepared in " +
        std::to_string(m_status.lastPreparationSeconds) + " s; switching from " + DescribeConfigurationIdentity(m_status.presented) +
        " to " + DescribeConfigurationIdentity(identity) + ".");
    m_previousPresented = m_presented;
    m_presented = requested;
    m_status.presented = identity;
    m_status.pending.reset();
    m_status.state = ConfigurationPreparationState::Presenting;
    m_status.detail.clear();
    ++m_status.switches;

    // Requested and prepared in the same frame: nothing needed preparation.
    if(now == m_pendingSince)
    {
        ++m_status.immediateSwitches;
    }

    m_initialPresentation = false;
    m_switchPendingFirstFrame = true;
    m_failureLogged = false;
    EndStartupPreparing("the requested configuration is presented");
}

void ConfigurationPreparation::ActivationFailed(const std::string& reason, Clock::time_point now)
{
    const ConfigurationIdentity failed = m_status.presented;
    Log::Error("Configuration " + DescribeConfigurationIdentity(failed) + " failed to activate; restoring " +
        DescribeConfigurationIdentity(MakeConfigurationIdentity(m_previousPresented)) + ": " + reason);
    m_presented = m_previousPresented;
    m_status.presented = MakeConfigurationIdentity(m_previousPresented);
    m_status.pending = failed;
    m_status.state = ConfigurationPreparationState::Failed;
    m_status.detail = reason;
    ++m_status.failures;
    m_failureLogged = true;
    m_failedAt = now;
    m_switchPendingFirstFrame = false;
    EndStartupPreparing("the requested configuration failed to activate; Ray Tracing is presented");
}

void ConfigurationPreparation::Reset()
{
    *this = ConfigurationPreparation{};
}
