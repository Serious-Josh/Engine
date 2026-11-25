#include "Renderer.h"

#include "Core/Log.h"
#include "Renderer/FrameLighting.h"
#include "Renderer/SkyAtmosphereModel.h"
#include "Scene/PhotometricUnits.h"

#include <algorithm>

// Physical sky S1 (Documentation/Rendering/Physical_Sky_Design_Proposal.md §4.1-4.5): the renderer's side of SkyAtmosphere. Which
// environment a frame reads, and when the sky re-bakes. Ownership and publication are described in SkyAtmosphere.h.

EnvironmentMap& Renderer::ActiveEnvironment() noexcept
{
    return m_skyEnvironmentThisFrame ? m_skyAtmosphere.GetPublishedEnvironment() : m_environment;
}

Texture& Renderer::FrameReflectionCube() noexcept
{
    return m_frameHdriReflectionCube ? *m_frameHdriReflectionCube : ActiveEnvironment().GetCubemap();
}

std::uint64_t Renderer::RequestSkyHdri(SkyHdriSource source)
{
    const std::uint64_t ticket = ++m_skyHdriLatestTicket;
    m_skyHdriRequested = true;
    m_skyHdriError.clear();
    const std::string known = source.identity.empty() ? std::string() :
        SkyHdriIdentity(source.identity, source.radianceScale, source.rotationDegrees);

    if(!known.empty() && m_skyHdri && m_skyHdri->identity == known)
    {
        // Already prepared: anything in flight is older intent.
        if(m_skyHdriJob)
        {
            m_skyHdriJob->Cancel();
            m_skyHdriDrainingJobs.push_back(std::move(m_skyHdriJob));
        }

        m_skyHdriSnapExposure = m_skyHdriSnapExposure || source.snapExposure;
        m_skyHdriState = EnvironmentPreparationState::Pending;
        m_skyHdriCurrent = true;
        ResolveSkyHdriPublication();
        return ticket;
    }

    if(!known.empty() && m_skyHdriJob && m_skyHdriJob->requestedIdentity == known)
    {
        // The same HDRI, calibration and rotation are already preparing: the request joins it.
        m_skyHdriSnapExposure = m_skyHdriSnapExposure || source.snapExposure;
        m_skyHdriState = EnvironmentPreparationState::Pending;
        return ticket;
    }

    if(m_skyHdriJob)
    {
        m_skyHdriJob->Cancel();
        m_skyHdriDrainingJobs.push_back(std::move(m_skyHdriJob));
    }

    m_skyHdriSnapExposure = source.snapExposure;
    m_skyHdriCurrent = false;
    const unsigned int background = m_skyAtmosphere.GetHdriBackgroundWidth();
    m_skyHdriJob = std::make_unique<SkyHdriJob>(ticket, std::move(source), m_skyAtmosphere.GetEnvironmentWidth(), background);
    m_skyHdriJob->Start();
    m_skyHdriState = EnvironmentPreparationState::Pending;
    return ticket;
}

void Renderer::ClearSkyHdri()
{
    if(m_skyHdriJob)
    {
        m_skyHdriJob->Cancel();
        m_skyHdriDrainingJobs.push_back(std::move(m_skyHdriJob));
    }

    m_skyHdriRequested = false;
    m_skyHdriCurrent = false;
    m_skyHdriSnapExposure = false;
    m_skyHdri.reset();
    m_skyHdriError.clear();
    m_skyHdriState = EnvironmentPreparationState::Unknown;
}

Renderer::EnvironmentPreparationState Renderer::QuerySkyHdri(std::uint64_t ticket, std::string* error) const
{
    if(ticket == 0 || ticket > m_skyHdriLatestTicket)
    {
        return EnvironmentPreparationState::Unknown;
    }

    if(ticket < m_skyHdriLatestTicket || !m_skyHdriRequested)
    {
        return EnvironmentPreparationState::Superseded;
    }

    if(error && m_skyHdriState == EnvironmentPreparationState::Failed)
    {
        *error = m_skyHdriError;
    }

    return m_skyHdriState;
}

void Renderer::UpdateSkyHdri()
{
    // Superseded workers are released once they have finished on their own.
    std::erase_if(m_skyHdriDrainingJobs, [](const std::unique_ptr<SkyHdriJob>& job) { return job->IsDone(); });

    if(!m_skyHdriJob || !m_skyHdriJob->IsDone())
    {
        return;
    }

    std::string error;
    std::shared_ptr<const SkyHdriTexels> prepared = m_skyHdriJob->TakeResult(error);
    const double latency =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_skyHdriJob->requested).count();
    const std::string displayName = m_skyHdriJob->source.displayName;
    m_skyHdriJob.reset();

    if(!prepared)
    {
        // The sky stops showing an HDRI it cannot have: those frames light from the S0.5 environment.
        m_skyHdri.reset();
        m_skyHdriError = error.empty() ? "HDRI sky preparation failed." : error;
        m_skyHdriState = EnvironmentPreparationState::Failed;
        Log::Error("HDRI sky preparation failed (" + displayName + "): " + m_skyHdriError);
        return;
    }

    m_skyHdri = std::move(prepared);
    m_skyHdriCurrent = true;
    Log::Info("HDRI sky prepared: " + displayName + " in " + std::to_string(latency) + " ms (" + std::to_string(m_skyHdri->width) +
        "x" + std::to_string(m_skyHdri->height) + (m_skyHdri->backgroundWidth ? ", background " +
        std::to_string(m_skyHdri->backgroundWidth) + "x" + std::to_string(m_skyHdri->backgroundHeight) : std::string()) + ").");
}

void Renderer::ResolveSkyHdriPublication()
{
    const SkyHdriTexels* published = m_skyAtmosphere.GetPublishedHdri();

    if(m_skyHdriState != EnvironmentPreparationState::Pending || !m_skyHdri || !m_skyHdriCurrent || !published ||
        published->identity != m_skyHdri->identity)
    {
        return;
    }

    m_skyHdriState = EnvironmentPreparationState::Published;
    m_exposureSnapAfterEnvironmentPublication = m_exposureSnapAfterEnvironmentPublication || m_skyHdriSnapExposure;
    m_skyHdriSnapExposure = false;
}

void Renderer::SelectFrameEnvironment(const RendererOptions& options)
{
    // S5: the sky clock (frame time, at most a quarter second per frame) the clouds' wind moves with.
    m_skyTimeSeconds += std::clamp(double(options.frameDeltaSeconds), 0.0, 0.25);

    // The sky lights the frame once it has published at least once for a Physical sky, or (S4) for an HDRI sky whose preparation
    // has not failed. Until then (the first frames after a switch), m_environment does, which the Engine has asked to hold the
    // neutral environment. Between a sky change and the new sky's publication, the set published last keeps rendering, as an
    // S0.5 environment does until its replacement is ready.
    const bool hdriSky = options.hdriSky && m_skyHdriRequested && m_skyHdriState != EnvironmentPreparationState::Failed;
    m_skyEnvironmentThisFrame = (options.physicalSky.has_value() || hdriSky) && m_skyAtmosphere.HasPublished();
    const bool background = m_skyEnvironmentThisFrame && m_hdriBackgroundForTesting;
    m_frameHdriBackground = background ? m_skyAtmosphere.GetHdriBackground() : nullptr;
    m_frameHdriReflectionCube = background ? m_skyAtmosphere.GetHdriReflectionCube() : nullptr;
    // The accumulation key follows re-bakes and the background slot's completion (camera rays then see the slot).
    m_frameSkyGeneration = m_skyEnvironmentThisFrame ? (m_skyAtmosphere.GetGeneration() << 20) +
        (m_frameHdriBackground ? (m_skyAtmosphere.GetHdriBackgroundGeneration() & 0xFFFFFu) + 1 : 0) : 0;

    // S4: what the sets show. A different HDRI, or a change between an HDRI and the physical sky, is an environment replacement.
    std::optional<std::string> skyIdentity;

    if(m_skyEnvironmentThisFrame)
    {
        const SkyHdriTexels* published = m_skyAtmosphere.GetPublishedHdri();
        skyIdentity = published ? published->identity : std::string();
    }

    const bool skyReplaced = m_skyEnvironmentThisFrame && m_skyEnvironmentLastFrame && skyIdentity != m_frameSkyIdentity;
    m_frameSkyIdentity = skyIdentity;

    if(skyReplaced)
    {
        InvalidateSceneLighting(SceneLightingInvalidation::Environment);
        ResetSceneHistory();
        m_sharcSkyInputs.reset();
        m_skyIrradianceMean.reset();
        Log::Info(skyIdentity->empty() ? "Environment: the physical sky lights the scene." :
            "Environment: the HDRI sky lights the scene (" + m_skyAtmosphere.GetPublishedHdri()->displayName + ").");
    }

    // Physical sky S2 (§4.5.3): SHARC follows a slowly changing sky by accumulation, like a turning sun; an edit that changes the
    // sky's radiance by more than its 20% threshold at once clears it, as a global lighting discontinuity does.
    if(m_skyEnvironmentThisFrame && m_skyAtmosphere.GetPublishedInputs())
    {
        const SkyAtmosphere::BakeInputs& published = *m_skyAtmosphere.GetPublishedInputs();

        if(m_sharcSkyInputs && SkyAtmosphere::RadianceChange(*m_sharcSkyInputs, published) > 0.2)
        {
            m_sharcNvidia.RequestClear();
        }

        m_sharcSkyInputs = published;
    }
    else
    {
        m_sharcSkyInputs.reset();
    }

    // Changing which environment lights the scene is an environment replacement, as an S0.5 publication is. Re-bakes of the
    // sky are parameter changes: the accumulation key, SHARC and ReSTIR PT follow them through the environment identity and the
    // sky's generation, without a full history reset.
    // S5B: each published set's irradiance-cube mean, read back once its last stage completed (a frame or two after it publishes).
    // Changing by more than 2 stops either way from the previous set's is a lighting cut, handled as a camera cut. A replaced or
    // newly selected sky has already reset, and its first mean is only recorded.
    if(const std::optional<double> mean = m_skyAtmosphere.TakeIrradianceMean(m_commandContext.GetCompletedFenceValue()))
    {
        if(m_skyEnvironmentThisFrame && m_skyEnvironmentLastFrame && m_skyIrradianceMean &&
            LightingJumpExceeds(*m_skyIrradianceMean, *mean))
        {
            ApplyLightingCut("the sky", *m_skyIrradianceMean, *mean);
        }

        m_skyIrradianceMean = mean;
    }

    if(m_skyEnvironmentThisFrame != m_skyEnvironmentLastFrame)
    {
        m_skyIrradianceMean.reset();
        InvalidateSceneLighting(SceneLightingInvalidation::Environment);
        ResetSceneHistory();
        Log::Info(!m_skyEnvironmentThisFrame ? "Environment: the scene environment (HDRI, neutral or none) lights the scene." :
            (m_skyAtmosphere.GetPublishedHdri() ? "Environment: the HDRI sky lights the scene (" +
                m_skyAtmosphere.GetPublishedHdri()->displayName + ")." : std::string("Environment: the physical sky lights the scene.")));
    }

    m_skyEnvironmentLastFrame = m_skyEnvironmentThisFrame;
}

void Renderer::ApplyLightingCut(const char* cause, double before, double after)
{
    m_lastLightingCut = {cause, before, after};
    // The camera cut's history handling. A cut also snaps the automatic exposure (its new view identity does); a lighting cut
    // snaps it through the environment publication's flag, consumed by this frame's exposure pass.
    ResetSceneHistory();
    m_exposureSnapAfterEnvironmentPublication = true;
    ++m_lightingCutCount;
}

void Renderer::RecordPhysicalSkyBake(ID3D12GraphicsCommandList* commandList, const RendererOptions& options,
    const FrameLighting& frameLighting, const Vector3& cameraPosition, unsigned int frameIndex)
{
    if(!m_skyAtmosphere.IsInitialized())
    {
        return;
    }

    // S5: first, the noise volumes (once), the cloud shadow plane and the frame information the frame's readers use.
    {
        SkyAtmosphere::FrameParameters frame;
        frame.active = m_skyEnvironmentThisFrame && options.physicalSky.has_value() && !m_skyAtmosphere.PublishedHdri();
        frame.cameraPosition = cameraPosition;
        frame.cameraAltitudeMeters = options.physicalSky ?
            SkyAtmosphereModel::CameraAltitudeMeters(*options.physicalSky, double(cameraPosition.y)) : 0.5;
        frame.timeSeconds = m_skyTimeSeconds;
        frame.moonGroundIrradiance = frameLighting.moonPresent ? frameLighting.moonGroundIrradiance : Vector3{0.0f, 0.0f, 0.0f};
        frame.moonAngularRadius = frameLighting.moonAngularRadius;
        frame.sunAngularRadius = frameLighting.sun.angularRadius;
        frame.renderScale = Photometry::RenderScale(double(frameLighting.atmosphereSun.referenceEV100));
        m_skyAtmosphere.RecordFrame(commandList, m_resourceDescriptorHeap, m_uploadBuffer, frame);
    }

    // S4: an HDRI sky bakes its prepared texels (the previous sky keeps rendering until a preparation finishes). In frames without a
    // rebuild stage, the background slot builds a slice.
    if(!options.physicalSky)
    {
        if(options.hdriSky && m_skyHdriRequested && m_skyHdri)
        {
            SkyAtmosphere::BakeInputs inputs;
            inputs.hdri = m_skyHdri;

            if(m_skyAtmosphere.NeedsBake(inputs))
            {
                m_skyAtmosphere.RecordBake(commandList, m_resourceDescriptorHeap, m_uploadBuffer, m_brdfIntegrationLUT, inputs,
                    frameIndex);
            }
        }

        m_skyAtmosphere.RecordHdriBackground(commandList, m_resourceDescriptorHeap, frameIndex);
        return;
    }

    if(options.physicalSkyUpdateRequest)
    {
        m_skyAtmosphere.RequestUpdate();
    }

    SkyAtmosphere::BakeInputs inputs;
    inputs.settings = *options.physicalSky;
    inputs.sunDirection = frameLighting.atmosphereSun.direction;
    inputs.sunIrradiance = frameLighting.atmosphereSun.irradiance;
    inputs.referenceEV100 = frameLighting.atmosphereSun.referenceEV100;
    // S5: the moon (zero without one) and the sky clock.
    inputs.moonDirection = frameLighting.atmosphereMoon.direction;
    inputs.moonIrradiance = frameLighting.moonPresent ? frameLighting.atmosphereMoon.irradiance : Vector3{0.0f, 0.0f, 0.0f};
    inputs.cloudTimeSeconds = m_skyTimeSeconds;
    inputs.cameraWorldX = cameraPosition.x;
    inputs.cameraWorldZ = cameraPosition.z;
    inputs.cameraAltitudeMeters = SkyAtmosphereModel::CameraAltitudeMeters(inputs.settings, double(cameraPosition.y));

    if(!m_skyAtmosphere.NeedsBake(inputs))
    {
        return;
    }

    m_skyAtmosphere.RecordBake(commandList, m_resourceDescriptorHeap, m_uploadBuffer, m_brdfIntegrationLUT, inputs, frameIndex);
}
