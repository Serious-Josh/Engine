#include "UploadBuffer.h"
#include "Debug/MemoryRegistry.h"
#include "Core/Log.h"
#include "DescriptorHeap.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <string>
#include <utility>

void UploadBuffer::Swap(UploadBuffer& other) noexcept
{
    std::swap(m_resource, other.m_resource);
    std::swap(m_mappedData, other.m_mappedData);
    std::swap(m_capacityPerFrame, other.m_capacityPerFrame);
    std::swap(m_frameOffset, other.m_frameOffset);
    std::swap(m_offset, other.m_offset);
    std::swap(m_frameCount, other.m_frameCount);
    std::swap(m_device, other.m_device);
    std::swap(m_overflowPages, other.m_overflowPages);
    std::swap(m_frameIndex, other.m_frameIndex);
    std::swap(m_overflowPage, other.m_overflowPage);
    std::swap(m_overflowOffset, other.m_overflowOffset);
    std::swap(m_overflowBytesUsed, other.m_overflowBytesUsed);
}

static std::size_t AlignUp(
    std::size_t value,
    std::size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

bool UploadBuffer::Initialize(
    ID3D12Device* device,
    std::size_t capacityPerFrame,
    unsigned int frameCount)
{
    if (!device || capacityPerFrame == 0 || frameCount == 0)
    {
        return false;
    }

    const std::size_t totalCapacity = capacityPerFrame * frameCount;

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC resourceDescription{};
    resourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;

    resourceDescription.Width = totalCapacity;
    resourceDescription.Height = 1;
    resourceDescription.DepthOrArraySize = 1;
    resourceDescription.MipLevels = 1;

    resourceDescription.SampleDesc.Count = 1;
    resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT result =
        device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription,
                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_resource));

    if (FAILED(result))
    {
        return false;
    }

    MemoryRegistry::TrackResource(m_resource);

    result = m_resource->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedData));

    if (FAILED(result))
    {
        m_resource->Release();
        m_resource = nullptr;

        return false;
    }

    m_capacityPerFrame = capacityPerFrame;
    m_frameCount = frameCount;

    m_frameOffset = 0;
    m_offset = 0;

    m_device = device;
    m_overflowPages.assign(frameCount, {});
    m_frameIndex = 0;
    m_overflowPage = 0;
    m_overflowOffset = 0;
    m_overflowBytesUsed = 0;

    return true;
}

void UploadBuffer::Shutdown()
{
    // Callers shut down only after the GPU finished with every frame slot (as for the ring resource itself).
    for (auto& pages : m_overflowPages)
    {
        for (auto& page : pages)
        {
            page.resource->Unmap(0, nullptr);
            page.resource->Release();
        }
    }

    m_overflowPages.clear();
    m_device = nullptr;
    m_frameIndex = 0;
    m_overflowPage = 0;
    m_overflowOffset = 0;
    m_overflowBytesUsed = 0;

    if (m_resource)
    {
        m_resource->Unmap(0, nullptr);

        m_resource->Release();
        m_resource = nullptr;
    }

    m_mappedData = nullptr;
    m_capacityPerFrame = 0;
    m_frameOffset = 0;

    m_offset = 0;
    m_frameCount = 0;
}

void UploadBuffer::Reset(unsigned int frameIndex)
{
    if (frameIndex >= m_frameCount)
    {
        return;
    }

    m_frameOffset = m_capacityPerFrame * frameIndex;
    m_offset = m_frameOffset;

    // The slot's overflow pages are reused from the start; their previous contents belong to a completed submission.
    m_frameIndex = frameIndex;
    m_overflowPage = 0;
    m_overflowOffset = 0;
    m_overflowBytesUsed = 0;
}

std::size_t UploadBuffer::GetOverflowBytesRetained() const noexcept
{
    std::size_t bytes = 0;

    for (const auto& pages : m_overflowPages)
    {
        for (const auto& page : pages)
        {
            bytes += page.capacity;
        }
    }

    return bytes;
}

D3D12_GPU_VIRTUAL_ADDRESS UploadBuffer::AllocateOverflow(
    const void* data,
    std::size_t size,
    std::size_t alignment,
    UploadLocation& location)
{
    if (!m_device || m_frameIndex >= m_overflowPages.size())
    {
        return 0;
    }

    auto& pages = m_overflowPages[m_frameIndex];

    // Continue in the current page, then in the slot's later retained pages.
    while (m_overflowPage < pages.size())
    {
        OverflowPage& page = pages[m_overflowPage];
        const std::size_t alignedOffset = AlignUp(m_overflowOffset, alignment);

        if (alignedOffset + size <= page.capacity)
        {
            std::memcpy(page.mappedData + alignedOffset, data, size);
            m_overflowBytesUsed += alignedOffset + size - m_overflowOffset;
            m_overflowOffset = alignedOffset + size;
            location = {page.resource, alignedOffset};
            return page.resource->GetGPUVirtualAddress() + alignedOffset;
        }

        m_overflowBytesUsed += page.capacity - m_overflowOffset;
        ++m_overflowPage;
        m_overflowOffset = 0;
    }

    // A new page for this slot: at least the ring region's size, so a large scene settles after a page or two.
    const std::size_t capacity = AlignUp((std::max)(m_capacityPerFrame, size + alignment), 64u * 1024u);
    std::size_t retained = 0;

    for (const auto& page : pages)
    {
        retained += page.capacity;
    }

    if (retained + capacity > MaxOverflowBytesPerFrame)
    {
        return 0;
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC resourceDescription{};
    resourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDescription.Width = capacity;
    resourceDescription.Height = 1;
    resourceDescription.DepthOrArraySize = 1;
    resourceDescription.MipLevels = 1;
    resourceDescription.SampleDesc.Count = 1;
    resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    OverflowPage page;
    page.capacity = capacity;

    if (FAILED(m_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&page.resource))))
    {
        return 0;
    }

    if (FAILED(page.resource->Map(0, nullptr, reinterpret_cast<void**>(&page.mappedData))))
    {
        page.resource->Release();
        return 0;
    }

    MemoryRegistry::TrackResource(page.resource, MemoryCategory::UploadReadback);
    pages.push_back(page);
    Log::Info("Frame upload ring: frame slot " + std::to_string(m_frameIndex) + " grew an overflow page of " +
        std::to_string(capacity >> 10) + " KiB (" + std::to_string((retained + capacity) >> 10) + " KiB retained).");

    m_overflowPage = pages.size() - 1;
    m_overflowOffset = 0;
    return AllocateOverflow(data, size, alignment, location);
}

D3D12_GPU_VIRTUAL_ADDRESS UploadBuffer::Allocate(
    const void* data,
    std::size_t size,
    std::size_t alignment)
{
    UploadLocation location;
    return Allocate(data, size, alignment, location);
}

D3D12_GPU_VIRTUAL_ADDRESS UploadBuffer::Allocate(
    const void* data,
    std::size_t size,
    std::size_t alignment,
    UploadLocation& location)
{
    if (!m_resource ||
        !m_mappedData ||
        !data ||
        size == 0)
    {
        return 0;
    }

    if (alignment == 0)
    {
        return 0;
    }

    const std::size_t alignedOffset = AlignUp(m_offset, alignment);
    const std::size_t frameEnd = m_frameOffset + m_capacityPerFrame;

    if (alignedOffset + size > frameEnd)
    {
        // The ring region is full: continue in this slot's overflow pages (bounded, fence-safe, no waits).
        return AllocateOverflow(data, size, alignment, location);
    }

    std::memcpy(m_mappedData + alignedOffset, data, size);

    const D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = m_resource->GetGPUVirtualAddress() + alignedOffset;

    m_offset = alignedOffset + size;
    location = {m_resource, alignedOffset};

    return gpuAddress;
}

bool UploadBuffer::CreateRawSRV(
    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddress,
    std::size_t size,
    unsigned int& descriptorIndex) const
{
    descriptorIndex = UINT_MAX;

    if (!device || !m_resource || size == 0 || size % sizeof(std::uint32_t) != 0)
    {
        return false;
    }

    const D3D12_GPU_VIRTUAL_ADDRESS baseAddress = m_resource->GetGPUVirtualAddress();

    if (gpuAddress < baseAddress)
    {
        return false;
    }

    const UINT64 offset = gpuAddress - baseAddress;

    const UINT64 capacity = m_resource->GetDesc().Width;

    if (offset % sizeof(std::uint32_t) != 0 || offset > capacity || size > capacity - offset ||

        size / sizeof(std::uint32_t) > UINT_MAX)
    {
        return false;
    }

    const unsigned int index = descriptorHeap.Allocate();

    if (index == UINT_MAX)
    {
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};

    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    srv.Buffer.FirstElement = offset / sizeof(std::uint32_t);
    srv.Buffer.NumElements = static_cast<UINT>(size / sizeof(std::uint32_t));
    srv.Buffer.StructureByteStride = 0;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;

    device->CreateShaderResourceView(m_resource, &srv, descriptorHeap.GetCPUHandle(index));

    descriptorIndex = index;
    return true;
}
