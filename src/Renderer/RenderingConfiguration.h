#pragma once

#include "Renderer/Passes/PathTraceSpatialPass.h"
#include "Renderer/Passes/ToneMapPass.h"
#include "Renderer/RendererLightingMode.h"

#include <cstdint>
#include <optional>

class Renderer;
struct RendererOptions;

// Legacy reflection denoiser.
enum class ReflectionDenoiserMode
{
    Nrd,
    Native
};

// PS.2B: the effective rendering configuration of a frame, already resolved for its lighting mode
// (ResolveRenderingConfiguration). Every value here is what the renderer applies; values that do not
// apply to the mode hold their inactive defaults. Independent of DebugUI, the Editor and the project
// layer, so the Editor and the packaged runtime share it.
struct RenderingConfiguration
{
    RendererLightingMode lightingMode = RendererLightingMode::Legacy;
    // Final display mapping only. Existing is AgX.
    ToneMapOperator toneMapOperator = ToneMapOperator::Existing;

    // Legacy.
    ReflectionDenoiserMode reflectionDenoiser = ReflectionDenoiserMode::Nrd;
    bool contributionLimiter = true;
    bool rtgi = true;
    float rtgiContactAOStrength = 1.0f;
    float rtgiContactAORadius = 2.5f;

    // Path-traced modes: Path Tracing's quality and PSR, or Reference's fixed quality.
    PathTraceReferenceSettings pathTrace;

    // Path Tracing only (None / Off / defaults in the other modes).
    ToneMapDebugView psrDebugView = ToneMapDebugView::None;
    PathTraceDenoiserMode denoiser = PathTraceDenoiserMode::Off;
    PathTraceDenoiserVisualization denoiserVisualization{};
    PathTraceDenoiserStages denoiserStages{};
    bool cameraJitter = false;
    // Renderer::RenderScale order: 0 Native, 1 bilinear 75%, 2 DLSS 75%, 3 DLSS Quality, 4 Balanced, 5 Performance.
    int renderScaleIndex = 0;
    // The user asked for DLSS Super Resolution (normal, not diagnostic, DLSS).
    bool dlssRequested = false;
    bool dlssInputPreparation = false;
    SharcSettings sharc{};
    bool pixelCentredCaustics = false;
    std::uint32_t pixelCausticView = 0;
    bool pixelCausticInsertion = false;

    // Path Tracing, and Reference through a Debug panel request only.
    bool dedicatedCaustics = false;
    bool causticOnlyView = false;

    bool operator==(const RenderingConfiguration&) const = default;
};

// Debug panel inputs outside the project model: development and diagnostic controls. Never saved.
// Mode rules for them are applied by ResolveRenderingConfiguration like everything else.
struct RenderingDiagnostics
{
    // 0 Native, 1 bilinear 75%, 2 DLSS 75%; superseded by DLSS Super Resolution.
    int diagnosticRenderScale = 0;
    bool dlssInputPreparation = false;
    PathTraceDenoiserStages denoiserStages{};
    // SHARC tuning; enabled comes from the settings, and historyPrior is the request (Engine denoiser only).
    SharcSettings sharcTuning{};
    std::uint32_t pixelCausticView = 0;
    bool pixelCausticInsertion = false;

    // Reference quality requested by the Debug panel, whose quality sliders both path-traced modes share
    // until PS.2C. Absent: Reference's fixed internal quality.
    std::optional<PathTraceReferenceSettings> referenceQuality;
    // Dedicated caustics requested for Reference by the Debug panel (no project setting reaches Reference).
    bool referenceDedicatedCaustics = false;
    bool referenceCausticOnlyView = false;

    bool operator==(const RenderingDiagnostics&) const = default;
};

// Reference Path Tracer's fixed internal quality (1 spp, depth 3, roulette 3, target 65536; no PSR).
PathTraceReferenceSettings GetReferencePathTracerQuality();

// DLSS may be configured only in an active Path Tracing frame whose denoiser runs and is prepared.
bool IsDlssConfigurationEligible(const RenderingConfiguration& configuration, RendererLightingMode activeMode,
    bool denoiserPreparing);

// Path Tracing with a denoiser is requested, but the mode or the denoiser is still preparing.
bool IsDlssPreparing(const RenderingConfiguration& configuration, RendererLightingMode activeMode,
    bool denoiserPreparing);

// The one application path of a configuration: its per-frame values into options, and the caustic
// passes through their renderer setters (before the frame's caustic snapshot boundary).
void ApplyRenderingConfiguration(const RenderingConfiguration& configuration, Renderer& renderer,
    RendererOptions& options);

// The render scale, applied with the next scene resize. Eligibility and retry come from the caller's
// view of the renderer state (IsDlssConfigurationEligible).
void ApplyRenderScale(const RenderingConfiguration& configuration, Renderer& renderer, bool dlssEligible,
    bool dlssRetry);
