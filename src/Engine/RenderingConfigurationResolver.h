#pragma once

#include "Project/ProjectSettings.h"
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
