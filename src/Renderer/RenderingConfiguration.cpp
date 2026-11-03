#include "Renderer/RenderingConfiguration.h"

PathTraceReferenceSettings GetReferencePathTracerQuality()
{
    PathTraceReferenceSettings quality;
    quality.samplesPerFrame = 1;
    quality.maxPathDepth = 3;
    quality.russianRouletteDepth = 3;
    quality.maxAccumulatedSamples = 1u << 16;
    // PSR serves Path Tracing only; Reference ignores it either way (PS.2A baselines).
    quality.primarySurfaceReplacementInPlace = false;
    quality.primarySurfaceReplacementChain = 1;

    return quality;
}

bool IsDlssConfigurationEligible(const RenderingConfiguration& configuration, RendererLightingMode activeMode,
    bool denoiserPreparing)
{
    return configuration.lightingMode == RendererLightingMode::PathTracedRasterPrimaryReSTIRDI &&
        activeMode == configuration.lightingMode && configuration.denoiser != PathTraceDenoiserMode::Off &&
        !denoiserPreparing;
}

bool IsDlssPreparing(const RenderingConfiguration& configuration, RendererLightingMode activeMode,
    bool denoiserPreparing)
{
    return configuration.lightingMode == RendererLightingMode::PathTracedRasterPrimaryReSTIRDI &&
        configuration.denoiser != PathTraceDenoiserMode::Off &&
        (activeMode != configuration.lightingMode || denoiserPreparing);
}
