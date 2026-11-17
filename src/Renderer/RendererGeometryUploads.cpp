#include "Renderer.h"
#include "Debug/StreamingTelemetry.h"

#include <algorithm>

// S0.8B asynchronous geometry uploads: staged buffer copies recorded into frame recordings on the frame queue (which
// orders them before every later BLAS build and draw), confirmed by that frame's submission, with staging retired behind
// its fence. Ownership and lifetime are described in Renderer.h.
namespace
{
constexpr size_t MaximumQueuedGeometryBytes = 128ull << 20;
constexpr size_t GeometryBytesPerFrame = 32ull << 20;
}

bool Renderer::CanQueueGeometryUploads(size_t bytes) const noexcept
{
    size_t queued = 0;

    for(const auto& batch : m_geometryUploads)
    {
        queued += batch.batch.bytes;
    }

    for(const auto& retirement : m_geometryStagingRetirements)
    {
        queued += retirement.bytes;
    }

    return m_sceneResourceLifetime != 0 && bytes <= MaximumQueuedGeometryBytes && queued + bytes <= MaximumQueuedGeometryBytes;
}

std::uint64_t Renderer::QueueGeometryUploads(DeferredBufferUploadBatch&& batch)
{
    if(batch.uploads.empty() || !CanQueueGeometryUploads(batch.bytes))
    {
        return 0;
    }

    const std::uint64_t ticket = ++m_geometryUploadTicket;
    m_geometryUploads.push_back({ticket, std::move(batch), m_sceneResourceLifetime});
    m_geometryUploadStates[ticket] = GeometryUploadState::Queued;
    StreamingTelemetry::Add(StreamingTelemetry::Metric::GeometryUploadsQueued, 1);
    return ticket;
}

Renderer::GeometryUploadState Renderer::TakeGeometryUploadState(std::uint64_t ticket)
{
    const auto found = m_geometryUploadStates.find(ticket);

    if(found == m_geometryUploadStates.end())
    {
        return GeometryUploadState::Unknown;
    }

    const auto state = found->second;

    if(state == GeometryUploadState::Submitted)
    {
        m_geometryUploadStates.erase(found);
    }

    return state;
}

void Renderer::CancelGeometryUpload(std::uint64_t ticket) noexcept
{
    // Only copies not yet recorded are dropped; recorded and submitted copies retire behind their fence as usual.
    std::erase_if(m_geometryUploads, [&](const auto& batch) { return batch.ticket == ticket && !batch.recordedUncommitted; });
    m_geometryUploadStates.erase(ticket);
}

void Renderer::RecordGeometryUploads(ID3D12GraphicsCommandList* commandList)
{
    if(m_geometryUploads.empty() || !commandList)
    {
        return;
    }

    const StreamingTelemetry::Scope recordTiming(StreamingTelemetry::Metric::GeometryUploadRecordNs);
    size_t recorded = 0;

    for(auto& batch : m_geometryUploads)
    {
        if(batch.recordedUncommitted || batch.uncertain || batch.lifetime != m_sceneResourceLifetime)
        {
            continue;
        }

        if(recorded > 0 && recorded + batch.batch.bytes > GeometryBytesPerFrame)
        {
            break;
        }

        // Each destination is a fresh COMMON buffer no recording has used: the copy promotes it to COPY_DEST, and the
        // barrier establishes the same final state the synchronous uploader does within its submission.
        for(const auto& upload : batch.batch.uploads)
        {
            commandList->CopyBufferRegion(upload.destination, 0, upload.staging.Get(), 0, upload.size);

            if(upload.finalState != D3D12_RESOURCE_STATE_COPY_DEST)
            {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = upload.destination;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                barrier.Transition.StateAfter = upload.finalState;
                commandList->ResourceBarrier(1, &barrier);
            }
        }

        batch.recordedUncommitted = true;
        recorded += batch.batch.bytes;
    }

    StreamingTelemetry::Add(StreamingTelemetry::Metric::GeometryUploadBytes, recorded);
}

void Renderer::CommitGeometryUploadSubmission(bool executed)
{
    const std::uint64_t fence = m_commandContext.GetLastSubmissionFenceValue();

    for(auto batch = m_geometryUploads.begin(); batch != m_geometryUploads.end();)
    {
        if(!batch->recordedUncommitted)
        {
            ++batch;
            continue;
        }

        if(!executed)
        {
            // The copies may or may not execute: never publish, and keep the staging until the next GPU drain.
            batch->uncertain = true;
            batch->recordedUncommitted = false;
            m_geometryUploadStates[batch->ticket] = GeometryUploadState::Uncertain;
            ++batch;
            continue;
        }

        for(auto& upload : batch->batch.uploads)
        {
            m_geometryStagingRetirements.push_back({fence, upload.size, std::move(upload.staging)});
        }

        m_geometryUploadStates[batch->ticket] = GeometryUploadState::Submitted;
        batch = m_geometryUploads.erase(batch);
    }
}

void Renderer::CollectGeometryUploadRetirements()
{
    const std::uint64_t completed = m_commandContext.GetCompletedFenceValue();
    std::erase_if(m_geometryStagingRetirements, [&](const auto& retirement) { return retirement.fence <= completed; });

    // A frame that recorded copies but never reached submission (an early failure return) leaves them unsubmitted: the
    // command list is reset before reuse, so they are simply recorded again.
    for(auto& batch : m_geometryUploads)
    {
        if(batch.recordedUncommitted && !m_activeRecordingGeneration)
        {
            batch.recordedUncommitted = false;
        }
    }
}

void Renderer::ReleaseGeometryUploads() noexcept
{
    // After a confirmed GPU drain: no copy can still read staging or write a destination.
    for(const auto& batch : m_geometryUploads)
    {
        m_geometryUploadStates.erase(batch.ticket);
    }

    m_geometryUploads.clear();
    m_geometryStagingRetirements.clear();
}
