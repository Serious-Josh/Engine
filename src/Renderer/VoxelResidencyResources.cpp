#include "Renderer/VoxelResidencyResources.h"

#include "Renderer/CommandContext.h"
#include "Renderer/DescriptorHeap.h"

#include <algorithm>
#include <cmath>

using Voxel::Residency::BricksPerPage;
using Voxel::Residency::PackedBrick;
using Voxel::Residency::PageBytes;
using Voxel::Residency::RegionState;
using Voxel::Residency::TableEntry;

namespace
{
constexpr std::size_t BrickChunkTargetBytes = std::size_t{8} << 20;
constexpr std::size_t IndexChunkTargetBytes = std::size_t{4} << 20;
constexpr std::size_t UploadAlignment = 16;
// The state every residency buffer is left in for readers.
constexpr D3D12_RESOURCE_STATES ReadState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

std::uint32_t ChunkLimit(std::size_t ceilingBytes, std::size_t chunkBytes)
{
    const std::size_t chunks = chunkBytes == 0 ? 0 : ceilingBytes / chunkBytes;
    return static_cast<std::uint32_t>(std::clamp<std::size_t>(chunks, 1, VoxelResidencyResources::MaximumChunks));
}
}

bool VoxelResidencyResources::Initialize(ID3D12Device* device, DescriptorHeap& descriptorHeap, const Settings& settings)
{
    m_device = device;
    m_descriptorHeap = &descriptorHeap;
    m_settings = settings;
    return device != nullptr;
}

void VoxelResidencyResources::Shutdown()
{
    // Buffer has no destructor: every buffer is shut down explicitly to release its resource and descriptor.
    for(Buffer& chunk : m_brickChunks)
    {
        chunk.Shutdown();
    }

    for(Buffer& chunk : m_indexChunks)
    {
        chunk.Shutdown();
    }

    for(RetiredBuffer& retired : m_retiredBuffers)
    {
        retired.buffer.Shutdown();
    }

    m_brickChunks.clear();
    m_indexChunks.clear();
    m_retiredBuffers.clear();
    m_table.Shutdown();
    m_upload.Shutdown();
    m_uploadReady = false;
    m_configured = false;
    m_pending.clear();
    m_regions.clear();
    m_binding = {};
}

// ---------------------------------------------------------------------------------------------------------------------------
// Configuration

bool VoxelResidencyResources::Configure(std::uint32_t regionBricks, int baseExponent, std::uint32_t window, std::uint64_t lastSubmissionFence,
    std::string* error)
{
    const auto fail = [&](const char* message)
    {
        if(error)
        {
            *error = message;
        }

        return false;
    };

    ReleaseAllStorage(lastSubmissionFence);

    const auto bricksLog2 = Voxel::RegionBricksLog2(regionBricks);

    if(!bricksLog2 || window == 0 || window % 2 == 0 || window > 15 || !Voxel::IsValidBaseExponent(baseExponent))
    {
        return fail("Unsupported voxel residency configuration.");
    }

    m_regionBricks = regionBricks;
    m_sliceBytes = std::size_t{regionBricks} * regionBricks * regionBricks * sizeof(std::uint32_t);
    m_brickChunkBytes = (BrickChunkTargetBytes / PageBytes) * PageBytes;
    m_indexChunkBytes = (std::max)(IndexChunkTargetBytes, m_sliceBytes);
    const std::size_t tableBytes = std::size_t{window} * window * window * sizeof(TableEntry);

    // A whole index slot and a table version must fit one frame's uploads.
    if(m_sliceBytes + tableBytes > m_settings.uploadBytesPerFrame)
    {
        return fail("The per-frame voxel upload budget is smaller than one index slot.");
    }

    // The upload ring is created on first use: nothing is reserved until residency is configured.
    if(!m_uploadReady)
    {
        m_uploadReady = m_upload.Initialize(m_device, m_settings.uploadBytesPerFrame, CommandContext::FrameCount);

        if(!m_uploadReady)
        {
            return fail("Creating the voxel upload ring failed.");
        }
    }

    if(!CreateChunk(tableBytes * TableVersions, m_table))
    {
        return fail("Creating the voxel region table failed.");
    }

    m_pages = Voxel::Residency::SlotAllocator(0);
    m_slots = Voxel::Residency::SlotAllocator(0);
    m_versions = Voxel::Residency::SlotAllocator(TableVersions);

    // The generation only grows: consumers detect every change, including a new configuration, by comparing it.
    const std::uint32_t generation = m_binding.generation;
    m_binding = {};
    m_binding.generation = generation;
    m_binding.window = window;
    m_binding.tableIndex = m_table.GetDescriptorIndex();
    m_binding.regionBricksLog2 = *bricksLog2;
    m_binding.baseExponent = baseExponent;
    m_binding.pagesPerChunk = static_cast<std::uint32_t>(m_brickChunkBytes / PageBytes);
    m_binding.slotsPerChunk = static_cast<std::uint32_t>(m_indexChunkBytes / m_sliceBytes);
    m_binding.voxelSize = static_cast<float>(std::ldexp(1.0, baseExponent));
    m_binding.inverseVoxelSize = static_cast<float>(std::ldexp(1.0, -baseExponent));
    m_configured = true;
    return true;
}

void VoxelResidencyResources::Reset(std::uint64_t lastSubmissionFence)
{
    ReleaseAllStorage(lastSubmissionFence);
}

void VoxelResidencyResources::ReleaseAllStorage(std::uint64_t fence)
{
    // Frames already submitted may read any of this; it is released once the GPU completes `fence`.
    for(Buffer& chunk : m_brickChunks)
    {
        RetireBuffer(chunk, fence);
    }

    for(Buffer& chunk : m_indexChunks)
    {
        RetireBuffer(chunk, fence);
    }

    RetireBuffer(m_table, fence);
    m_brickChunks.clear();
    m_indexChunks.clear();
    m_pending.clear();
    m_regions.clear();
    m_uploadedSlots.clear();
    m_pendingTable.reset();
    m_publishedVersion.reset();
    m_publishedTable.clear();
    m_configured = false;

    const std::uint32_t generation = m_binding.generation + 1;
    m_binding = {};
    m_binding.generation = generation;
}

void VoxelResidencyResources::RetireBuffer(Buffer& buffer, std::uint64_t fence)
{
    if(buffer.GetResource() == nullptr)
    {
        return;
    }

    RetiredBuffer retired;
    retired.buffer = std::move(buffer);
    retired.fence = fence;
    m_retiredBuffers.push_back(std::move(retired));
}

bool VoxelResidencyResources::CreateChunk(std::size_t bytes, Buffer& chunk)
{
    if(chunk.InitializeGPUWritable(m_device, bytes) && chunk.CreateRawSRV(m_device, *m_descriptorHeap))
    {
        return true;
    }

    chunk.Shutdown();
    return false;
}

bool VoxelResidencyResources::GrowBricks(std::uint32_t pagesNeeded)
{
    const std::uint32_t limit = ChunkLimit(m_settings.brickPoolBytes, m_brickChunkBytes);

    while(m_pages.FreeCount() < pagesNeeded && m_brickChunks.size() < limit)
    {
        Buffer chunk;

        if(!CreateChunk(m_brickChunkBytes, chunk))
        {
            return false;
        }

        m_binding.brickChunks[m_brickChunks.size()] = chunk.GetDescriptorIndex();
        m_brickChunks.push_back(std::move(chunk));
        m_pages.Grow(m_binding.pagesPerChunk);
    }

    return m_pages.FreeCount() >= pagesNeeded;
}

bool VoxelResidencyResources::GrowSlots()
{
    if(m_slots.FreeCount() > 0)
    {
        return true;
    }

    if(m_indexChunks.size() >= ChunkLimit(m_settings.indexPoolBytes, m_indexChunkBytes))
    {
        return false;
    }

    Buffer chunk;

    if(!CreateChunk(m_indexChunkBytes, chunk))
    {
        return false;
    }

    m_binding.indexChunks[m_indexChunks.size()] = chunk.GetDescriptorIndex();
    m_indexChunks.push_back(std::move(chunk));
    m_slots.Grow(m_binding.slotsPerChunk);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Residency changes (between frames)

std::optional<std::uint32_t> VoxelResidencyResources::AddRegion(const Voxel::Residency::PackedRegion& region, std::uint64_t completedFence)
{
    if(!m_configured || region.regionBricks != m_regionBricks)
    {
        return std::nullopt;
    }

    m_pages.Collect(completedFence);
    m_slots.Collect(completedFence);
    const std::uint32_t pagesNeeded = region.PageCount();

    if(!GrowBricks(pagesNeeded) || !GrowSlots())
    {
        return std::nullopt;
    }

    const auto pages = m_pages.Allocate(pagesNeeded);
    const auto slot = m_slots.Allocate(1);

    if(!pages || !slot)
    {
        // Allocated but never written or read: free again at once (fence 0 is always complete).
        if(pages)
        {
            m_pages.Retire(*pages, 0);
            m_pages.Collect(0);
        }

        return std::nullopt;
    }

    PendingRegion pending;
    pending.slot = slot->front();
    pending.pages = *pages;
    pending.index = Voxel::Residency::ResolveIndex(region, *pages);
    pending.bricks = region.bricks;
    m_regions[pending.slot] = ResidentRegion{*pages, false, false};
    m_pending.push_back(std::move(pending));
    return slot->front();
}

void VoxelResidencyResources::RemoveRegion(std::uint32_t slot)
{
    const auto found = m_regions.find(slot);

    if(found == m_regions.end())
    {
        return;
    }

    // An upload in progress stops; pages it already copied are covered by the fence the region retires with.
    found->second.removing = true;
    std::erase_if(m_pending, [slot](const PendingRegion& pending) { return pending.slot == slot; });
}

void VoxelResidencyResources::SetTable(std::vector<TableEntry> table)
{
    if(!m_configured || table.size() != std::size_t{m_binding.window} * m_binding.window * m_binding.window)
    {
        return;
    }

    if(m_publishedVersion && table == m_publishedTable)
    {
        m_pendingTable.reset();
        return;
    }

    m_pendingTable = std::move(table);
}

std::vector<std::uint32_t> VoxelResidencyResources::TakeUploadedSlots()
{
    return std::exchange(m_uploadedSlots, {});
}

// ---------------------------------------------------------------------------------------------------------------------------
// Recording (inside Render)

void VoxelResidencyResources::CollectRetired(std::uint64_t completedFence)
{
    m_pages.Collect(completedFence);
    m_slots.Collect(completedFence);
    m_versions.Collect(completedFence);

    while(!m_retiredBuffers.empty() && m_retiredBuffers.front().fence <= completedFence)
    {
        m_retiredBuffers.front().buffer.Shutdown();
        m_retiredBuffers.pop_front();
    }
}

bool VoxelResidencyResources::Copy(ID3D12GraphicsCommandList* commandList, Buffer& destination, std::size_t offset, const void* data,
    std::size_t bytes, std::vector<Buffer*>& touched)
{
    const D3D12_GPU_VIRTUAL_ADDRESS source = m_upload.Allocate(data, bytes, UploadAlignment);

    if(source == 0)
    {
        return false;
    }

    if(std::find(touched.begin(), touched.end(), &destination) == touched.end())
    {
        destination.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        touched.push_back(&destination);
    }

    const UINT64 sourceOffset = source - m_upload.GetResource()->GetGPUVirtualAddress();
    commandList->CopyBufferRegion(destination.GetResource(), offset, m_upload.GetResource(), sourceOffset, bytes);
    m_bytesUploaded += bytes;
    return true;
}

bool VoxelResidencyResources::UploadRegions(ID3D12GraphicsCommandList* commandList, std::size_t& budget, std::vector<Buffer*>& touched)
{
    const std::uint32_t pagesPerChunk = m_binding.pagesPerChunk;
    const std::uint32_t slotsPerChunk = m_binding.slotsPerChunk;

    while(!m_pending.empty())
    {
        PendingRegion& region = m_pending.front();

        // Pages first, one copy per page, then the index slot; a region spans frames when the budget runs out.
        while(region.pagesDone < region.pages.size())
        {
            const std::size_t first = std::size_t{region.pagesDone} * BricksPerPage;
            const std::size_t count = (std::min)(std::size_t{BricksPerPage}, region.bricks.size() - first);
            const std::size_t bytes = count * sizeof(PackedBrick);
            const std::uint32_t page = region.pages[region.pagesDone];

            if(bytes > budget || !Copy(commandList, m_brickChunks[page / pagesPerChunk], std::size_t{page % pagesPerChunk} * PageBytes,
                &region.bricks[first], bytes, touched))
            {
                return false;
            }

            budget -= bytes;
            ++region.pagesDone;
        }

        if(!region.indexDone)
        {
            if(m_sliceBytes > budget || !Copy(commandList, m_indexChunks[region.slot / slotsPerChunk],
                std::size_t{region.slot % slotsPerChunk} * m_sliceBytes, region.index.data(), m_sliceBytes, touched))
            {
                return false;
            }

            budget -= m_sliceBytes;
            region.indexDone = true;
        }

        // Complete: recorded in this frame before any consumer, so a table published from now on may name it.
        m_regions[region.slot].uploaded = true;
        m_uploadedSlots.push_back(region.slot);
        m_pending.pop_front();
    }

    return true;
}

void VoxelResidencyResources::PublishTable(ID3D12GraphicsCommandList* commandList, std::uint64_t lastSubmissionFence, std::size_t& budget,
    std::vector<Buffer*>& touched)
{
    if(!m_pendingTable)
    {
        return;
    }

    // Consistent snapshot: every Populated entry names a region that is fully uploaded and not leaving.
    for(const TableEntry& entry : *m_pendingTable)
    {
        if(entry.State() != RegionState::Populated)
        {
            continue;
        }

        const auto region = m_regions.find(entry.IndexSlot());

        if(region == m_regions.end() || !region->second.uploaded || region->second.removing)
        {
            return;
        }
    }

    // A version no in-flight frame reads; with none free yet, publication waits a frame.
    const auto version = m_versions.Allocate(1);

    if(!version)
    {
        return;
    }

    const std::size_t entries = m_pendingTable->size();
    const std::size_t bytes = entries * sizeof(TableEntry);

    if(!Copy(commandList, m_table, std::size_t{version->front()} * bytes, m_pendingTable->data(), bytes, touched))
    {
        m_versions.Retire(*version, 0);
        return;
    }

    budget -= (std::min)(budget, bytes);

    // Frames up to lastSubmissionFence may still read the previous version; this frame reads the new one.
    if(m_publishedVersion)
    {
        const std::uint32_t previous = *m_publishedVersion;
        m_versions.Retire(std::span<const std::uint32_t>(&previous, 1), lastSubmissionFence);
    }

    m_publishedVersion = version->front();
    m_publishedTable = std::move(*m_pendingTable);
    m_pendingTable.reset();
    m_binding.tableBase = static_cast<std::uint32_t>(version->front() * entries);
    m_binding.valid = 1;
    ++m_binding.generation;
    ++m_tablesPublished;
}

void VoxelResidencyResources::RecordUploads(ID3D12GraphicsCommandList* commandList, unsigned int frameIndex, std::uint64_t lastSubmissionFence,
    std::uint64_t completedFence, std::uint64_t submissionLifetime)
{
    // A new command-context lifetime follows a GPU drain: nothing retired can still be read, and old fences mean nothing.
    if(submissionLifetime != m_submissionLifetime)
    {
        if(m_submissionLifetime != 0)
        {
            m_pages.CollectAll();
            m_slots.CollectAll();
            m_versions.CollectAll();
            CollectRetired((std::numeric_limits<std::uint64_t>::max)());
        }

        m_submissionLifetime = submissionLifetime;
    }

    CollectRetired(completedFence);

    if(!m_configured || !m_uploadReady || commandList == nullptr)
    {
        return;
    }

    // Begin has waited for this frame slot's previous submission, so its part of the ring is free to rewrite.
    m_upload.Reset(frameIndex);
    const std::size_t tableBytes = std::size_t{m_binding.window} * m_binding.window * m_binding.window * sizeof(TableEntry);
    std::size_t budget = m_settings.uploadBytesPerFrame - tableBytes;
    std::vector<Buffer*> touched;

    UploadRegions(commandList, budget, touched);
    budget += tableBytes;
    PublishTable(commandList, lastSubmissionFence, budget, touched);

    // Removed regions that the published table no longer names: frames up to lastSubmissionFence may still read them.
    std::set<std::uint32_t> referenced;

    for(const TableEntry& entry : m_publishedTable)
    {
        if(entry.State() == RegionState::Populated)
        {
            referenced.insert(entry.IndexSlot());
        }
    }

    for(auto region = m_regions.begin(); region != m_regions.end();)
    {
        if(!region->second.removing || referenced.contains(region->first))
        {
            ++region;
            continue;
        }

        const std::uint32_t slot = region->first;
        m_pages.Retire(region->second.pages, lastSubmissionFence);
        m_slots.Retire(std::span<const std::uint32_t>(&slot, 1), lastSubmissionFence);
        region = m_regions.erase(region);
    }

    // Copies done: every touched buffer returns to the read state before any pass of this frame.
    for(Buffer* buffer : touched)
    {
        buffer->Transition(commandList, ReadState);
    }
}

VoxelResidencyResources::Statistics VoxelResidencyResources::GetStatistics() const
{
    Statistics statistics;
    statistics.brickChunks = static_cast<std::uint32_t>(m_brickChunks.size());
    statistics.indexChunks = static_cast<std::uint32_t>(m_indexChunks.size());
    statistics.pagesFree = m_pages.FreeCount();
    statistics.pagesRetired = m_pages.RetiredCount();
    statistics.slotsFree = m_slots.FreeCount();
    statistics.slotsRetired = m_slots.RetiredCount();
    statistics.pendingRegions = m_pending.size();
    statistics.retiredBuffers = m_retiredBuffers.size();
    statistics.bytesUploaded = m_bytesUploaded;
    statistics.tablesPublished = m_tablesPublished;
    return statistics;
}
