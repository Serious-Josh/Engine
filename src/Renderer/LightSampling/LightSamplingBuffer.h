#pragma once

#include "Renderer/Buffer.h"
#include "Renderer/LightSampling/LightSamplingData.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

class DescriptorHeap;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

// M4.2A: bytes before the data: the published LightSamplingConstants, zero-padded (LIGHT_SAMPLING_HEADER_BYTES in
// LightSampling.hlsli).
inline constexpr std::uint64_t LightSamplingHeaderBytes = 128;

// M4.1: the GPU copy of one LightSamplingData (a raw buffer, read through LightSamplingConstants.bufferIndex), published
// in the frame command list after the scene light buffer and keyed by the same lighting revision.
// - M4.2A: the buffer starts with a LightSamplingHeaderBytes header holding the published LightSamplingConstants
//   (bufferIndex, valid and revision included), so a shader holding only the descriptor index can read them. The data's
//   section offsets are relative to the end of the header (LightSampling.hlsli adds it).
// - Copied only when the revision differs from the one the buffer holds (0 = unknown: always copied), from a staging
//   upload resource of its own, so a large build does not grow the frame upload ring's retained overflow pages.
// - Capacity grows to the data plus 25% (64 KiB steps) and never shrinks while the renderer lives. A replaced buffer and
//   every staging resource are retired with a fence their last possible reader precedes, and released once it completes.
//   M4.2C fix: a staging resource is read by the recording that created it, whose submission fence is known only once
//   it is submitted (other submissions may come first), so it stays pending until Submitted reports that fence for its
//   recording generation. Before, it was retired with "last submitted fence + 1", which an intervening submission could
//   complete while the copy was still queued (a D3D12 debug-layer object-deleted-while-in-use break).
// - A recording that is discarded (DiscardRecording) forgets the revision it would have published, so the next frame
//   copies again: a skipped copy can never leave a buffer whose contents were never written.
// - Constants are valid (valid = 1) only for the frame that published or reused this exact revision; on failure they
//   are zeroed and consumers must not read the buffer.
// M4.1.1: an incremental update's changed words. The buffer applies them only when it holds exactly `baseRevision` (the
// data before the update); otherwise, or when the buffer must grow, it copies the whole data. The GPU queue runs frames in
// order, so the frame that records the patch copies after every earlier frame's reads, as for a whole copy.
struct LightSamplingPatch
{
    std::uint64_t baseRevision = 0;
    const std::vector<LightSamplingDirtyRange>* ranges = nullptr;
};

class LightSamplingBuffer
{
    public:
    enum class RecordResult
    {
        // The data was copied this frame (whole, or as a patch: see GetLastCopyWasPatch).
        Published,
        // The buffer already holds this revision; nothing was copied.
        Reused,
        // Nothing can be published (allocation failed); constants are zeroed (valid = 0).
        Failed
    };

    LightSamplingBuffer() = default;
    LightSamplingBuffer(const LightSamplingBuffer&) = delete;
    LightSamplingBuffer& operator=(const LightSamplingBuffer&) = delete;
    ~LightSamplingBuffer();

    // Records the copy of `data` (unless the buffer already holds `revision`) and the transition to shader read.
    // lastSubmittedFence: the newest fence submitted before this recording; completedFence: the GPU's completed fence.
    // patch (M4.1.1): when not null, the words that changed since `patch->baseRevision`; copied instead of the whole data
    // when the buffer holds that revision.
    // recordingGeneration: the renderer's recording that will submit this command list (see Submitted).
    RecordResult Record(ID3D12Device* device, DescriptorHeap& descriptorHeap, ID3D12GraphicsCommandList* commandList,
        const LightSamplingData& data, std::uint64_t revision, std::uint64_t recordingGeneration,
        std::uint64_t lastSubmittedFence, std::uint64_t completedFence, LightSamplingConstants& constants,
        std::string* failureReason = nullptr, const LightSamplingPatch* patch = nullptr);

    // The recording `recordingGeneration` was submitted with `fence`: its staging resources are released once it completes.
    void Submitted(std::uint64_t recordingGeneration, std::uint64_t fence) noexcept;

    // The recording that called Record was discarded before submission: its copy never ran, so its staging resources
    // need only outlive what was submitted before (lastSubmittedFence).
    void DiscardRecording(std::uint64_t lastSubmittedFence) noexcept;

    // Releases retired resources whose fence completed (also done by Record).
    void ReleaseCompleted(std::uint64_t completedFence);

    // Releases everything. Only after the renderer's GPU drain.
    void Shutdown();

    std::uint64_t GetCapacityBytes() const noexcept
    {
        return m_capacityBytes;
    }

    std::uint64_t GetCopyCount() const noexcept
    {
        return m_copyCount;
    }

    std::uint64_t GetLastCopyBytes() const noexcept
    {
        return m_lastCopyBytes;
    }

    // M4.1.1: whether the last copy was a patch, and how many patches have been copied.
    bool GetLastCopyWasPatch() const noexcept
    {
        return m_lastCopyWasPatch;
    }

    std::uint64_t GetPatchCount() const noexcept
    {
        return m_patchCount;
    }

    // M4.2C: bytes copied by every recording so far (whole copies and patches).
    std::uint64_t GetTotalCopyBytes() const noexcept
    {
        return m_totalCopyBytes;
    }

    std::size_t GetRetiredCount() const noexcept
    {
        return m_retired.size();
    }

    // Tests only: the published buffer (shader-read state between recordings), to read its words back.
    ID3D12Resource* GetResourceForTesting() const noexcept
    {
        return m_buffer.GetResource();
    }

    private:
    struct Retired
    {
        Buffer buffer;
        ID3D12Resource* staging = nullptr;
        std::uint64_t fence = 0;
        // A staging resource of a recording not yet submitted: fence unknown (never released) until Submitted.
        bool pending = false;
        std::uint64_t generation = 0;
    };

    bool Grow(ID3D12Device* device, DescriptorHeap& descriptorHeap, std::uint64_t bytes, std::uint64_t retireFence,
        std::string& error);

    Buffer m_buffer;
    std::uint64_t m_capacityBytes = 0;
    std::deque<Retired> m_retired;
    // The revision m_buffer holds (0 = none). m_recordingPending: the current recording copied it (until the next Record
    // or a discard).
    std::uint64_t m_publishedRevision = 0;
    bool m_recordingPending = false;
    D3D12_RESOURCE_STATES m_stateBeforeRecording = D3D12_RESOURCE_STATE_COMMON;
    std::uint64_t m_copyCount = 0;
    std::uint64_t m_lastCopyBytes = 0;
    std::uint64_t m_patchCount = 0;
    std::uint64_t m_totalCopyBytes = 0;
    bool m_lastCopyWasPatch = false;
};
