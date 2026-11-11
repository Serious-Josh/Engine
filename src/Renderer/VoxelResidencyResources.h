#pragma once

#include "Renderer/Buffer.h"
#include "Renderer/UploadBuffer.h"
#include "Voxel/VoxelResidency.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

class DescriptorHeap;

// VX.5 Step 2: the GPU side of static voxel obstacle residency (VoxelArchitecture.md §6.10).
//
// Storage, all raw (ByteAddressBuffer) SRVs in the bindless heap:
//   - brick pool: up to MaximumChunks chunk buffers of whole 64-brick pages (80-byte bricks), added as residency needs them;
//   - index pool: up to MaximumChunks chunk buffers of R^3-entry index slots, added likewise;
//   - region table: TableVersions versions of the W^3 toroidal table in one buffer.
// Nothing is reserved up front: the upload ring is created on the first Configure, chunks as regions are added.
//
// Publication. Inputs arrive between frames (AddRegion, RemoveRegion, SetTable); the copies are recorded by RecordUploads at the
// start of the frame's command list, right after Begin has waited for that frame slot (so its upload memory is free), before any
// pass. A region uploads over as many frames as the per-frame budget needs. A table is published only when every Populated
// entry names a fully uploaded slot, into a table version no in-flight frame reads; the binding then switches to it.
//
// Lifetime. Nothing an older submission may read is overwritten: pages and index slots of a removed region, and the previous
// table version, retire with the last submission fence before the publishing frame (the publishing frame already reads the new
// table) and are reused only after the GPU completes it. Whole chunks and table buffers retire the same way on Configure and
// Reset; their descriptors are freed with them. A command-context reset drains the GPU, so retired items are then all released.
//
// Before a table is published, and after Reset, the binding's `valid` is 0: every query answers unknown.

// Mirrored in HLSL (Step 3). Descriptor indices are into the Renderer's bindless resource heap.
struct VoxelWorldBinding
{
    std::uint32_t valid = 0;
    std::uint32_t window = 0;
    std::uint32_t tableIndex = 0;
    // First entry of the published table version.
    std::uint32_t tableBase = 0;

    std::int32_t regionBricksLog2 = 0;
    std::int32_t baseExponent = 0;
    std::uint32_t pagesPerChunk = 0;
    std::uint32_t slotsPerChunk = 0;

    float voxelSize = 0.0f;
    float inverseVoxelSize = 0.0f;
    std::uint32_t generation = 0;
    std::uint32_t padding = 0;

    std::array<std::uint32_t, 8> brickChunks{};
    std::array<std::uint32_t, 8> indexChunks{};

    std::array<std::uint32_t, 4> reserved{};
};

static_assert(sizeof(VoxelWorldBinding) == 128);

class VoxelResidencyResources
{
public:
    static constexpr std::uint32_t MaximumChunks = 8;
    static constexpr std::uint32_t TableVersions = 4;

    struct Settings
    {
        // Ceilings, approved for VX.5; storage grows towards them in chunks.
        std::size_t brickPoolBytes = std::size_t{64} << 20;
        std::size_t indexPoolBytes = std::size_t{32} << 20;
        std::size_t uploadBytesPerFrame = std::size_t{8} << 20;
    };

    struct Statistics
    {
        std::uint32_t brickChunks = 0;
        std::uint32_t indexChunks = 0;
        std::uint32_t pagesFree = 0;
        std::uint32_t pagesRetired = 0;
        std::uint32_t slotsFree = 0;
        std::uint32_t slotsRetired = 0;
        std::size_t pendingRegions = 0;
        std::size_t retiredBuffers = 0;
        std::uint64_t bytesUploaded = 0;
        std::uint64_t tablesPublished = 0;
    };

    bool Initialize(ID3D12Device* device, DescriptorHeap& descriptorHeap, const Settings& settings);
    // After the Renderer's GPU drain: releases everything, retired items included.
    void Shutdown();

    // ---- Main thread, between frames ----

    // A new residency space (a scene's manifest): region size R (16, 32 or 64), voxel size and window. Everything before it
    // retires with `lastSubmissionFence`; the binding is invalid until the first table is published.
    bool Configure(std::uint32_t regionBricks, int baseExponent, std::uint32_t window, std::uint64_t lastSubmissionFence,
        std::string* error);
    // Nothing resident; the binding is invalid. Used on scene switches.
    void Reset(std::uint64_t lastSubmissionFence);

    // Queues a region for upload and returns its index slot, or nothing when the ceilings are reached or a chunk cannot be
    // created. Storage freed by earlier removals becomes available once the GPU completes its fence.
    std::optional<std::uint32_t> AddRegion(const Voxel::Residency::PackedRegion& region, std::uint64_t completedFence);
    // The region in `slot` leaves residency. Its storage retires when a table that no longer names the slot is published. A
    // region still uploading is cancelled.
    void RemoveRegion(std::uint32_t slot);
    // The table to publish next (W^3 entries). Published when every Populated entry names an uploaded slot; an identical table
    // is not uploaded again.
    void SetTable(std::vector<Voxel::Residency::TableEntry> table);
    // Index slots whose upload completed since the last call (in the order they completed).
    std::vector<std::uint32_t> TakeUploadedSlots();

    // ---- Inside Render, after Begin, before any pass ----

    void RecordUploads(ID3D12GraphicsCommandList* commandList, unsigned int frameIndex, std::uint64_t lastSubmissionFence,
        std::uint64_t completedFence, std::uint64_t submissionLifetime);

    const VoxelWorldBinding& GetBinding() const noexcept
    {
        return m_binding;
    }

    Statistics GetStatistics() const;

private:
    friend class VoxelResidencyVerification;
    friend class VoxelQueryVerification;
    friend class VoxelStreamingVerification;
    friend class AirFieldVerification;

    struct PendingRegion
    {
        std::uint32_t slot = 0;
        std::vector<std::uint32_t> pages;
        std::vector<std::uint32_t> index;
        std::vector<Voxel::Residency::PackedBrick> bricks;
        // Upload progress: pages copied so far, and whether the index slot is copied.
        std::uint32_t pagesDone = 0;
        bool indexDone = false;
    };

    struct ResidentRegion
    {
        std::vector<std::uint32_t> pages;
        bool uploaded = false;
        bool removing = false;
    };

    struct RetiredBuffer
    {
        Buffer buffer;
        std::uint64_t fence = 0;
    };

    bool GrowBricks(std::uint32_t pagesNeeded);
    bool GrowSlots();
    bool CreateChunk(std::size_t bytes, Buffer& chunk);
    void RetireBuffer(Buffer& buffer, std::uint64_t fence);
    void CollectRetired(std::uint64_t completedFence);
    void ReleaseAllStorage(std::uint64_t fence);
    bool UploadRegions(ID3D12GraphicsCommandList* commandList, std::size_t& budget, std::vector<Buffer*>& touched);
    void PublishTable(ID3D12GraphicsCommandList* commandList, std::uint64_t lastSubmissionFence, std::size_t& budget,
        std::vector<Buffer*>& touched);
    bool Copy(ID3D12GraphicsCommandList* commandList, Buffer& destination, std::size_t offset, const void* data, std::size_t bytes,
        std::vector<Buffer*>& touched);

    ID3D12Device* m_device = nullptr;
    DescriptorHeap* m_descriptorHeap = nullptr;
    Settings m_settings;

    UploadBuffer m_upload;
    bool m_uploadReady = false;

    // The configured space.
    bool m_configured = false;
    std::uint32_t m_regionBricks = 0;
    std::size_t m_sliceBytes = 0;
    std::size_t m_brickChunkBytes = 0;
    std::size_t m_indexChunkBytes = 0;

    std::vector<Buffer> m_brickChunks;
    std::vector<Buffer> m_indexChunks;
    Buffer m_table;
    Voxel::Residency::SlotAllocator m_pages;
    Voxel::Residency::SlotAllocator m_slots;
    Voxel::Residency::SlotAllocator m_versions;
    std::optional<std::uint32_t> m_publishedVersion;
    std::vector<Voxel::Residency::TableEntry> m_publishedTable;
    std::optional<std::vector<Voxel::Residency::TableEntry>> m_pendingTable;

    std::deque<PendingRegion> m_pending;
    std::map<std::uint32_t, ResidentRegion> m_regions;
    std::vector<std::uint32_t> m_uploadedSlots;
    std::deque<RetiredBuffer> m_retiredBuffers;
    std::uint64_t m_submissionLifetime = 0;

    VoxelWorldBinding m_binding;
    std::uint64_t m_bytesUploaded = 0;
    std::uint64_t m_tablesPublished = 0;
};
