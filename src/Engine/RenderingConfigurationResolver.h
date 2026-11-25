#pragma once

#include "Project/ProjectSettings.h"
#include "Renderer/Latency/ReflexStatus.h"
#include "Renderer/RenderingConfiguration.h"

// PS.2B: the one mapping from the project's rendering settings (PS.1 model) to the renderer's effective
// configuration. Every lighting-mode rule lives here:
// - Legacy, Reference and Path Tracing map onto their renderer modes (Path Tracing is raster primary +
//   ReSTIR DI; the standalone raster-primary mode is not reachable);
// - Reference uses its fixed internal quality, never Path Tracing's quality or PSR;
// - the denoiser, PSR debug view, denoiser views, camera jitter, render scale and SHARC apply to Path Tracing only;
// - the sample target is at least the samples per frame.
// The Editor and (from PS.4) the packaged runtime both resolve through it.
RenderingConfiguration ResolveRenderingConfiguration(const RenderingSettings& settings,
    const RenderingDiagnostics& diagnostics = {});

// FG.3: DLSS Frame Generation's saved toggle (rendering.frameGeneration.enabled). The renderer decides whether it can run
// (capability, Editor viewport only); the request alone locks Reflex at On.
bool IsFrameGenerationRequested(const RenderingSettings& settings);

// NVIDIA Reflex is not part of the rendering configuration: changing it neither prepares nor resets anything, so the
// frame loop applies it directly (Renderer::BeginLatencyFrame). DLSS Frame Generation requires Reflex and NVIDIA's UI rules
// forbid turning it off meanwhile, so a Frame Generation request runs On whatever mode is saved.
ReflexMode ResolveReflexMode(const RenderingSettings& settings);
