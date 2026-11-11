#pragma once

#include "Renderer/Buffer.h"
#include "Renderer/EditorVoxelOverlay.h"
#include "Renderer/Pipeline.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

class ShaderLibrary;
class DepthBuffer;
class RenderTarget;
class ResourceUploader;

// VX.4C.2 Editor voxel overlay (EditorVoxel.hlsl): opaque cubes and region boxes, drawn after tone mapping into the final
// Editor image, before the grid and collider lines.
//
// Depth: the cubes test against and write the presented depth, so they occlude each other correctly and the grid and collider
// lines drawn after them are occluded by them. That depth has already served this frame's lighting and depth-history copy, and
// is cleared or rewritten before the next frame reads it, so voxels never reach lighting, shadows, ray tracing or history.
//
// GPU data: Publish replaces the buffers as a whole, between frames. A replaced pair may still be read by submitted frames, so it
// is queued with the last submission fence and released only once the GPU has completed that fence (CollectRetired), or at
// Shutdown, which the Renderer calls after its GPU drain. Each replacement queues its own entry; nothing is overwritten.
class EditorVoxelPass
{
public:
    bool Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary);
    // After the Renderer's confirmed GPU drain: releases everything, queued buffers included.
    void Shutdown();

    // Uploads `data` (synchronously, confirmed complete) and makes it current. Must not be called while a frame is being
    // recorded. On failure the previous data stays current. Returns the new generation (never 0), or 0 on failure.
    std::uint64_t Publish(ID3D12Device* device, ResourceUploader& uploader, const EditorVoxelOverlay::Data& data,
        std::uint64_t lastSubmissionFence, std::string* error);
    // Drops the current data, queued like a replacement.
    void Clear(std::uint64_t lastSubmissionFence);
    void CollectRetired(std::uint64_t completedFence);

    // Records the overlay when `request` names the current data.
    void Execute(ID3D12GraphicsCommandList* commandList, RenderTarget& viewportColor, DepthBuffer& viewportDepth,
        const Matrix4& viewProjection, const EditorVoxelOverlay::Request& request) const;

    std::uint64_t GetGeneration() const noexcept
    {
        return m_generation;
    }

    std::size_t GetRetiredCount() const noexcept
    {
        return m_retired.size();
    }

    // Diagnostics: the fence the oldest queued pair waits for (0 when none).
    std::uint64_t GetOldestRetiredFence() const noexcept
    {
        return m_retired.empty() ? 0 : m_retired.front().fence;
    }

    // Root constants at b0: clipFromLattice and regionVoxels.
    static constexpr unsigned int ConstantCount = 17;

private:
    friend class VoxelOverlayVerification;

    struct Retired
    {
        Buffer cells;
        Buffer regions;
        std::uint64_t fence = 0;
    };

    void RetireCurrent(std::uint64_t lastSubmissionFence);

    Pipeline m_cellPipeline;
    Pipeline m_boxPipeline;

    Buffer m_cells;
    Buffer m_regions;
    std::uint32_t m_cellCount = 0;
    std::uint32_t m_regionCount = 0;
    std::uint32_t m_regionVoxels = 0;
    std::uint64_t m_generation = 0;
    std::uint64_t m_nextGeneration = 1;
    std::deque<Retired> m_retired;
};
