#include "Renderer/EnvironmentPreparation.h"

#include "Debug/StreamingTelemetry.h"
#include "Renderer/Renderer.h"

#include <Windows.h>

#include <cmath>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <vector>

namespace
{
// Releases worker-created resources that no view or registry entry references yet (no descriptors to free).
void DiscardEnvironment(EnvironmentPreparationJob::CpuResult& result)
{
    if(result.environment)
    {
        result.environment->Shutdown();
        result.environment.reset();
    }
}
}

EnvironmentPreparationJob::EnvironmentPreparationJob(std::uint64_t requestTicket, EnvironmentPreparationSource requestSource,
    std::string publishedIdentity)
    : ticket(requestTicket), source(std::move(requestSource)), m_publishedIdentity(std::move(publishedIdentity))
{
    requestedIdentity = source.identity.empty() ? std::string() :
        Renderer::CalibratedEnvironmentIdentity(source.identity, source.radianceScale);
    requested = std::chrono::steady_clock::now();
}

EnvironmentPreparationJob::~EnvironmentPreparationJob()
{
    Cancel();

    if(m_worker.joinable())
    {
        m_worker.join();
    }

    ShutdownGpuObjects();
}

void EnvironmentPreparationJob::Start(ID3D12Device* device)
{
    m_worker = std::thread([this, device]() { Run(device); });
}

EnvironmentPreparationJob::CpuResult& EnvironmentPreparationJob::TakeCpuResult()
{
    if(m_worker.joinable())
    {
        m_worker.join();
    }

    return m_result;
}

void EnvironmentPreparationJob::ShutdownGpuObjects()
{
    multiscatterPass.Shutdown();
    diffusePass.Shutdown();
    cubemapPass.Shutdown();
    mipPass.Shutdown();

    if(environment)
    {
        environment->Shutdown();
        environment.reset();
    }

    m_result.staging.Reset();
    DiscardEnvironment(m_result);
}

void EnvironmentPreparationJob::Run(ID3D12Device* device)
{
    // Background work must not compete with the frame thread.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const auto started = std::chrono::steady_clock::now();
    CpuResult& result = m_result;

    try
    {
        const auto cancelled = [this]() { return m_cancelled.load(std::memory_order_relaxed); };
        result.sourceIdentity = source.identity;

        if(!requestedIdentity.empty() && requestedIdentity == m_publishedIdentity)
        {
            result.unchanged = true;
        }

        HDRImageData image;

        if(!result.unchanged && !cancelled())
        {
            image = source.load(result.sourceIdentity);

            if(Renderer::CalibratedEnvironmentIdentity(result.sourceIdentity, source.radianceScale) == m_publishedIdentity)
            {
                result.unchanged = true;
            }
        }

        if(!result.unchanged && !cancelled())
        {
            // The checks EnvironmentMap::Initialize applies to the synchronous path.
            if(image.width == 0 || image.height == 0 || image.channels != 4 ||
                image.pixels.size() != static_cast<size_t>(image.width) * image.height * 4)
            {
                throw std::runtime_error("Invalid environment image dimensions or pixel storage.");
            }

            const float scale = source.radianceScale;

            if(!std::isfinite(scale) || scale < 0.0f)
            {
                throw std::runtime_error("Invalid environment radiance scale.");
            }

            // In place: the same single multiplication per texel the synchronous path applies to its calibrated copy.
            if(scale != 1.0f)
            {
                for(size_t index = 0; index < image.pixels.size(); ++index)
                {
                    if(index % 4 != 3)
                    {
                        image.pixels[index] *= scale;
                    }
                }
            }

            std::vector<float> conditionalCDF;
            std::vector<float> marginalCDF;
            result.totalWeight = EnvironmentMap::BuildImportanceDistribution(image, conditionalCDF, marginalCDF);
            result.width = image.width;
            result.height = image.height;

            if(!cancelled())
            {
                // The destination texture's description, so the footprint matches the frame-recorded copy exactly.
                D3D12_RESOURCE_DESC texture{};
                texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                texture.Width = image.width;
                texture.Height = image.height;
                texture.DepthOrArraySize = 1;
                texture.MipLevels = static_cast<UINT16>(EnvironmentMap::SourceMipLevels(image.width, image.height));
                texture.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                texture.SampleDesc.Count = 1;
                texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

                UINT rows = 0;
                UINT64 rowBytes = 0;
                UINT64 textureBytes = 0;
                device->GetCopyableFootprints(&texture, 0, 1, 0, &result.textureFootprint, &rows, &rowBytes, &textureBytes);

                if(textureBytes == 0 || rowBytes != static_cast<UINT64>(image.width) * 16)
                {
                    throw std::runtime_error("Environment staging footprint is invalid.");
                }

                constexpr std::uint64_t alignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
                result.textureRows = rows;
                result.conditionalBytes = conditionalCDF.size() * sizeof(float);
                result.marginalBytes = marginalCDF.size() * sizeof(float);
                result.conditionalOffset = (textureBytes + alignment - 1) / alignment * alignment;
                result.marginalOffset = (result.conditionalOffset + result.conditionalBytes + alignment - 1) / alignment * alignment;

                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_UPLOAD;
                D3D12_RESOURCE_DESC buffer{};
                buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                buffer.Width = result.marginalOffset + result.marginalBytes;
                buffer.Height = 1;
                buffer.DepthOrArraySize = 1;
                buffer.MipLevels = 1;
                buffer.SampleDesc.Count = 1;
                buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

                // The device is free-threaded. The main thread registers the buffer with the memory registry at handoff.
                if(FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&result.staging))))
                {
                    throw std::runtime_error("Failed to create environment staging buffer.");
                }

                std::byte* mapped = nullptr;
                const D3D12_RANGE noRead{0, 0};

                if(FAILED(result.staging->Map(0, &noRead, reinterpret_cast<void**>(&mapped))))
                {
                    result.staging.Reset();
                    throw std::runtime_error("Failed to map environment staging buffer.");
                }

                const auto* pixels = reinterpret_cast<const std::byte*>(image.pixels.data());

                for(UINT y = 0; y < rows; ++y)
                {
                    std::memcpy(mapped + result.textureFootprint.Offset + static_cast<size_t>(y) * result.textureFootprint.Footprint.RowPitch,
                        pixels + static_cast<size_t>(y) * rowBytes, static_cast<size_t>(rowBytes));
                }

                std::memcpy(mapped + result.conditionalOffset, conditionalCDF.data(), static_cast<size_t>(result.conditionalBytes));
                std::memcpy(mapped + result.marginalOffset, marginalCDF.data(), static_cast<size_t>(result.marginalBytes));
                result.staging->Unmap(0, nullptr);

                // Committing hundreds of MiB of destination memory costs milliseconds: it happens here, not in a frame.
                result.environment = std::make_unique<EnvironmentMap>();

                if(!result.environment->CreateResourcesForUpload(device, image.width, image.height, result.totalWeight))
                {
                    throw std::runtime_error("Failed to create environment resources.");
                }
            }
        }
    }
    catch(const std::exception& exception)
    {
        result.staging.Reset();
        DiscardEnvironment(result);
        result.error = exception.what();

        if(result.error.empty())
        {
            result.error = "Environment preparation failed.";
        }
    }
    catch(...)
    {
        result.staging.Reset();
        DiscardEnvironment(result);
        result.error = "Environment preparation failed.";
    }

    result.workerNanoseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    StreamingTelemetry::Add(StreamingTelemetry::Metric::EnvironmentWorkerNs, result.workerNanoseconds);
    m_cpuDone.store(true, std::memory_order_release);
}
