#include "Renderer.h"
#include "Renderer/BuiltinEnvironment.h"
#include "Renderer/TextureMemoryAllocator.h"
#include "Scene/SceneDocument.h"
#include "Debug/MemoryRegistry.h"
#include "Debug/StreamingTelemetry.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

// S0.5 asynchronous scene environment preparation: request, supersession, per-frame recording, publication and
// retirement. Ownership and synchronization are described in EnvironmentPreparation.h.
namespace
{
// Defaults keep one frame's preparation work near 2 ms of GPU time on the development GPU (RTX 4080; see
// SceneStreamingS0_5_Results.md): staging bytes copied per frame, and the estimated precompute microseconds per frame.
constexpr std::uint64_t DefaultCopyBytesPerFrame = 16ull << 20;
constexpr std::uint64_t DefaultPrecomputeMicrosecondsPerFrame = 2000;

// The precompute cost model, measured on the development GPU: a draw costs its texel-samples at the sustained sampling
// rate, but never less than its per-texel loop length times the latency of one dependent iteration, which bounds the small
// mips (one wave running thousands of serial samples behind its own barrier).
constexpr double SamplesPerMicrosecond = 64000.0;
constexpr double MicrosecondsPerSerialSample = 0.25;

double EstimateDrawMicroseconds(std::uint64_t texels, std::uint64_t samplesPerTexel)
{
    return (std::max)(static_cast<double>(texels) * static_cast<double>(samplesPerTexel) / SamplesPerMicrosecond,
        static_cast<double>(samplesPerTexel) * MicrosecondsPerSerialSample);
}

std::uint64_t ReadBudget(const char* name, std::uint64_t fallback, std::uint64_t unit)
{
    char text[32]{};
    const DWORD length = GetEnvironmentVariableA(name, text, sizeof(text));

    if(length == 0 || length >= sizeof(text))
    {
        return fallback;
    }

    const std::uint64_t value = std::strtoull(text, nullptr, 10);
    return value > 0 ? value * unit : fallback;
}

// Texture samples per texel of the cubemap prefilter (EquirectangularToCubemap.hlsl): mip 0 copies the source, the terminal
// mip averages 2,048 directions, every other mip prefilters with 4,096.
std::uint64_t CubemapSamplesPerTexel(unsigned int mip, unsigned int mipLevels)
{
    if(mip == 0)
    {
        return 1;
    }

    return mip + 1 == mipLevels ? 2048 : 4096;
}

constexpr std::uint64_t DiffuseSamplesPerTexel = 4096;
constexpr std::uint64_t MultiscatterSamplesPerTexel = 1024;
}

EnvironmentPreparationSource Renderer::NeutralEnvironmentSource(float radianceScale)
{
    return {[](std::string&) { return MakeNeutralEnvironmentImage(); }, "neutral", "Neutral", {}, radianceScale};
}

EnvironmentPreparationSource Renderer::NoneEnvironmentSource(float radianceScale)
{
    return {[](std::string&) { return MakeBlackEnvironmentImage(); }, SceneDocument::NoneEnvironmentReference, "None",
        SceneDocument::NoneEnvironmentReference, radianceScale};
}

std::uint64_t Renderer::RequestEnvironment(EnvironmentPreparationSource source)
{
    StreamingTelemetry::Add(StreamingTelemetry::Metric::EnvironmentRequests, 1);
    const std::uint64_t ticket = ++m_environmentLatestTicket;
    m_environmentError.clear();
    const std::string known = source.identity.empty() ? std::string() :
        CalibratedEnvironmentIdentity(source.identity, source.radianceScale);

    if(!known.empty() && known == m_environmentIdentity)
    {
        // Already published: anything in flight is older intent.
        SupersedeEnvironmentJob();
        m_environmentQueuedSource.reset();
        m_environmentState = EnvironmentPreparationState::Published;
        return ticket;
    }

    if(!known.empty() && m_environmentJob && m_environmentJob->requestedIdentity == known)
    {
        // The same source and calibration are already preparing: the request joins it.
        m_environmentJob->ticket = ticket;
        m_environmentJob->source.snapExposure = m_environmentJob->source.snapExposure || source.snapExposure;
        m_environmentQueuedSource.reset();
        m_environmentState = EnvironmentPreparationState::Pending;
        return ticket;
    }

    SupersedeEnvironmentJob();
    m_environmentState = EnvironmentPreparationState::Pending;

    if(!m_environmentDrainingJobs.empty())
    {
        // Bound concurrency: one superseded worker drains at a time; the newest waiting request replaces older ones.
        m_environmentQueuedSource = std::move(source);
        m_environmentQueuedTicket = ticket;
        return ticket;
    }

    m_environmentQueuedSource.reset();
    StartEnvironmentJob(std::move(source), ticket);
    return ticket;
}

Renderer::EnvironmentPreparationState Renderer::QueryEnvironmentPreparation(std::uint64_t ticket, std::string* error) const
{
    if(ticket == 0 || ticket > m_environmentLatestTicket)
    {
        return EnvironmentPreparationState::Unknown;
    }

    if(ticket < m_environmentLatestTicket)
    {
        return EnvironmentPreparationState::Superseded;
    }

    if(error && m_environmentState == EnvironmentPreparationState::Failed)
    {
        *error = m_environmentError;
    }

    return m_environmentState;
}

void Renderer::CancelEnvironmentPreparation()
{
    SupersedeEnvironmentJob();
    m_environmentQueuedSource.reset();

    if(m_environmentState == EnvironmentPreparationState::Pending)
    {
        m_environmentState = EnvironmentPreparationState::Superseded;
    }
}

void Renderer::StartEnvironmentJob(EnvironmentPreparationSource source, std::uint64_t ticket)
{
    auto job = std::make_unique<EnvironmentPreparationJob>(ticket, std::move(source), m_environmentIdentity);
    job->Start(m_graphicsDevice.GetDevice());
    m_environmentJob = std::move(job);
}

void Renderer::SupersedeEnvironmentJob()
{
    if(!m_environmentJob)
    {
        return;
    }

    StreamingTelemetry::Add(StreamingTelemetry::Metric::EnvironmentSuperseded, 1);
    m_environmentJob->Cancel();

    if(m_environmentJob->stage == EnvironmentPreparationStage::Cpu && !m_environmentJob->IsCpuDone())
    {
        m_environmentDrainingJobs.push_back(std::move(m_environmentJob));
        return;
    }

    RetireEnvironmentJob(std::move(m_environmentJob));
}

void Renderer::RetireEnvironmentJob(std::unique_ptr<EnvironmentPreparationJob> job)
{
    if(!job)
    {
        return;
    }

    // Commands recorded into a frame that never confirmed submission may still execute: keep the owners until shutdown's
    // drain. Otherwise the last confirmed submission is the last GPU reader (0: never submitted).
    const std::uint64_t fence = job->submissionUncertain || job->recordedUncommitted ? UINT64_MAX : job->lastSubmissionFence;
    m_environmentRetirements.push_back({fence, std::move(job)});
}

void Renderer::FailEnvironmentJob(const std::string& error)
{
    Log::Error("Scene environment preparation failed: " + error);
    RetireEnvironmentJob(std::move(m_environmentJob));
    m_environmentState = EnvironmentPreparationState::Failed;
    m_environmentError = error;
}

void Renderer::CollectEnvironmentPreparation()
{
    const std::uint64_t completed = m_commandContext.GetCompletedFenceValue();
    const auto released = std::remove_if(m_environmentRetirements.begin(), m_environmentRetirements.end(),
        [&](const EnvironmentRetirement& retirement) { return retirement.fence <= completed; });

    if(released != m_environmentRetirements.end())
    {
        StreamingTelemetry::Add(StreamingTelemetry::Metric::EnvironmentRetirementReleases,
            static_cast<std::uint64_t>(m_environmentRetirements.end() - released));
        m_environmentRetirements.erase(released, m_environmentRetirements.end());
    }

    for(auto& draining : m_environmentDrainingJobs)
    {
        if(draining->IsCpuDone())
        {
            // A cancelled worker's staging buffer was never referenced by the GPU.
            draining.reset();
        }
    }

    std::erase(m_environmentDrainingJobs, nullptr);

    if(m_environmentDrainingJobs.empty() && m_environmentQueuedSource && !m_environmentJob)
    {
        auto source = std::move(*m_environmentQueuedSource);
        m_environmentQueuedSource.reset();
        StartEnvironmentJob(std::move(source), m_environmentQueuedTicket);
    }
}

bool Renderer::BeginEnvironmentGpuStage(EnvironmentPreparationJob& job)
{
    auto& result = job.TakeCpuResult();

    if(!result.error.empty())
    {
        FailEnvironmentJob(result.error);
        return false;
    }

    if(result.unchanged)
    {
        // Same identity as the published environment (a reload): nothing to replace, as SetEnvironmentImage returns early.
        RetireEnvironmentJob(std::move(m_environmentJob));
        m_environmentState = EnvironmentPreparationState::Published;
        return false;
    }

    job.calibratedIdentity = CalibratedEnvironmentIdentity(result.sourceIdentity, job.source.radianceScale);
    MemoryRegistry::TrackResource(result.staging.Get(), MemoryCategory::UploadReadback);

    const StreamingTelemetry::Scope createTiming(StreamingTelemetry::Metric::EnvironmentCreateNs);
    const MemoryCategoryScope memoryScope(MemoryCategory::MeshesTextures);
    ID3D12Device* device = m_graphicsDevice.GetDevice();
    // The worker created the resources; views, registry entries and the passes' render-target views are main-thread work.
    job.environment = std::move(result.environment);
    EnvironmentMap& environment = *job.environment;

    if(!environment.CreateViewsForUpload(device, m_resourceDescriptorHeap) ||
        !job.mipPass.Initialize(device, m_shaderLibrary, m_rtvDescriptorHeap, environment.GetSourceTexture()) ||
        !job.cubemapPass.Initialize(device, m_shaderLibrary, m_rtvDescriptorHeap, environment.GetCubemap()) ||
        !job.diffusePass.Initialize(device, m_shaderLibrary, m_rtvDescriptorHeap, environment.GetDiffuseIrradiance()) ||
        !job.multiscatterPass.Initialize(device, m_shaderLibrary, m_rtvDescriptorHeap, environment.GetGGXMultiscatterIrradiance()))
    {
        // Nothing was recorded: the owners have no GPU reader.
        FailEnvironmentJob("Environment resources or precompute passes could not be created.");
        return false;
    }

    job.stage = EnvironmentPreparationStage::Copy;
    Log::Info("HDR environment prepared off-thread: " + std::to_string(result.width) + "x" + std::to_string(result.height) +
        ", total weight = " + std::to_string(result.totalWeight) + ", worker " +
        std::to_string(static_cast<double>(result.workerNanoseconds) / 1e6) + " ms");
    return true;
}

void Renderer::PublishPreparedEnvironment()
{
    CollectEnvironmentPreparation();

    if(!m_environmentJob || m_activeRecordingGeneration)
    {
        return;
    }

    EnvironmentPreparationJob& job = *m_environmentJob;

    if(job.recordedUncommitted)
    {
        // The previous frame recorded preparation commands but did not confirm their submission. Texture state tracking
        // advanced with the discarded commands, so restart from the source once.
        auto source = job.source;
        const std::uint64_t ticket = job.ticket;
        const std::uint32_t attempts = job.attempts;
        RetireEnvironmentJob(std::move(m_environmentJob));

        if(attempts >= 2)
        {
            FailEnvironmentJob("Environment preparation commands were not submitted.");
            return;
        }

        Log::Warning("Environment preparation frame was not submitted; restarting the preparation.");
        StartEnvironmentJob(std::move(source), ticket);
        m_environmentJob->attempts = attempts + 1;
        return;
    }

    if(job.stage == EnvironmentPreparationStage::Cpu)
    {
        if(job.IsCpuDone())
        {
            BeginEnvironmentGpuStage(job);
        }

        return;
    }

    if(job.stage != EnvironmentPreparationStage::Recorded || job.lastSubmissionFence == 0)
    {
        return;
    }

    // Every preparation command was submitted on this queue, which orders it before this frame. The environment the
    // previous frames rendered retires behind the last submission, with the passes and the staging buffer.
    const StreamingTelemetry::Scope publishTiming(StreamingTelemetry::Metric::EnvironmentPublishNs,
        StreamingTelemetry::Metric::EnvironmentPublications);
    m_environment.Swap(*job.environment);
    job.lastSubmissionFence = (std::max)(job.lastSubmissionFence, m_commandContext.GetLastSubmissionFenceValue());
    m_environmentIdentity = job.calibratedIdentity;
    const auto& source = job.source;
    auto entry = std::find_if(m_availableEnvironments.begin(), m_availableEnvironments.end(), [&](const auto& e)
        { return e.relativePath.empty() && e.reference == source.reference && e.displayName == source.displayName; });

    if(entry == m_availableEnvironments.end())
    {
        m_availableEnvironments.push_back({source.displayName, {}, source.reference});
        m_currentEnvironmentIndex = m_availableEnvironments.size() - 1;
    }
    else
    {
        m_currentEnvironmentIndex = static_cast<size_t>(entry - m_availableEnvironments.begin());
    }

    InvalidateSceneLighting(SceneLightingInvalidation::Environment);
    ResetSceneHistory();
    m_exposureSnapAfterEnvironmentPublication = m_exposureSnapAfterEnvironmentPublication || source.snapExposure;
    const double latency = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - job.requested).count();
    Log::Info("Environment changed to: " + source.displayName + " (prepared asynchronously in " + std::to_string(latency) +
        " ms)");
    RetireEnvironmentJob(std::move(m_environmentJob));
    m_environmentState = EnvironmentPreparationState::Published;
    ++m_environmentPublications;
}

void Renderer::RecordEnvironmentPreparation(ID3D12GraphicsCommandList* commandList)
{
    if(!m_environmentJob || !commandList)
    {
        return;
    }

    EnvironmentPreparationJob& job = *m_environmentJob;

    if(job.stage == EnvironmentPreparationStage::Cpu || job.stage == EnvironmentPreparationStage::Recorded)
    {
        return;
    }

    if(m_environmentCopyBudget == 0)
    {
        m_environmentCopyBudget = ReadBudget("ENGINE_ENVIRONMENT_PREP_COPY_MIB", DefaultCopyBytesPerFrame, 1ull << 20);
        m_environmentWorkBudget = ReadBudget("ENGINE_ENVIRONMENT_PREP_GPU_US", DefaultPrecomputeMicrosecondsPerFrame, 1);
    }

    const StreamingTelemetry::Scope recordTiming(StreamingTelemetry::Metric::EnvironmentRecordNs,
        StreamingTelemetry::Metric::EnvironmentRecordFrames);
    auto& staged = job.TakeCpuResult();
    EnvironmentMap& environment = *job.environment;
    ID3D12DescriptorHeap* descriptorHeaps[] = {m_resourceDescriptorHeap.GetHeap()};
    commandList->SetDescriptorHeaps(1, descriptorHeaps);
    job.recordedUncommitted = true;

    if(job.stage == EnvironmentPreparationStage::Copy)
    {
        // Placed environment textures start undefined: activate them before the first copy or draw
        // (TextureMemoryAllocator.h; each is activated once).
        for(Texture* texture : {&environment.GetSourceTexture(), &environment.GetCubemap(),
                &environment.GetDiffuseIrradiance(), &environment.GetGGXMultiscatterIrradiance()})
        {
            TextureMemoryAllocator::RecordActivation(commandList, texture->GetResource(), texture->GetSubresourceState(0));
        }

        // Mip 0 rows, then the CDFs, at most the copy budget per frame. The texture is in COPY_DEST since creation; the
        // COMMON buffers promote to COPY_DEST and decay back to COMMON when this frame's submission completes.
        std::uint64_t copied = 0;
        const auto& footprint = staged.textureFootprint;
        const std::uint64_t rowPitch = footprint.Footprint.RowPitch;

        while(job.copyRow < staged.textureRows && copied < m_environmentCopyBudget)
        {
            const unsigned int rows = static_cast<unsigned int>((std::min)(
                static_cast<std::uint64_t>(staged.textureRows - job.copyRow),
                (std::max)(std::uint64_t{1}, (m_environmentCopyBudget - copied) / rowPitch)));
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = staged.staging.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprint;
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = environment.GetSourceTexture().GetResource();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = 0;
            const D3D12_BOX box{0, job.copyRow, 0, footprint.Footprint.Width, job.copyRow + rows, 1};
            commandList->CopyTextureRegion(&destination, 0, job.copyRow, 0, &source, &box);
            job.copyRow += rows;
            copied += rows * rowPitch;
        }

        const auto copyBuffer = [&](ID3D12Resource* destination, std::uint64_t& cursor, std::uint64_t offset, std::uint64_t size)
        {
            if(cursor >= size || copied >= m_environmentCopyBudget)
            {
                return;
            }

            const std::uint64_t bytes = (std::min)(size - cursor, m_environmentCopyBudget - copied);
            commandList->CopyBufferRegion(destination, cursor, staged.staging.Get(), offset + cursor, bytes);
            cursor += bytes;
            copied += bytes;
        };

        copyBuffer(environment.GetImportanceConditionalCDF().GetResource(), job.copyConditional, staged.conditionalOffset,
            staged.conditionalBytes);
        copyBuffer(environment.GetImportanceMarginalCDF().GetResource(), job.copyMarginal, staged.marginalOffset,
            staged.marginalBytes);
        StreamingTelemetry::Add(StreamingTelemetry::Metric::EnvironmentCopyBytes, copied);

        if(job.copyRow == staged.textureRows && job.copyConditional == staged.conditionalBytes &&
            job.copyMarginal == staged.marginalBytes)
        {
            job.stage = EnvironmentPreparationStage::MipChain;
            job.copiesFinishedUncommitted = true;
        }

        return;
    }

    // Precompute: the passes PreprocessEnvironment executes, in its order of dependencies (mips first; the other three read
    // only the source), split into per-target draws and row bands of the large targets so one frame records about the GPU
    // time budget (EstimateDrawMicroseconds).
    double used = 0.0;
    const double budget = static_cast<double>(m_environmentWorkBudget);
    const auto rowsWithin = [&](unsigned int size, unsigned int remaining, std::uint64_t samples)
    {
        const double perRow = static_cast<double>(size) * static_cast<double>(samples) / SamplesPerMicrosecond;
        const double available = (std::max)(0.0, budget - used);
        return static_cast<unsigned int>((std::min)(static_cast<double>(remaining), (std::max)(1.0, std::floor(available / perRow))));
    };

    while((used < budget || used == 0.0) && job.stage != EnvironmentPreparationStage::Recorded)
    {
        switch(job.stage)
        {
        case EnvironmentPreparationStage::MipChain:
            job.mipPass.Execute(commandList, environment.GetSourceTexture());
            used += EstimateDrawMicroseconds(static_cast<std::uint64_t>(staged.width) * staged.height * 4 / 3, 4);
            job.stage = EnvironmentPreparationStage::DiffuseIrradiance;
            job.face = 0;
            job.row = 0;
            break;

        case EnvironmentPreparationStage::DiffuseIrradiance:
        {
            const unsigned int size = job.diffusePass.GetSize();
            const unsigned int rows = rowsWithin(size, size - job.row, DiffuseSamplesPerTexel);
            job.diffusePass.RecordRows(commandList, environment.GetSourceTexture(), environment.GetDiffuseIrradiance(), job.face,
                job.row, job.row + rows);
            used += EstimateDrawMicroseconds(static_cast<std::uint64_t>(rows) * size, DiffuseSamplesPerTexel);
            job.row += rows;

            if(job.row == size)
            {
                job.row = 0;

                if(++job.face == 6)
                {
                    job.stage = EnvironmentPreparationStage::Cubemap;
                    job.face = 0;
                    job.mip = 0;
                }
            }

            break;
        }

        case EnvironmentPreparationStage::Cubemap:
        {
            const unsigned int mipLevels = job.cubemapPass.GetMipLevels();
            const unsigned int size = (std::max)(1u, job.cubemapPass.GetSize() >> job.mip);
            const std::uint64_t samples = CubemapSamplesPerTexel(job.mip, mipLevels);
            const unsigned int rows = rowsWithin(size, size - job.row, samples);
            job.cubemapPass.RecordRows(commandList, environment.GetSourceTexture(), environment.GetCubemap(), job.mip, job.face,
                job.row, job.row + rows);
            used += EstimateDrawMicroseconds(static_cast<std::uint64_t>(rows) * size, samples);
            job.row += rows;

            if(job.row == size)
            {
                job.row = 0;

                if(++job.face == 6)
                {
                    job.face = 0;

                    if(++job.mip == mipLevels)
                    {
                        job.stage = EnvironmentPreparationStage::Multiscatter;
                        job.mip = 0;
                    }
                }
            }

            break;
        }

        case EnvironmentPreparationStage::Multiscatter:
        {
            const unsigned int size = (std::max)(1u, job.multiscatterPass.GetSize() >> job.mip);
            job.multiscatterPass.RecordFace(commandList, environment.GetSourceTexture(), m_brdfIntegrationLUT,
                environment.GetGGXMultiscatterIrradiance(), job.mip, job.face);
            used += EstimateDrawMicroseconds(static_cast<std::uint64_t>(size) * size, MultiscatterSamplesPerTexel);

            if(++job.face == 6)
            {
                job.face = 0;

                if(++job.mip == job.multiscatterPass.GetMipLevels())
                {
                    job.stage = EnvironmentPreparationStage::Recorded;
                }
            }

            break;
        }

        default:
            job.stage = EnvironmentPreparationStage::Recorded;
            break;
        }
    }

    StreamingTelemetry::Add(StreamingTelemetry::Metric::EnvironmentWorkUnits, static_cast<std::uint64_t>(used));
}

void Renderer::CommitEnvironmentPreparationSubmission(bool executed)
{
    if(!m_environmentJob || !m_environmentJob->recordedUncommitted)
    {
        return;
    }

    if(!executed)
    {
        // Publication restarts or fails the job; its owners stay until shutdown (RetireEnvironmentJob).
        m_environmentJob->submissionUncertain = true;
        return;
    }

    m_environmentJob->recordedUncommitted = false;
    m_environmentJob->lastSubmissionFence = m_commandContext.GetLastSubmissionFenceValue();

    if(std::exchange(m_environmentJob->copiesFinishedUncommitted, false))
    {
        // The last copy reading the staging buffer is in this submission: release it behind that fence, not at publication.
        m_environmentRetirements.push_back({m_environmentJob->lastSubmissionFence, nullptr,
            std::move(m_environmentJob->TakeCpuResult().staging)});
    }
}

void Renderer::ShutdownEnvironmentPreparation()
{
    // After the shutdown drain: every retirement is complete; the job destructors join their workers.
    m_environmentQueuedSource.reset();
    m_environmentJob.reset();
    m_environmentDrainingJobs.clear();
    m_environmentRetirements.clear();

    if(m_environmentState == EnvironmentPreparationState::Pending)
    {
        m_environmentState = EnvironmentPreparationState::Superseded;
    }
}
