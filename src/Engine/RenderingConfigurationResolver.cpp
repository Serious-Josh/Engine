#include "Engine/RenderingConfigurationResolver.h"

#include <algorithm>

namespace
{
    RendererLightingMode ToRendererLightingMode(ProjectLightingMode mode)
    {
        switch(mode)
        {
        case ProjectLightingMode::ReferencePathTracer:
            return RendererLightingMode::PathTracedReference;
        case ProjectLightingMode::PathTracing:
            return RendererLightingMode::PathTracedRasterPrimaryReSTIRDI;
        case ProjectLightingMode::Legacy:
        default:
            return RendererLightingMode::Legacy;
        }
    }

    ToneMapOperator ToToneMapOperator(ProjectToneMapper toneMapper)
    {
        return toneMapper == ProjectToneMapper::GT7 ? ToneMapOperator::GT7 : ToneMapOperator::Existing;
    }

    ReflectionDenoiserMode ToReflectionDenoiser(ProjectReflectionDenoiser denoiser)
    {
        return denoiser == ProjectReflectionDenoiser::Native ? ReflectionDenoiserMode::Native : ReflectionDenoiserMode::Nrd;
    }

    PathTraceDenoiserMode ToPathTraceDenoiser(ProjectPathTraceDenoiser denoiser)
    {
        switch(denoiser)
        {
        case ProjectPathTraceDenoiser::Off:
            return PathTraceDenoiserMode::Off;
        case ProjectPathTraceDenoiser::Engine:
            return PathTraceDenoiserMode::Engine;
        case ProjectPathTraceDenoiser::NrdReblur:
            return PathTraceDenoiserMode::NrdReblur;
        case ProjectPathTraceDenoiser::NrdRelax:
            return PathTraceDenoiserMode::NrdRelax;
        case ProjectPathTraceDenoiser::NrdHybrid:
        default:
            return PathTraceDenoiserMode::NrdHybrid;
        }
    }

    ToneMapDebugView ToPsrDebugView(ProjectPsrDebugView view)
    {
        switch(view)
        {
        case ProjectPsrDebugView::ReplacementMask:
            return ToneMapDebugView::PsrReplacementMask;
        case ProjectPsrDebugView::ReflectedNormals:
            return ToneMapDebugView::PsrReflectedNormal;
        case ProjectPsrDebugView::ReflectedAlbedo:
            return ToneMapDebugView::PsrReflectedAlbedo;
        case ProjectPsrDebugView::Off:
        default:
            return ToneMapDebugView::None;
        }
    }

    PathTraceDenoiserView ToDenoiserView(ProjectPathTraceDenoiserView view)
    {
        switch(view)
        {
        case ProjectPathTraceDenoiserView::Temporal:
            return PathTraceDenoiserView::TemporalRadiance;
        case ProjectPathTraceDenoiserView::Spatial:
            return PathTraceDenoiserView::SpatialRadiance;
        case ProjectPathTraceDenoiserView::SpatialTemporalDifference:
            return PathTraceDenoiserView::Difference;
        case ProjectPathTraceDenoiserView::EstimatedUncertainty:
            return PathTraceDenoiserView::Uncertainty;
        case ProjectPathTraceDenoiserView::HistoryLength:
            return PathTraceDenoiserView::History;
        case ProjectPathTraceDenoiserView::RawExtraction:
            return PathTraceDenoiserView::Raw;
        case ProjectPathTraceDenoiserView::Normal:
        default:
            return PathTraceDenoiserView::Normal;
        }
    }

    // Path Tracing's quality and PSR request; the renderer decides the effective PSR chain.
    PathTraceReferenceSettings ToPathTracingQuality(const PathTracingRenderingSettings& pathTracing)
    {
        PathTraceReferenceSettings quality;
        quality.samplesPerFrame = pathTracing.samplesPerFrame;
        quality.maxPathDepth = pathTracing.maxPathDepth;
        quality.russianRouletteDepth = pathTracing.rouletteFromDepth;
        quality.maxAccumulatedSamples = (std::max)(pathTracing.sampleTarget, pathTracing.samplesPerFrame);
        quality.primarySurfaceReplacementInPlace = pathTracing.psrMode != ProjectPsrMode::Off;
        quality.primarySurfaceReplacementChain = pathTracing.psrMode == ProjectPsrMode::SingleBounce ? 1u : 2u;

        return quality;
    }

    PathTraceReferenceSettings ToReferenceQuality(const RenderingDiagnostics& diagnostics)
    {
        PathTraceReferenceSettings quality = GetReferencePathTracerQuality();

        if(diagnostics.referenceQuality)
        {
            const PathTraceReferenceSettings& requested = *diagnostics.referenceQuality;
            quality.samplesPerFrame = requested.samplesPerFrame;
            quality.maxPathDepth = requested.maxPathDepth;
            quality.russianRouletteDepth = requested.russianRouletteDepth;
            quality.maxAccumulatedSamples = (std::max)(requested.maxAccumulatedSamples, requested.samplesPerFrame);
        }

        return quality;
    }

    // Path Tracing's denoiser views and stage isolation run only with the Engine/NRD denoisers, which only
    // Path Tracing has.
    PathTraceDenoiserVisualization ToDenoiserVisualization(const PathTracingRenderingSettings& pathTracing)
    {
        PathTraceDenoiserVisualization visualization;
        visualization.view = ToDenoiserView(pathTracing.denoiserView);
        visualization.signal = pathTracing.denoiserSignal == ProjectPathTraceDenoiserSignal::Specular ? 1u : 0u;
        visualization.exposureStops = pathTracing.viewExposureStops;
        visualization.differenceAmplification = pathTracing.differenceAmplification;
        visualization.uncertaintyFloorLog10 = pathTracing.uncertaintyFloorLog10;

        return visualization;
    }

    // Renderer::RenderScale: DLSS Super Resolution's presets follow the three diagnostic scales.
    int ToRenderScaleIndex(const PathTracingRenderingSettings& pathTracing, const RenderingDiagnostics& diagnostics)
    {
        if(!pathTracing.dlss)
        {
            return std::clamp(diagnostics.diagnosticRenderScale, 0, 2);
        }

        switch(pathTracing.dlssQuality)
        {
        case ProjectDlssQuality::Balanced:
            return 4;
        case ProjectDlssQuality::Performance:
            return 5;
        case ProjectDlssQuality::Quality:
        default:
            return 3;
        }
    }
}

RenderingConfiguration ResolveRenderingConfiguration(const RenderingSettings& settings,
    const RenderingDiagnostics& diagnostics)
{
    const PathTracingRenderingSettings& pathTracing = settings.pathTracing;
    const RendererLightingMode mode = ToRendererLightingMode(settings.lightingMode);
    const bool isPathTracing = mode == RendererLightingMode::PathTracedRasterPrimaryReSTIRDI;
    const bool isReference = mode == RendererLightingMode::PathTracedReference;

    RenderingConfiguration configuration;
    configuration.lightingMode = mode;
    configuration.toneMapOperator = ToToneMapOperator(settings.toneMapper);

    // The Legacy values reach every frame, as before; the renderer applies them to Legacy lighting only.
    configuration.reflectionDenoiser = ToReflectionDenoiser(settings.legacy.reflectionDenoiser);
    configuration.contributionLimiter = settings.legacy.contributionLimiter;
    configuration.rtgi = settings.legacy.rtgi;
    configuration.rtgiContactAOStrength = settings.legacy.rtgiContactAOStrength;
    configuration.rtgiContactAORadius = settings.legacy.rtgiContactAORadius;

    // Legacy frames carry Path Tracing's quality unused, so returning to Path Tracing changes nothing.
    configuration.pathTrace = isReference ? ToReferenceQuality(diagnostics) : ToPathTracingQuality(pathTracing);

    // Stage isolation is applied whenever it changes (a change discards denoiser history), in every mode.
    configuration.denoiserStages = diagnostics.denoiserStages;
    configuration.dlssRequested = pathTracing.dlss;
    configuration.dlssInputPreparation = diagnostics.dlssInputPreparation;

    // SHARC's history prior needs the Engine denoiser to be the selection, whatever the mode.
    configuration.sharc = diagnostics.sharcTuning;
    configuration.sharc.enabled = isPathTracing && pathTracing.sharc;
    configuration.sharc.historyPrior = diagnostics.sharcTuning.historyPrior &&
        pathTracing.denoiser == ProjectPathTraceDenoiser::Engine;

    if(isPathTracing)
    {
        configuration.denoiser = ToPathTraceDenoiser(pathTracing.denoiser);
        configuration.denoiserVisualization = ToDenoiserVisualization(pathTracing);
        configuration.psrDebugView = pathTracing.psrMode != ProjectPsrMode::Off ?
            ToPsrDebugView(pathTracing.psrDebugView) : ToneMapDebugView::None;
        configuration.cameraJitter = pathTracing.cameraJitter;
        configuration.renderScaleIndex = ToRenderScaleIndex(pathTracing, diagnostics);

        configuration.dedicatedCaustics = pathTracing.dedicatedCaustics;
        configuration.causticOnlyView = pathTracing.dedicatedCaustics && pathTracing.causticOnlyView;
        configuration.pixelCentredCaustics = pathTracing.pixelCentredCaustics && !pathTracing.dedicatedCaustics;
        configuration.pixelCausticView = configuration.pixelCentredCaustics ? diagnostics.pixelCausticView : 0u;
        configuration.pixelCausticInsertion = configuration.pixelCentredCaustics && diagnostics.pixelCausticInsertion;
    }
    else if(isReference)
    {
        configuration.dedicatedCaustics = diagnostics.referenceDedicatedCaustics;
        configuration.causticOnlyView = diagnostics.referenceDedicatedCaustics && diagnostics.referenceCausticOnlyView;
    }

    return configuration;
}
