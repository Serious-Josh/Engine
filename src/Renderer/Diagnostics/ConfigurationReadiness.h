#pragma once

#include "Renderer/RendererLightingMode.h"
#include "Renderer/RenderingConfiguration.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

// M0c.4 Stage 4.0: a read-only view of whether a complete requested rendering configuration is ready, and of what the
// renderer actually rendered. Diagnostics only: evaluating it creates, compiles, allocates, waits for and changes nothing
// (Renderer::EvaluateConfigurationReadiness observes the same state the gates use, through const paths).

enum class ReadinessState : std::uint8_t
{
    Ready,
    // Not ready yet, because preparation is ongoing or has not been requested (the detail says which).
    Preparing,
    // The configuration cannot be prepared on this device or build (e.g. no DLSS runtime, no material tables).
    Unsupported,
    Failed,
    NotApplicable
};

const char* ReadinessStateName(ReadinessState state) noexcept;

enum class ReadinessDependency : std::uint8_t
{
    // Path-traced modes: DXR support, material tables and the path tracer's availability.
    PathTracingAvailability,
    // The lighting mode's own DXR libraries and pipelines (primary, ReSTIR DI), without denoiser, PSR or SHARC additions.
    PathTracingPipelines,
    // The path tracer's pass and mode resources (PT residency, RTXDI for ReSTIR DI).
    PathTracingResources,
    // The denoiser's additional DXR libraries and pipelines (signal extraction, raw diffuse hit distance).
    DenoiserPipelines,
    // The denoiser's instance or passes (NRD instance at the render extent; the Engine denoiser's passes).
    DenoiserInstance,
    // PSR: auxiliary pipelines and the PSR lighting set's libraries and pipelines.
    Psr,
    // SHARC: its training and render libraries and pipelines.
    Sharc,
    // DLSS Super Resolution: runtime availability and the feature at the current render/output extents.
    Dlss,
    // The renderer's output extent and effective render scale against the request.
    RenderExtent,
    // Ray Tracing (Legacy): its textures at the render extent and, with NRD reflections, the Legacy REBLUR pair.
    RayTracingResources,
    Count
};

const char* ReadinessDependencyName(ReadinessDependency dependency) noexcept;

struct DependencyReadiness
{
    ReadinessState state = ReadinessState::NotApplicable;
    std::string detail;

    bool operator==(const DependencyReadiness&) const = default;
};

// What preparation depends on: everything else in RenderingConfiguration (exposure, tone mapping, debug views, sample
// counts, AIR quality) is applied per frame and needs no preparation.
struct ConfigurationRequirements
{
    RendererLightingMode lightingMode = RendererLightingMode::Legacy;
    PathTraceDenoiserMode denoiser = PathTraceDenoiserMode::Off;
    // 0: none; 1 single bounce; 2 two-bounce (as RecordRenderFrame selects it).
    std::uint32_t psrChain = 0;
    bool sharc = false;
    bool nrdReflections = false;
    int renderScaleIndex = 0;
    bool dlss = false;
    unsigned int outputWidth = 0;
    unsigned int outputHeight = 0;

    bool operator==(const ConfigurationRequirements&) const = default;
};

std::string DescribeRequirements(const ConfigurationRequirements& requirements);

struct ConfigurationReadiness
{
    ConfigurationRequirements requirements;
    std::array<DependencyReadiness, static_cast<std::size_t>(ReadinessDependency::Count)> dependencies{};
    ReadinessState overall = ReadinessState::Ready;
};

// What the last recorded frame actually rendered.
struct RenderedConfiguration
{
    // M0c.4 Stage 4.3/4.5: false when the frame recorded no lighting at all (startup preparation, or a path-traced scene
    // with no resident geometry yet); the other fields then describe the presented configuration, not lit content.
    bool lit = true;
    RendererLightingMode lightingMode = RendererLightingMode::Legacy;
    PathTraceDenoiserMode denoiser = PathTraceDenoiserMode::Off;
    std::uint32_t psrChain = 0;
    bool sharc = false;
    bool dlss = false;
    unsigned int renderWidth = 0;
    unsigned int renderHeight = 0;
    unsigned int outputWidth = 0;
    unsigned int outputHeight = 0;

    bool operator==(const RenderedConfiguration&) const = default;
};

std::string DescribeRendered(const RenderedConfiguration& rendered);

// Whether a rendered frame shows exactly the requested configuration (mode, denoiser, PSR, SHARC, DLSS and output extent).
bool RenderedMatchesRequest(const RenderedConfiguration& rendered, const ConfigurationRequirements& requirements);

// Logs readiness per request as state transitions: the request (and when it was first observed), each dependency change
// with its time since the request, when the whole configuration first became ready, every change of what was rendered,
// the frames rendered in configurations other than the request, and the scene resizes in between. Nothing per frame.
class ConfigurationReadinessTrace
{
public:
    using Clock = std::chrono::steady_clock;

    // Whether this frame needs a full evaluation (the evaluation walks every DXR requirement, ~1.5-4 ms per call in Debug):
    // always while the request is not settled (ready and rendered as requested) or when the configuration or extent changed;
    // once settled, every SettledEvaluationInterval frames. Frames in between pass only what was rendered (ObserveRendered).
    bool NeedsEvaluation(const RenderingConfiguration& configuration, unsigned int outputWidth, unsigned int outputHeight);
    static constexpr std::uint64_t SettledEvaluationInterval = 30;

    void Observe(const ConfigurationReadiness& readiness, const RenderedConfiguration& rendered, bool frameRendered,
        Clock::time_point now);
    void ObserveRendered(const RenderedConfiguration& rendered, bool frameRendered, Clock::time_point now);

private:
    void Summarize(const char* reason, Clock::time_point now);
    double Since(Clock::time_point now) const;

    bool m_started = false;
    std::uint64_t m_requestNumber = 0;
    ConfigurationRequirements m_requirements;
    Clock::time_point m_requestTime{};
    std::array<DependencyReadiness, static_cast<std::size_t>(ReadinessDependency::Count)> m_dependencies{};
    ReadinessState m_overall = ReadinessState::Ready;
    bool m_readyLogged = false;
    double m_readySeconds = -1.0;
    bool m_matchedLogged = false;
    double m_matchedSeconds = -1.0;
    RenderedConfiguration m_rendered;
    bool m_renderedKnown = false;
    std::uint64_t m_framesRendered = 0;
    std::uint64_t m_framesNotMatching = 0;
    std::uint32_t m_renderedConfigurations = 0;
    std::uint32_t m_extentChanges = 0;
    bool m_haveInputs = false;
    RenderingConfiguration m_lastConfiguration;
    unsigned int m_lastOutputWidth = 0;
    unsigned int m_lastOutputHeight = 0;
    std::uint64_t m_framesSinceEvaluation = 0;
};
