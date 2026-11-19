#pragma once

#include "Renderer/Buffer.h"
#include "Renderer/ComputePipeline.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <wrl/client.h>

class DescriptorHeap;
class GraphicsDevice;
class ShaderLibrary;
class UploadBuffer;

// NVIDIA SHARC v1.8.3 (NVIDIA-RTX/SHARC 4e21b585): the Engine owner of NVIDIA's spatially hashed radiance cache
// (Documentation/Rendering/NVIDIA_SHARC_Integration_A.md). NVIDIA's unmodified shader headers (assets/Shaders/Sharc,
// deployed by CMake for the Editor only) do the hashing, accumulation, resolve and lookup; this class owns their three
// buffers, the clear, the resolve and the statistics, with the lifetime rules of the D6 cache it will replace:
//   - one queue: a frame records [clear] -> update (training dispatch) -> resolve -> render, separated by UAV barriers, so
//     no fence or CPU wait is needed between them or across frames;
//   - replaced buffers are retired, with their descriptors, until the last submission that may use them completes;
//   - a clear counts only once its command list is submitted; an unsubmitted or uncertain recording clears again.
class SharcCache
{
public:
    struct Settings
    {
        // log2 of the entry count: 20 (40 MiB, production), 21 or 22 (80 or 160 MiB).
        std::uint32_t capacityLog2 = 20;
        // NVIDIA's HashGridParameters::sceneScale: a voxel edge is about 2^level / sceneScale metres, level ~ log2 of the
        // camera distance (at least 1).
        float sceneScale = 35.0f;
        // SharcResolveParameters: the temporal window (frames) and the stale-entry lifetime.
        std::uint32_t accumulationFrames = 16;
        std::uint32_t staleFrames = 64;
        // SharcParameters::radianceScale: the fixed-point scale of the per-frame 32-bit accumulation. A fixed value in
        // Engine's GPU radiance units (scene-linear, already scaled by the scene's reference exposure EV_ref), never
        // derived from auto-exposure, so a stored entry means the same radiance in every frame. 1e5 (Checkpoint A precision
        // probe): a deposit loses at most 1e-5 to truncation (-0.04% bias for dim deposits of mean 0.01, -0.5% at 0.001), and
        // a voxel's per-frame sum wraps only beyond 42,950 units, about 13,000x the largest sum measured in the fixtures.
        float radianceScale = 1.0e5f;

        bool operator==(const Settings&) const = default;
    };

    // Must match SharcNvConstants (assets/Shaders/SharcNvCommon.hlsli).
    struct Constants
    {
        std::uint32_t hashEntriesIndex = 0;
        std::uint32_t accumulationIndex = 0;
        std::uint32_t resolvedIndex = 0;
        std::uint32_t capacity = 0;

        float cameraPosition[3]{};
        float sceneScale = 0.0f;

        float previousCameraPosition[3]{};
        float radianceScale = 0.0f;

        std::uint32_t accumulationFrames = 0;
        std::uint32_t staleFrames = 0;
        std::uint32_t frameIndex = 0;
        std::uint32_t trainingTile = 0;

        std::uint32_t trainingTilesX = 0;
        std::uint32_t trainingTilesY = 0;
        std::uint32_t queryVertex = 0;
        std::uint32_t flags = 0;

        std::uint32_t statisticsIndex = 0;
        std::uint32_t trainingMaxDepth = 0;
        std::uint32_t depositFirstVertex = 0;
        std::uint32_t depositLastVertex = 0;

        float depositLuminanceLimit = 0.0f;
        float padding0[3]{};

        float padding1[4]{};
    };

    static_assert(sizeof(Constants) == 8 * 16);

    static constexpr std::uint32_t FlagStatistics = 1;
    static constexpr std::uint32_t MinimumCapacityLog2 = 20;
    static constexpr std::uint32_t MaximumCapacityLog2 = 22;
    // Hash key (uint64), accumulation (uint4) and resolved (float16_t4 + 2 uints) per entry: NVIDIA's 40-byte layout.
    static constexpr std::uint32_t KeyStride = 8;
    static constexpr std::uint32_t AccumulationStride = 16;
    static constexpr std::uint32_t ResolvedStride = 16;
    static constexpr std::uint64_t BytesPerEntry = KeyStride + AccumulationStride + ResolvedStride;

    // SHARC_NV_STATISTIC_* (SharcNvCommon.hlsli).
    static constexpr std::uint32_t StatisticWords = 16;
    static constexpr std::uint32_t StatisticQueryReached = 0;
    static constexpr std::uint32_t StatisticQueryAdmitted = 1;
    static constexpr std::uint32_t StatisticQueryHit = 2;
    static constexpr std::uint32_t StatisticTrainingPaths = 3;
    static constexpr std::uint32_t StatisticDeposits = 4;
    static constexpr std::uint32_t StatisticDepositStops = 5;
    static constexpr std::uint32_t StatisticMisses = 6;
    static constexpr std::uint32_t StatisticMaximumDeposit = 7;
    static constexpr std::uint32_t StatisticClampedDeposits = 8;
    static constexpr std::uint32_t StatisticOccupied = 9;
    static constexpr std::uint32_t StatisticSampled = 10;
    static constexpr std::uint32_t StatisticMaximumAccumulation = 11;
    static constexpr std::uint32_t StatisticHighAccumulation = 12;
    static constexpr std::uint32_t StatisticMaximumSamples = 13;
    static constexpr std::uint32_t StatisticSaturated = 14;
    static constexpr std::uint32_t StatisticResolvedOccupied = 15;
    static constexpr std::uint32_t ReadbackSlots = 2;

    struct Diagnostics
    {
        std::uint64_t allocations = 0;
        std::uint64_t allocationFailures = 0;
        std::uint64_t retirements = 0;
        std::uint64_t clearsRecorded = 0;
        std::uint64_t clearsCommitted = 0;
        std::uint64_t uncertainSubmissions = 0;
        std::uint64_t resolvesRecorded = 0;
    };

    // Why NVIDIA SHARC cannot run on this device or installation; empty when it can. shaderRoot is the deployed
    // assets/Shaders directory; a packaged game (packaged) has none and runs NVIDIA SHARC from its cooked DXIL.
    static std::string CapabilityFailure(const GraphicsDevice& device, const std::filesystem::path& shaderRoot,
        bool packaged);

    static bool IsValidSettings(const Settings& settings);

    static std::uint64_t ResourceBytes(const Settings& settings)
    {
        return (std::uint64_t{1} << settings.capacityLog2) * BytesPerEntry;
    }

    // Allocates the buffers for settings, or keeps the current ones if they match. Replaced buffers are retired until
    // lastSubmissionFence completes. A new allocation is cleared before its first use (RecordClearIfNeeded).
    bool EnsureResources(ID3D12Device* device, ShaderLibrary& shaderLibrary, DescriptorHeap& descriptorHeap,
        const Settings& settings, std::uint64_t lastSubmissionFence, bool failForTesting);

    // Moves the buffers to the retired list until lastSubmissionFence completes.
    void Retire(std::uint64_t lastSubmissionFence);
    void ReleaseRetired(std::uint64_t completedFence);
    // After the renderer's final GPU drain only.
    void Shutdown();

    // A frame recording begins: a clear recorded by an earlier, never submitted recording did not run.
    void BeginRecording();
    // The frame's list was submitted: its clear (if any) now precedes every later frame.
    void CommitSubmission();
    // Submission failed after it may have reached the GPU: contents unknown, so the next frame clears.
    void MarkSubmissionUncertain();
    // The next frame clears the cache (an invalidation of what the cached radiance means).
    void RequestClear();

    // The frame's constants with this cache's buffers, capacity and settings filled in (the caller sets the rest).
    Constants MakeConstants() const;

    // Records the clear if one is due, then the transitions to unordered access. Must precede every cache access of the
    // frame. The cache is usable this frame only if this returns true.
    bool RecordFrameBegin(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer, const Constants& constants);

    // The UAV barrier between the update (training) dispatch and the resolve, and between the resolve and the render.
    void RecordUAVBarrier(ID3D12GraphicsCommandList* commandList);

    // The resolve of this frame's samples (after the update dispatch and a UAV barrier, which this records). With
    // constants.flags & FlagStatistics it also records the cache statistics passes around it.
    bool RecordResolve(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer, const Constants& constants,
        std::uint32_t frameSlot);

    // After the frame's render dispatch: copies the statistics (update, cache and query counters) to frameSlot's readback,
    // when the frame records statistics.
    void RecordStatisticsReadback(ID3D12GraphicsCommandList* commandList, const Constants& constants,
        std::uint32_t frameSlot);

    // The statistics frameSlot's previous frame copied, after the renderer's wait for that slot (false if none).
    bool ReadStatistics(std::uint32_t frameSlot, std::uint32_t (&words)[StatisticWords]) const;

    bool HasResources() const
    {
        return m_resources.has_value();
    }

    bool IsUsable() const
    {
        return m_usable;
    }

    const Settings& GetSettings() const
    {
        return m_settings;
    }

    // Frames resolved into the current contents since their clear.
    std::uint64_t GetResolvedFrames() const
    {
        return m_resolvedFrames;
    }

    const Diagnostics& GetDiagnostics() const
    {
        return m_diagnostics;
    }

    std::size_t GetRetiredCount() const
    {
        return m_retired.size();
    }

    // Diagnostics read the raw buffers.
    Buffer* GetHashEntriesForTesting()
    {
        return m_resources ? &m_resources->hashEntries : nullptr;
    }

    Buffer* GetResolvedForTesting()
    {
        return m_resources ? &m_resources->resolved : nullptr;
    }

private:
    struct Resources
    {
        Buffer hashEntries;
        Buffer accumulation;
        Buffer resolved;
        Buffer statistics;
    };

    struct RetiredResources
    {
        Resources resources;
        std::uint64_t lastUseFence = 0;
    };

    static void ReleaseResources(Resources& resources);
    bool EnsurePipelines(ID3D12Device* device, ShaderLibrary& shaderLibrary);
    bool RecordDispatch(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer, const Constants& constants,
        const ComputePipeline& pipeline, std::uint32_t threads);
    bool EnsureReadback(ID3D12Device* device);

    ComputePipeline m_clearPipeline;
    ComputePipeline m_clearStatisticsPipeline;
    ComputePipeline m_accumulationStatisticsPipeline;
    ComputePipeline m_resolvePipeline;
    ComputePipeline m_resolvedStatisticsPipeline;
    bool m_pipelinesReady = false;
    bool m_pipelinesFailed = false;

    std::optional<Resources> m_resources;
    std::vector<RetiredResources> m_retired;
    Settings m_settings;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_readback;
    bool m_readbackRecorded[ReadbackSlots]{};

    // The contents need a clear (new allocation, invalidation, uncertain submission).
    bool m_clearNeeded = true;
    // A clear was recorded in the current, not yet submitted recording.
    bool m_clearRecorded = false;
    bool m_usable = false;
    std::uint64_t m_resolvedFrames = 0;
    Diagnostics m_diagnostics;
};
