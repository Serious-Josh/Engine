// M0c.4 Stage 4.0: the read-only configuration readiness probe. Every check observes state the renderer's gates already
// keep (const members, worker states, NRD instance extents, the DLSS feature status); nothing here prepares, submits,
// takes worker results, allocates, waits, logs or changes a transition. Diagnostics only (Engine calls it when
// ENGINE_DIAG_READINESS is set).

#include "Renderer.h"

#include <algorithm>
#include <set>
#include <string>

namespace
{
    ReadinessState FromPreparation(ShaderLibraryPreparationState state)
    {
        switch(state)
        {
        case ShaderLibraryPreparationState::Ready:
            return ReadinessState::Ready;
        case ShaderLibraryPreparationState::Failed:
            return ReadinessState::Failed;
        case ShaderLibraryPreparationState::Queued:
        case ShaderLibraryPreparationState::Compiling:
        case ShaderLibraryPreparationState::PreparingPipelines:
            break;
        }

        return ReadinessState::Preparing;
    }

    // Unsupported before Failed before Preparing before Ready; NotApplicable never wins.
    int Severity(ReadinessState state)
    {
        switch(state)
        {
        case ReadinessState::Unsupported:
            return 4;
        case ReadinessState::Failed:
            return 3;
        case ReadinessState::Preparing:
            return 2;
        case ReadinessState::Ready:
            return 1;
        case ReadinessState::NotApplicable:
            break;
        }

        return 0;
    }

    void Combine(DependencyReadiness& into, const DependencyReadiness& other)
    {
        if(Severity(other.state) > Severity(into.state))
        {
            into = other;
        }
    }

    std::vector<PathTraceLibraryRequirement> LibrariesNotIn(const std::vector<PathTraceLibraryRequirement>& libraries,
        const std::vector<PathTraceLibraryRequirement>& base)
    {
        std::vector<PathTraceLibraryRequirement> result;

        for(const PathTraceLibraryRequirement& library : libraries)
        {
            const bool shared = std::any_of(base.begin(), base.end(), [&](const PathTraceLibraryRequirement& other)
            {
                return other.path == library.path;
            });

            if(!shared)
            {
                result.push_back(library);
            }
        }

        return result;
    }

    std::vector<PathTraceReferencePass::PipelineRequirement> PipelinesNotIn(
        const std::vector<PathTraceReferencePass::PipelineRequirement>& pipelines,
        const std::vector<PathTraceReferencePass::PipelineRequirement>& base)
    {
        // Each set's keys are built once (a key is a long description string).
        std::set<std::string> baseKeys;

        for(const PathTraceReferencePass::PipelineRequirement& other : base)
        {
            baseKeys.insert(PathTraceReferencePass::PipelineKey(other.description));
        }

        std::vector<PathTraceReferencePass::PipelineRequirement> result;

        for(const PathTraceReferencePass::PipelineRequirement& pipeline : pipelines)
        {
            if(!baseKeys.contains(PathTraceReferencePass::PipelineKey(pipeline.description)))
            {
                result.push_back(pipeline);
            }
        }

        return result;
    }

    DependencyReadiness Make(ReadinessState state, std::string detail = {})
    {
        return DependencyReadiness{state, std::move(detail)};
    }

    // An NRD instance created lazily on its first dispatch (synchronously, on the render thread).
    DependencyReadiness ObserveNrdInstance(const NrdDenoiser& denoiser, bool creationFailed, unsigned int width, unsigned int height,
        const char* name)
    {
        if(creationFailed)
        {
            return Make(ReadinessState::Failed, std::string(name) + " instance creation failed");
        }

        if(!denoiser.IsInitialized())
        {
            return Make(ReadinessState::Preparing, std::string(name) + " instance created on its first dispatch");
        }

        if(denoiser.GetWidth() != width || denoiser.GetHeight() != height)
        {
            return Make(ReadinessState::Preparing, std::string(name) + " instance at " + std::to_string(denoiser.GetWidth()) + "x" +
                std::to_string(denoiser.GetHeight()) + ", not the render extent");
        }

        return Make(ReadinessState::Ready);
    }
}

DependencyReadiness Renderer::ObserveDxrRequirements(const std::vector<PathTraceLibraryRequirement>& libraries,
    const std::vector<PathTraceReferencePass::PipelineRequirement>& pipelines) const
{
    if(libraries.empty() && pipelines.empty())
    {
        return Make(ReadinessState::NotApplicable);
    }

    const ShaderLibraryCompileWorker* compileWorker = m_shaderCompileWorkerRunning ? m_shaderCompileWorker.get() : nullptr;
    const RayTracingPipelineWorker* pipelineWorker = m_pipelineWorkerRunning ? m_pipelineWorker.get() : nullptr;
    DependencyReadiness result = Make(ReadinessState::Ready);
    std::size_t librariesReady = 0;

    for(const PathTraceLibraryRequirement& library : libraries)
    {
        std::string detail;
        ShaderLibraryPreparationState state = ShaderLibraryPreparationState::Ready;

        if(std::find(m_preparedDxrLibraries.begin(), m_preparedDxrLibraries.end(), library.path) == m_preparedDxrLibraries.end())
        {
            state = m_pathTraceReferencePass.ObserveLibrary(compileWorker, m_shaderLibrary.IsPackaged(), library.path, detail);

            // A failed RayQuery twin keeps its TraceRay pipeline; it never blocks (as PrepareDxrLibraries treats it).
            if(library.optional && state == ShaderLibraryPreparationState::Failed)
            {
                state = ShaderLibraryPreparationState::Ready;
            }
        }

        librariesReady += state == ShaderLibraryPreparationState::Ready ? 1 : 0;
        Combine(result, Make(FromPreparation(state), detail));
    }

    if(result.state != ReadinessState::Ready)
    {
        result.detail += " (" + std::to_string(librariesReady) + " of " + std::to_string(libraries.size()) + " libraries ready)";
        return result;
    }

    std::size_t pipelinesReady = 0;

    for(const PathTraceReferencePass::PipelineRequirement& pipeline : pipelines)
    {
        std::string detail;
        const ShaderLibraryPreparationState state = m_pathTraceReferencePass.ObservePipeline(pipelineWorker,
            m_graphicsDevice.GetRayTracingDevice(), pipeline, detail);
        pipelinesReady += state == ShaderLibraryPreparationState::Ready ? 1 : 0;
        Combine(result, Make(FromPreparation(state), detail));
    }

    if(result.state != ReadinessState::Ready)
    {
        result.detail += " (" + std::to_string(pipelinesReady) + " of " + std::to_string(pipelines.size()) + " pipelines ready)";
    }

    return result;
}

ConfigurationReadiness Renderer::EvaluateConfigurationReadiness(const RenderingConfiguration& configuration, unsigned int outputWidth,
    unsigned int outputHeight) const
{
    ConfigurationReadiness readiness;
    ConfigurationRequirements& requirements = readiness.requirements;
    const RendererLightingMode mode = configuration.lightingMode;
    const bool pathTraced = mode != RendererLightingMode::Legacy;
    const bool restirDI = mode == RendererLightingMode::PathTracedRasterPrimaryReSTIRDI;
    // M5.1: ReSTIR PT Enhanced is a raster-primary mode (denoisers, DLSS) without PSR or SHARC.
    const bool restirPT = mode == RendererLightingMode::PathTracedRasterPrimaryReSTIRPT;
    const bool rasterPrimary = restirDI || restirPT || mode == RendererLightingMode::PathTracedRasterPrimary;
    const RenderScale requestedScale = static_cast<RenderScale>(configuration.renderScaleIndex);

    // Identity: what preparation depends on, as RecordRenderFrame and ResizeScene derive it.
    requirements.lightingMode = mode;
    requirements.denoiser = pathTraced ? configuration.denoiser : PathTraceDenoiserMode::Off;
    requirements.psrChain = restirDI && configuration.pathTrace.primarySurfaceReplacementInPlace &&
        configuration.pathTrace.maxPathDepth > 0u ?
        (configuration.pathTrace.primarySurfaceReplacementChain >= 2u && configuration.pathTrace.maxPathDepth >= 3u ? 2u : 1u) : 0u;
    requirements.sharc = rasterPrimary && !restirPT && configuration.sharc.enabled;
    requirements.nrdReflections = !pathTraced && configuration.reflectionDenoiser == ReflectionDenoiserMode::Nrd;
    requirements.renderScaleIndex = configuration.renderScaleIndex;
    requirements.dlss = configuration.dlssRequested && IsDlssRenderScale(requestedScale);
    requirements.outputWidth = outputWidth;
    requirements.outputHeight = outputHeight;

    auto& dependency = readiness.dependencies;
    const auto at = [&](ReadinessDependency which) -> DependencyReadiness&
    {
        return dependency[static_cast<std::size_t>(which)];
    };

    for(DependencyReadiness& entry : dependency)
    {
        entry = Make(ReadinessState::NotApplicable);
    }

    // Render extent: the output extent and the effective render scale the request leads to (a DLSS scale without a DLSS
    // runtime renders Native, as ResizeScene decides). The DLSS render extent itself is NGX's answer, which this probe does
    // not query (the query writes NGX parameters); the effective scale and output extent identify it.
    const bool dlssRuntime = m_dlssBackend.GetStatus().availability == DlssAvailability::Available;
    const RenderScale targetScale = IsDlssRenderScale(requestedScale) && !dlssRuntime ? RenderScale::Native : requestedScale;
    const bool extentReady = m_outputWidth == outputWidth && m_outputHeight == outputHeight && m_renderScale == targetScale;
    at(ReadinessDependency::RenderExtent) = Make(extentReady ? ReadinessState::Ready : ReadinessState::Preparing,
        extentReady ? std::string() : "output " + std::to_string(m_outputWidth) + "x" + std::to_string(m_outputHeight) + " scale " +
        std::to_string(static_cast<int>(m_renderScale)) + ", requested " + std::to_string(outputWidth) + "x" + std::to_string(outputHeight) +
        " scale " + std::to_string(static_cast<int>(targetScale)));

    if(!pathTraced)
    {
        // Ray Tracing (Legacy): its textures at the current render extent (M0c.1/M0c.2) and, with NRD reflections, the Legacy
        // REBLUR pair (created on the first Legacy denoise; NRD retention since M0c.3B).
        DependencyReadiness resources = Make(ReadinessState::Ready);

        if(!m_legacyLighting.retention.resident)
        {
            resources = Make(ReadinessState::Preparing, "Legacy textures retired; recreated on the first Legacy frame");
        }
        else if(!m_legacyLighting.IsSizedFor(m_sceneWidth, m_sceneHeight))
        {
            resources = Make(ReadinessState::Preparing, "Legacy textures at an older extent; resized on the first Legacy frame");
        }

        if(requirements.nrdReflections)
        {
            const unsigned int giWidth = (m_sceneWidth + 1u) / 2u;
            const unsigned int giHeight = (m_sceneHeight + 1u) / 2u;
            Combine(resources, ObserveNrdInstance(m_nrdReflectionDenoiser, m_hybridNrdCreationFailed, m_sceneWidth, m_sceneHeight,
                "Legacy reflection REBLUR"));
            Combine(resources, ObserveNrdInstance(m_nrdGIDenoiser, m_hybridNrdCreationFailed, giWidth, giHeight, "Legacy GI REBLUR"));
        }

        at(ReadinessDependency::RayTracingResources) = resources;
    }
    else
    {
        // Availability: DXR, material tables and the path tracer's own (sticky) status.
        if(!m_graphicsDevice.SupportsRayTracing() || !m_graphicsDevice.GetRayTracingDevice())
        {
            at(ReadinessDependency::PathTracingAvailability) = Make(ReadinessState::Unsupported, "no DXR device");
        }
        else if(!m_materialTables.IsAvailable())
        {
            at(ReadinessDependency::PathTracingAvailability) = Make(ReadinessState::Unsupported, "no material tables");
        }
        else if(m_pathTraceStatus.availability == PathTraceReferenceAvailability::Unavailable)
        {
            at(ReadinessDependency::PathTracingAvailability) = Make(ReadinessState::Failed, m_pathTraceStatus.unavailableReason);
        }
        else
        {
            at(ReadinessDependency::PathTracingAvailability) = Make(ReadinessState::Ready);
        }

        // DXR requirements, attributed per feature by set difference: the mode's own, then what the denoiser, PSR and SHARC add.
        const PathTraceLibraryNeeds baseNeeds = GetPathTraceLibraryNeeds(mode, PathTraceDenoiserMode::Off, false);
        const PathTraceLibraryNeeds denoiserNeeds = GetPathTraceLibraryNeeds(mode, requirements.denoiser, false);
        const PathTraceLibraryNeeds sharcNeeds = GetPathTraceLibraryNeeds(mode, requirements.denoiser, true);
        PathTraceLibraryNeeds psrNeeds = GetPathTraceLibraryNeeds(mode, requirements.denoiser, requirements.sharc);
        psrNeeds.psrChain = requirements.psrChain;

        const auto baseLibraries = m_pathTraceReferencePass.GetRequiredLibraries(baseNeeds);
        const auto basePipelines = m_pathTraceReferencePass.GetRequiredPipelines(baseNeeds);
        const auto denoiserLibraries = m_pathTraceReferencePass.GetRequiredLibraries(denoiserNeeds);
        const auto denoiserPipelines = m_pathTraceReferencePass.GetRequiredPipelines(denoiserNeeds);

        DependencyReadiness pipelinesReadiness = ObserveDxrRequirements(baseLibraries, basePipelines);

        if(pipelinesReadiness.state == ReadinessState::Ready && m_pathTraceReferencePass.IsInitialized() &&
            !m_pathTraceReferencePass.IsPrimaryReady(GetLightingModePrimary(mode)))
        {
            pipelinesReadiness = Make(ReadinessState::Preparing, "primary pipeline prepared, adopted on activation");
        }

        at(ReadinessDependency::PathTracingPipelines) = pipelinesReadiness;

        // Mode resources: the pass, the PT residency (M0c.3A) and, for ReSTIR DI, the RTXDI reservoirs.
        DependencyReadiness resources = Make(ReadinessState::Ready);

        if(!m_pathTraceReferencePass.IsInitialized())
        {
            resources = Make(ReadinessState::Preparing, "path tracer created on activation");
        }
        else if(!m_pathTracingResidency.resident)
        {
            resources = Make(ReadinessState::Preparing, "path tracing resources retired; recreated on the next path-traced frame");
        }

        if(restirDI)
        {
            if(m_rtxdiResourcesFailed)
            {
                Combine(resources, Make(ReadinessState::Failed, "RTXDI resources could not be allocated"));
            }
            else if(!m_rtxdiResources.IsInitialized())
            {
                Combine(resources, Make(ReadinessState::Preparing, "RTXDI reservoirs created on activation"));
            }
        }

        // M5.1: ReSTIR PT Enhanced's pipelines, light tiles and counters (its reservoirs follow the render extent).
        if(restirPT)
        {
            if(m_restirPtPass.HasFailed())
            {
                Combine(resources, Make(ReadinessState::Failed, "ReSTIR PT: " + m_restirPtPass.GetFailure()));
            }
            else if(!m_restirPtPass.IsInitialized())
            {
                const bool prepared = m_restirPtPipelinePreparation == ReSTIRPTPipelinePreparation::Prepared;
                Combine(resources, Make(ReadinessState::Preparing, prepared ?
                    "ReSTIR PT pipelines prepared, adopted by the configuration's preparation" :
                    "ReSTIR PT pipelines preparing on their worker"));
            }
        }

        at(ReadinessDependency::PathTracingResources) = resources;

        // Denoiser: its extra DXR set, then its instance or passes at the render extent.
        if(requirements.denoiser != PathTraceDenoiserMode::Off)
        {
            at(ReadinessDependency::DenoiserPipelines) = ObserveDxrRequirements(LibrariesNotIn(denoiserLibraries, baseLibraries),
                PipelinesNotIn(denoiserPipelines, basePipelines));

            switch(requirements.denoiser)
            {
            case PathTraceDenoiserMode::Engine:
                if(m_pathTraceTemporalPass.HasInitializationFailed() || m_pathTraceSpatialPass.HasInitializationFailed())
                {
                    at(ReadinessDependency::DenoiserInstance) = Make(ReadinessState::Failed, "Engine denoiser passes failed");
                }
                else if(!m_pathTraceTemporalPass.IsInitialized() || !m_pathTraceSpatialPass.IsInitialized())
                {
                    at(ReadinessDependency::DenoiserInstance) = Make(ReadinessState::Preparing,
                        "Engine denoiser passes created on their first frame");
                }
                else
                {
                    at(ReadinessDependency::DenoiserInstance) = Make(ReadinessState::Ready);
                }
                break;
            case PathTraceDenoiserMode::NrdReblur:
                at(ReadinessDependency::DenoiserInstance) = ObserveNrdInstance(m_nrdPathTraceDenoiser, m_nrdPathTraceCreationFailed,
                    m_sceneWidth, m_sceneHeight, "REBLUR");
                break;
            case PathTraceDenoiserMode::NrdRelax:
                at(ReadinessDependency::DenoiserInstance) = ObserveNrdInstance(m_nrdRelaxDenoiser, m_nrdRelaxCreationFailed,
                    m_sceneWidth, m_sceneHeight, "RELAX");
                break;
            case PathTraceDenoiserMode::NrdHybrid:
                at(ReadinessDependency::DenoiserInstance) = ObserveNrdInstance(m_nrdHybridDenoiser, m_nrdHybridCreationFailed,
                    m_sceneWidth, m_sceneHeight, "hybrid");
                break;
            case PathTraceDenoiserMode::Off:
                break;
            }
        }

        // SHARC: its training and render libraries and pipelines (its GPU buffers are created on first use; not observed).
        if(requirements.sharc)
        {
            at(ReadinessDependency::Sharc) = ObserveDxrRequirements(
                LibrariesNotIn(m_pathTraceReferencePass.GetRequiredLibraries(sharcNeeds), denoiserLibraries),
                PipelinesNotIn(m_pathTraceReferencePass.GetRequiredPipelines(sharcNeeds), denoiserPipelines));
        }

        // PSR: the auxiliary pipelines, then the PSR lighting set beyond the ordinary one.
        if(requirements.psrChain != 0)
        {
            DependencyReadiness psr = Make(ReadinessState::Ready);

            if(!PsrLightingSupportsDenoiser(requirements.denoiser))
            {
                psr = Make(ReadinessState::Unsupported, "PSR lighting requires a supported path-tracing denoiser");
            }
            else if(m_psrAuxiliaryFailed || !m_pipelineWorkerRunning)
            {
                psr = Make(ReadinessState::Failed, "PSR auxiliary pipelines unavailable");
            }
            else if(!m_psrAuxiliaryReady)
            {
                psr = Make(ReadinessState::Preparing, "PSR auxiliary pipelines preparing");
            }
            else if(requirements.psrChain == 2u && !m_psrInPlaceReplacePass.IsChainAvailable())
            {
                psr = Make(ReadinessState::Ready, "two-bounce chain unavailable: single-bounce renders");
            }

            const PathTraceLibraryNeeds ordinaryNeeds = GetPathTraceLibraryNeeds(mode, requirements.denoiser, requirements.sharc);
            Combine(psr, ObserveDxrRequirements(
                LibrariesNotIn(m_pathTraceReferencePass.GetRequiredLibraries(psrNeeds),
                    m_pathTraceReferencePass.GetRequiredLibraries(ordinaryNeeds)),
                PipelinesNotIn(m_pathTraceReferencePass.GetRequiredPipelines(psrNeeds),
                    m_pathTraceReferencePass.GetRequiredPipelines(ordinaryNeeds))));
            at(ReadinessDependency::Psr) = psr;
        }

        // DLSS: needs a denoiser (IsDlssConfigurationEligible) and the runtime; ready when its feature exists for exactly the
        // current render and output extents (created on the first eligible frame).
        if(requirements.dlss)
        {
            const DlssFeatureStatus& feature = m_dlssBackend.GetFeatureStatus();

            if(requirements.denoiser == PathTraceDenoiserMode::Off || !rasterPrimary)
            {
                at(ReadinessDependency::Dlss) = Make(ReadinessState::Unsupported, "DLSS needs a raster-primary path-tracing denoiser");
            }
            else if(!dlssRuntime)
            {
                at(ReadinessDependency::Dlss) = Make(ReadinessState::Unsupported, m_dlssBackend.GetStatus().detail);
            }
            else if(!m_dlssFailure.empty())
            {
                at(ReadinessDependency::Dlss) = Make(ReadinessState::Failed, m_dlssFailure);
            }
            else if(feature.active && feature.renderWidth == m_sceneWidth && feature.renderHeight == m_sceneHeight &&
                feature.outputWidth == outputWidth && feature.outputHeight == outputHeight)
            {
                at(ReadinessDependency::Dlss) = Make(ReadinessState::Ready);
            }
            else
            {
                at(ReadinessDependency::Dlss) = Make(ReadinessState::Preparing, "feature created on the first eligible frame");
            }
        }
    }

    readiness.overall = ReadinessState::Ready;

    for(const DependencyReadiness& entry : dependency)
    {
        if(Severity(entry.state) > Severity(readiness.overall))
        {
            readiness.overall = entry.state;
        }
    }

    return readiness;
}

RenderedConfiguration Renderer::GetRenderedConfiguration() const
{
    RenderedConfiguration rendered;
    rendered.lightingMode = m_lightingModeTransition.GetStatus().active;
    const bool pathTraced = rendered.lightingMode != RendererLightingMode::Legacy;
    rendered.denoiser = pathTraced ? m_pathTraceDenoiserMode : PathTraceDenoiserMode::Off;
    rendered.psrChain = pathTraced && m_psrLightingLastFrame ? m_psrActiveChain : 0u;
    rendered.lit = m_litLastFrame;
    rendered.sharc = pathTraced && m_sharcRenderedLastFrame;
    rendered.dlss = m_dlssActiveLastFrame;
    rendered.renderWidth = m_sceneWidth;
    rendered.renderHeight = m_sceneHeight;
    rendered.outputWidth = m_outputWidth;
    rendered.outputHeight = m_outputHeight;
    return rendered;
}
