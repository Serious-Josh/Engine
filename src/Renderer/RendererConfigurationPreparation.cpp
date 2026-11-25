// M0c.4 Stage 4.1/4.2: whole-configuration preparation and atomic switching (see ConfigurationPreparation.h).
//
// The presented configuration is what Engine applies (render scale, options) each frame. A request with a different
// identity is prepared here, without blocking, by calling the preparation functions that RecordRenderFrame's gates call in
// the frame that first presents it, with the arguments they will pass:
//   - GateDenoiserRequest:  PrepareDxrLibraries(mode, denoiser, the SHARC state effective before the switch)
//   - GateSharcRequest:     PrepareDxrLibraries(mode, denoiser, true)
//   - the mode transition:  PrepareDxrLibraries(GetPathTraceLibraryNeeds(mode, denoiser, sharc))
//   - the PSR gate:         PreparePsrAuxiliary() and the PSR lighting set
// Once all of them report Ready, the switching frame passes every gate and records the whole configuration. Resources that
// are created on first use (Path Tracing's textures, RTXDI, NRD instances, the DLSS feature) are created in that frame, as
// before; they need no compilation and are not partial (that frame records all of them).

#include "Renderer.h"

#include <algorithm>
#include <bit>
#include <filesystem>
#include <memory>
#include <optional>

#include "Core/Log.h"
#include "Debug/TransitionTiming.h"

namespace
{
    PreparationStep Worse(PreparationStep current, ShaderLibraryPreparationState state)
    {
        if(current == PreparationStep::Failed || state == ShaderLibraryPreparationState::Ready)
        {
            return current;
        }

        return state == ShaderLibraryPreparationState::Failed ? PreparationStep::Failed : PreparationStep::Preparing;
    }
}

PreparationStep Renderer::PrepareConfiguration(const ConfigurationIdentity& identity, std::string& detail)
{
    TransitionTiming::Scope er1Scope("PrepareConfiguration", 0.0);
    const RendererLightingMode mode = identity.lightingMode;

    // Ray Tracing needs no compilation: its textures are created by its first frame (M0c.2).
    if(mode == RendererLightingMode::Legacy)
    {
        return PreparationStep::Ready;
    }

    if(!m_graphicsDevice.SupportsRayTracing() || !m_graphicsDevice.GetRayTracingDevice() || !m_materialTables.IsAvailable())
    {
        detail = "path tracing is unavailable on this device";
        return PreparationStep::Unsupported;
    }

#ifndef NDEBUG
    if(m_configurationPreparationFailureForTesting)
    {
        detail = "verification: injected preparation failure";
        return PreparationStep::Failed;
    }

    if(m_configurationPreparationHoldForTesting)
    {
        detail = "verification: preparation held";
        return PreparationStep::Preparing;
    }
#endif

    // Sets already ready for this request are not polled again (each poll walks its libraries and pipelines): accepted
    // libraries and adopted pipelines stay so, and the inputs of each set do not change while the request is pending.
    if(m_preparedStepsGeneration != m_configurationPreparation.GetStatus().generation)
    {
        m_preparedStepsGeneration = m_configurationPreparation.GetStatus().generation;
        m_preparedSteps = 0;
    }

    PreparationStep step = PreparationStep::Ready;
    const auto due = [&](std::uint32_t bit)
    {
        return (m_preparedSteps & bit) == 0;
    };
    const auto note = [&](const char* what, const ShaderLibraryPreparation& preparation, std::uint32_t bit)
    {
        if(preparation.state == ShaderLibraryPreparationState::Ready)
        {
            m_preparedSteps |= bit;
        }

        const PreparationStep before = step;
        step = Worse(step, preparation.state);

        if(step != before || (detail.empty() && preparation.state != ShaderLibraryPreparationState::Ready))
        {
            detail = std::string(what) + (preparation.state == ShaderLibraryPreparationState::Failed ?
                ": " + preparation.diagnostic : " preparing");
        }
    };

    // The denoiser gate (only when the denoiser changes: it accepts the effective one at once) and the SHARC gate.
    if(identity.denoiser != m_pathTraceDenoiserMode && due(1u))
    {
        note("denoiser libraries", PrepareDxrLibraries(mode, identity.denoiser, m_sharcSettings.enabled), 1u);
    }

    if(identity.sharc && due(2u))
    {
        note("SHARC libraries", PrepareDxrLibraries(mode, identity.denoiser, true), 2u);
    }

    // The mode's own set, as the transition prepares it once the denoiser and SHARC have switched.
    if(due(4u))
    {
        note("path tracing libraries", PrepareDxrLibraries(GetPathTraceLibraryNeeds(mode, identity.denoiser, identity.sharc)),
            4u);
    }

    // PSR: its auxiliary pipelines and its lighting set, as the PSR gate builds it (a two-bounce chain the replacement pass
    // cannot provide uses single bounce, as the gate does).
    if(identity.psrChain != 0 && due(8u))
    {
        const ShaderLibraryPreparation auxiliary = PreparePsrAuxiliary();
        note("PSR auxiliary pipelines", auxiliary, 0u);
        std::uint32_t chain = identity.psrChain;

        if(auxiliary.state == ShaderLibraryPreparationState::Ready && chain == 2u && !m_psrInPlaceReplacePass.IsChainAvailable())
        {
            chain = 1u;
        }

        const PathTraceLibraryNeeds modeNeeds = GetPathTraceLibraryNeeds(mode, identity.denoiser, identity.sharc);
        PathTraceLibraryNeeds needs{};
        needs.primary = PathTracePrimary::RasterReSTIRDI;
        needs.psrChain = chain;
        needs.extraction = modeNeeds.extraction;
        needs.rawDiffuseHitDistance = modeNeeds.rawDiffuseHitDistance;
        needs.radianceCache = identity.sharc;
        const ShaderLibraryPreparation lighting = PrepareDxrLibraries(needs);
        note("PSR lighting libraries", lighting, 0u);

        // The chain depends on the auxiliary pipelines, so the PSR step is complete only once both are ready.
        if(auxiliary.state == ShaderLibraryPreparationState::Ready && lighting.state == ShaderLibraryPreparationState::Ready)
        {
            m_preparedSteps |= 8u;
        }
    }

    // ReSTIR PT Enhanced (Editor-only): its compute pipelines, prepared on their own worker (usually in the background while
    // Path Tracing was presented) and adopted here, and its radiance cache's NVIDIA SHARC libraries (the cache-feedback set
    // its SHARC gate selects), so the switching frame creates no pipeline and renders the cache from its first frame.
    if(mode == RendererLightingMode::PathTracedRasterPrimaryReSTIRPT && !m_packagedRuntime)
    {
        if(due(16u))
        {
            ShaderLibraryPreparation pipelines = PrepareReSTIRPTPipelines();

            if(pipelines.state == ShaderLibraryPreparationState::Ready && !EnsureReSTIRPT())
            {
                pipelines.state = ShaderLibraryPreparationState::Failed;
                pipelines.diagnostic = m_restirPtPass.GetFailure();
            }

            note("ReSTIR PT pipelines", pipelines, 16u);
        }

        if(m_restirPtSettings.sharc && NvidiaSharcCapabilityFailure().empty() && due(32u))
        {
            PathTraceLibraryNeeds cacheNeeds = GetPathTraceLibraryNeeds(mode, identity.denoiser, true);
            cacheNeeds.radianceCacheImplementation = PathTraceRadianceCacheImplementation::Nvidia;
            note("ReSTIR PT radiance cache libraries", PrepareDxrLibraries(cacheNeeds), 32u);
        }
    }

    // Lighting Mode switch hitch: a request for Ray Reconstruction has its input-preparation pipelines built on the pipeline
    // worker, so its first frame creates none (the PSR variant alone took about a second there with a cold driver cache).
    if(RequestsRayReconstructionInputs(identity) && due(64u))
    {
        note("Ray Reconstruction input pipelines", PrepareRrInputPipelines(), 64u);
    }

    // Lighting Mode switch hitch: the NRD Hybrid instance of a request that denoises with it is created ahead of the switch
    // (about 50 ms in the switching frame otherwise), its SDK part on a thread of its own.
    if(RequestsNrdHybridInstance(identity) && due(128u))
    {
        note("NRD Hybrid instance", PrepareNrdHybridDenoiser(identity), 128u);
    }

    // M0c.5 Stage 5.5: a request for DLSS waits for NGX, initialized on its own thread from here or from InitializeProject (the
    // presented configuration keeps rendering, so enabling DLSS mid-session no longer stalls a frame). The switching frame
    // then sizes the scene once, at NGX's extent. A failed NGX is not a preparation failure: as before, DLSS is unavailable and
    // the request renders Native.
    if(identity.dlssRequested || IsDlssRenderScale(static_cast<RenderScale>(identity.renderScaleIndex)))
    {
        BeginDlssBackend(true);

        if(!PollDlssBackend())
        {
            step = Worse(step, ShaderLibraryPreparationState::Compiling);

            if(detail.empty())
            {
                detail = "DLSS (NGX) initializing";
            }
        }
    }

    // M0c.5 Stage 5.3: the first lit frame waits for the AIR and volumetrics the scene requested (prepared on the pipeline
    // worker since its first frame), so it is never presented without them. Only at startup: later, AIR enabled during the
    // session starts when ready, independently of configuration switches. Ray Tracing waits through RecordRenderFrame's hold.
    // Stage 5.5: applied after the polling above, so libraries and pipelines keep being adopted while AIR prepares.
    if(step == PreparationStep::Ready && m_configurationPreparation.GetStatus().startupPreparing &&
        m_volumetricState == VolumetricState::Preparing)
    {
        detail = "AIR and volumetrics preparing";
        step = PreparationStep::Preparing;
    }

    m_configurationPreparation.SetProgress(EstimatePreparationProgress(identity));
    return step;
}

// M0c.5 Stage 5.5: between InitializeProject's phases, before any frame: the pending request's preparation is polled exactly
// as a frame polls it (finished libraries are accepted and their DXR pipelines queued at once, rather than at the first
// frame), but nothing is presented or switched, so the M0c.4 presentation state stays the frames' to change.
void Renderer::AdvanceConfigurationPreparation()
{
    const ConfigurationPreparationStatus& status = m_configurationPreparation.GetStatus();

    if(!status.pending || m_activeRecordingGeneration != 0)
    {
        return;
    }

    const TransitionTiming::Scope advanceTiming("AdvanceConfigurationPreparation", 0.1, TransitionTiming::Startup);
    std::string detail;
    PrepareConfiguration(*status.pending, detail);
}

// Progress for the preparation presentation (Stage 4.3): the request's DXR libraries already accepted, and its preparation
// sets already ready (each set's pipelines follow its libraries). An estimate only; readiness is decided by the steps.
float Renderer::EstimatePreparationProgress(const ConfigurationIdentity& identity) const
{
    PathTraceLibraryNeeds needs = GetPathTraceLibraryNeeds(identity.lightingMode, identity.denoiser, identity.sharc);
    needs.psrChain = identity.psrChain;
    const std::vector<PathTraceLibraryRequirement> libraries = m_pathTraceReferencePass.GetRequiredLibraries(needs);
    std::size_t acceptedLibraries = 0;

    for(const PathTraceLibraryRequirement& library : libraries)
    {
        if(std::find(m_preparedDxrLibraries.begin(), m_preparedDxrLibraries.end(), library.path) != m_preparedDxrLibraries.end())
        {
            ++acceptedLibraries;
        }
    }

    std::uint32_t expectedSets = 4u;
    expectedSets |= identity.denoiser != m_pathTraceDenoiserMode ? 1u : 0u;
    expectedSets |= identity.sharc ? 2u : 0u;
    expectedSets |= identity.psrChain != 0 ? 8u : 0u;

    // ReSTIR PT Enhanced's pipelines and, with its radiance cache on, the cache's libraries (PrepareConfiguration).
    if(identity.lightingMode == RendererLightingMode::PathTracedRasterPrimaryReSTIRPT && !m_packagedRuntime)
    {
        const bool nvidiaSharcUsable = m_sharcNvidiaCapabilityFailure && m_sharcNvidiaCapabilityFailure->empty();
        expectedSets |= 16u;
        expectedSets |= m_restirPtSettings.sharc && nvidiaSharcUsable ? 32u : 0u;
    }

    expectedSets |= RequestsRayReconstructionInputs(identity) ? 64u : 0u;
    expectedSets |= RequestsNrdHybridInstance(identity) ? 128u : 0u;
    const int setCount = std::popcount(expectedSets);
    const int readySets = std::popcount(expectedSets & m_preparedSteps);
    const double units = static_cast<double>(libraries.size() + static_cast<std::size_t>(setCount));
    return units > 0.0 ? static_cast<float>((acceptedLibraries + static_cast<std::size_t>(readySets)) / units) : 1.0f;
}

const RenderingConfiguration& Renderer::SelectPresentedConfiguration(const RenderingConfiguration& requested)
{
    // M0c.5 Stage 5.6: a frame's DXR preparation report starts here, before this frame's preparation records into it, and is
    // read after Render (Engine: DxrPreparationTasks::Update). Render used to reset it, which erased this preparation's polls
    // (M0c.4 moved them ahead of Render): a batch then closed as "results not used" and a failure never failed its batch.
    m_dxrPreparationReport.waiting = false;
    m_dxrPreparationReport.libraryFailure.clear();
    m_dxrPreparationReport.pipelineFailure.clear();

    ConfigurationPreparation& preparation = m_configurationPreparation;
    const auto now = ConfigurationPreparation::Clock::now();

    if(!preparation.HasPresented())
    {
        preparation.PresentInitial(requested);
    }

    // ReSTIR PT Enhanced (Editor-only): its pipelines are prepared in the background once a path-traced configuration is
    // presented (its estimator is offered there) and the startup request is done, so a later switch to it finds them
    // ready. Only their worker runs (no GPU resource is created until ReSTIR PT is requested); a poll is cheap.
    const ConfigurationPreparationStatus& status = preparation.GetStatus();

    if(!m_packagedRuntime && !status.startupPreparing && status.presented.lightingMode != RendererLightingMode::Legacy)
    {
        PrepareReSTIRPTPipelines();
    }

    // The previous switching frame: if the renderer could not activate the presented mode (it fell back in that recording),
    // the configuration presented before it is restored rather than leaving a partial one presented.
    if(preparation.IsSwitchPendingFirstFrame())
    {
        preparation.FirstFrameRendered();
        const RendererLightingMode presentedMode = preparation.GetPresented().lightingMode;
        const LightingModeTransitionStatus& transition = m_lightingModeTransition.GetStatus();

        if(presentedMode != RendererLightingMode::Legacy && transition.active != presentedMode &&
            transition.stage == LightingModeTransitionStage::Failed)
        {
            preparation.ActivationFailed(transition.diagnostic, now);
        }
    }

    const ConfigurationIdentity identity = MakeConfigurationIdentity(requested);

    if(identity == preparation.GetStatus().presented)
    {
        // A background NRD Hybrid creation whose request was withdrawn is completed once it finishes, so NRD retention
        // tracks (and in time retires) the instance like any other.
        PollNrdHybridCreation(false);
        preparation.PresentSameIdentity(requested);
        return Presented();
    }

    // Not part of the identity: the request's own Ray Reconstruction selection, for its input pipelines' preparation.
    m_pendingRayReconstructionRequested = requested.rayReconstructionRequested;
    const bool newRequest = preparation.Request(identity, now);

    // A failed request keeps the presented configuration; it is attempted again after the retry interval.
    if(!newRequest && preparation.GetStatus().state == ConfigurationPreparationState::Failed && !preparation.IsRetryDue(now))
    {
        preparation.Prepared(requested, PreparationStep::Failed, preparation.GetStatus().detail, now);
        return Presented();
    }

    std::string detail;
    const PreparationStep step = PrepareConfiguration(identity, detail);
    preparation.Prepared(requested, step, detail, now);
    return Presented();
}

const RenderingConfiguration& Renderer::Presented()
{
    // The next recording renders this selection: unlit while the first request is still preparing.
    m_lightingSuppressedForNextFrame = m_configurationPreparation.GetStatus().startupPreparing;
    return m_configurationPreparation.GetPresented();
}

RenderingConfiguration Renderer::PresentConfiguration(const RenderingConfiguration& latest) const
{
    if(MakeConfigurationIdentity(latest) == m_configurationPreparation.GetStatus().presented)
    {
        return latest;
    }

    return m_configurationPreparation.GetPresented();
}

bool Renderer::IsPresentedDlssEligible() const
{
    const ConfigurationPreparationStatus& status = m_configurationPreparation.GetStatus();

    // Stage 4.4: while the first request prepares (nothing lit), its own DLSS eligibility decides the render extent, so the
    // scene is sized once for it and the switch needs no second resize. No DLSS work runs in those frames.
    if(status.startupPreparing)
    {
        return status.pending && IsRasterPrimaryReSTIRMode(status.pending->lightingMode) &&
            status.pending->denoiser != PathTraceDenoiserMode::Off;
    }

    const RenderingConfiguration& presented = m_configurationPreparation.GetPresented();

    if(!IsRasterPrimaryReSTIRMode(presented.lightingMode) ||
        presented.denoiser == PathTraceDenoiserMode::Off || !m_denoiserPreparation.empty())
    {
        return false;
    }

    // Prepared and switching this frame (its first frame applies the scale with the mode), or already rendering.
    return m_configurationPreparation.IsSwitchPendingFirstFrame() ||
        m_lightingModeTransition.GetStatus().active == presented.lightingMode;
}

bool Renderer::IsPendingConfigurationMode(bool pathTraced) const
{
    const auto& pending = m_configurationPreparation.GetStatus().pending;
    return pending && m_configurationPreparation.GetStatus().state == ConfigurationPreparationState::Preparing &&
        (pending->lightingMode != RendererLightingMode::Legacy) == pathTraced;
}

bool Renderer::IsPendingConfigurationNrdGroup(int group) const
{
    const auto& status = m_configurationPreparation.GetStatus();

    if(!status.pending || status.state != ConfigurationPreparationState::Preparing)
    {
        return false;
    }

    const PathTraceDenoiserMode denoiser = status.pending->denoiser;

    switch(static_cast<NrdRetentionGroup>(group))
    {
    case NrdRetentionGroup::Relax:
        return denoiser == PathTraceDenoiserMode::NrdRelax;
    case NrdRetentionGroup::RawDiffuseHitDistance:
        return denoiser == PathTraceDenoiserMode::NrdRelax || denoiser == PathTraceDenoiserMode::NrdHybrid;
    case NrdRetentionGroup::PathTraceReblur:
        return denoiser == PathTraceDenoiserMode::NrdReblur;
    case NrdRetentionGroup::Hybrid:
        return denoiser == PathTraceDenoiserMode::NrdHybrid;
    case NrdRetentionGroup::EngineDenoiser:
        return denoiser == PathTraceDenoiserMode::Engine;
    case NrdRetentionGroup::Legacy:
        return status.pending->lightingMode == RendererLightingMode::Legacy;
    case NrdRetentionGroup::Count:
        break;
    }

    return false;
}

void Renderer::BeginConfigurationPreparation(const RenderingConfiguration& requested)
{
    // Stage 4.4: the same selection a frame makes, made as soon as the configuration is known (before the first frame),
    // so its libraries and pipelines start compiling while the project and scene initialize.
    SelectPresentedConfiguration(requested);

    if(requested.lightingMode != RendererLightingMode::Legacy)
    {
        ReleaseUnusedLegacyTextures();
    }

    Log::Info("Configuration preparation started before the first frame: " +
        DescribeConfigurationIdentity(MakeConfigurationIdentity(requested)) + ".");
}

bool Renderer::IsLightingSuppressed() const noexcept
{
    return m_configurationPreparation.GetStatus().startupPreparing;
}

void Renderer::ReleaseUnusedLegacyTextures()
{
    LegacyLightingResources::Retention& retention = m_legacyLighting.retention;

    // Never recorded or submitted: no GPU work references them, so their descriptors may return to the heap at once.
    if(!retention.resident || retention.lastUseFence != 0 || retention.submissionUncertain || retention.usedThisRecording)
    {
        return;
    }

    m_legacyLighting.ReleaseTextures();
    retention.resident = false;
    Log::Info("Released the unused Legacy lighting textures: the starting configuration does not render Legacy.");
}

// Ray Reconstruction (Editor only) denoises the raster-primary ReSTIR modes' extraction, so only their requests with a
// denoiser and the RR selection prepare its input pipelines.
bool Renderer::RequestsRayReconstructionInputs(const ConfigurationIdentity& identity) const
{
    return m_pendingRayReconstructionRequested && !m_packagedRuntime && IsRasterPrimaryReSTIRMode(identity.lightingMode) &&
        identity.denoiser != PathTraceDenoiserMode::Off;
}

// The raster-primary ReSTIR modes denoise with the NRD Hybrid instance unless Ray Reconstruction replaces it (RR frames
// never run NRD).
bool Renderer::RequestsNrdHybridInstance(const ConfigurationIdentity& identity) const
{
    return IsRasterPrimaryReSTIRMode(identity.lightingMode) && identity.denoiser == PathTraceDenoiserMode::NrdHybrid &&
        !RequestsRayReconstructionInputs(identity);
}

// Lighting Mode switch hitch: the Ray Reconstruction input pipelines, built on the pipeline worker (with its own
// ShaderLibrary) and adopted here on the renderer thread, so the first RR frame creates none. Ready as well when they
// cannot be prepared (no worker, or the job failed): the first RR frame then creates them as before and reports a failure
// itself, so this step never fails a configuration.
ShaderLibraryPreparation Renderer::PrepareRrInputPipelines()
{
    ShaderLibraryPreparation result{};
    result.state = ShaderLibraryPreparationState::Ready;

    if(m_rrPipelinePreparationDone || m_rrInputPreparationPass.HasPipelines() || !m_pipelineWorkerRunning ||
        !m_pipelineWorker || m_shaderLibrary.IsPackaged())
    {
        return result;
    }

    const std::string key = "Ray Reconstruction input pipelines";
    const RayTracingPipelineJobState state = m_pipelineWorker->GetState(key);

    if(state == RayTracingPipelineJobState::Completed)
    {
        const std::optional<RayTracingPipelineOutcome> outcome = m_pipelineWorker->TakeCompleted(key);

        // Nothing in the prepared set was ever bound; it is published only here, on the renderer thread.
        if(outcome && outcome->succeeded && outcome->device == m_graphicsDevice.GetRayTracingDevice() &&
            m_rrPreparedPipelines)
        {
            m_rrInputPreparationPass.AdoptPrepared(*m_rrPreparedPipelines);
        }

        m_rrPreparedPipelines.reset();
        m_rrPipelinePreparationDone = true;
        return result;
    }

    if(state == RayTracingPipelineJobState::None)
    {
        auto prepared = std::make_shared<RrInputPreparationPass::PreparedPipelines>();
        const std::filesystem::path shaderRoot = m_shaderLibrary.GetShaderRoot();
        // Read only by the job (free-threaded device methods); Shutdown joins the worker before the device is released.
        ID3D12Device* device = m_graphicsDevice.GetDevice();
        RayTracingPipelineRequest request{};
        request.key = key;
        request.device = m_graphicsDevice.GetRayTracingDevice();
        request.prepareAuxiliary = [prepared, shaderRoot, device]()
        {
            ShaderLibrary shaders;
            const bool ready = shaders.Initialize(shaderRoot) &&
                RrInputPreparationPass::PreparePipelines(device, shaders, *prepared);
            shaders.Shutdown();
            return ready;
        };

        if(!m_pipelineWorker->Submit(std::move(request)))
        {
            m_rrPipelinePreparationDone = true;
            return result;
        }

        m_rrPreparedPipelines = std::move(prepared);
    }

    result.state = ShaderLibraryPreparationState::PreparingPipelines;
    return result;
}
