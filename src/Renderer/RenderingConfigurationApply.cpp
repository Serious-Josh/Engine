#include "Renderer/RenderingConfiguration.h"

#include "Renderer/Renderer.h"

#include <algorithm>

void ApplyRenderingConfiguration(const RenderingConfiguration& configuration, Renderer& renderer,
    RendererOptions& options)
{
    options.lightingMode = configuration.lightingMode;
    options.toneMapOperator = configuration.toneMapOperator;

    options.reflectionDenoiser = configuration.reflectionDenoiser;
    options.contributionLimiter = configuration.contributionLimiter;
    options.enableRTGI = configuration.rtgi;
    options.rtgiContactAOStrength = configuration.rtgiContactAOStrength;
    options.rtgiContactAORadius = configuration.rtgiContactAORadius;

    options.pathTraceReference = configuration.pathTrace;
    options.psrDebugView = configuration.psrDebugView;
    options.sharc = configuration.sharc;
    options.pathTraceDenoiserMode = configuration.denoiser;
    options.pathTraceDenoiserStages = configuration.denoiserStages;
    options.pathTraceDenoiserVisualization = configuration.denoiserVisualization;
    options.pathTraceCameraJitter = configuration.cameraJitter;
    options.dlssInputPreparation = configuration.dlssInputPreparation;

    // A toggle resets only the dedicated caustic history.
    renderer.SetDedicatedCaustics(configuration.dedicatedCaustics, configuration.causticOnlyView);
    renderer.SetPixelCentredCaustics(configuration.pixelCentredCaustics,
        static_cast<PixelCausticView>(configuration.pixelCausticView), configuration.pixelCausticInsertion);
}

void ApplyRenderScale(const RenderingConfiguration& configuration, Renderer& renderer, bool dlssEligible,
    bool dlssRetry)
{
    const int scale = std::clamp(configuration.renderScaleIndex, 0,
        static_cast<int>(Renderer::RenderScale::DlssPerformance));

    renderer.SetRenderScale(static_cast<Renderer::RenderScale>(scale), dlssEligible, dlssRetry,
        configuration.dlssRequested);
}
