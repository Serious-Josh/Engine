#include "PreviousGBufferHistory.h"
#include "DescriptorHeap.h"
#include "GBuffer.h"
#include "Core/Log.h"

#include <string>

bool PreviousGBufferHistory::Initialize(
    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    Shutdown();

    if(!device || width == 0 || height == 0)
    {
        return false;
    }

    for(unsigned int slotIndex = 0; slotIndex < SlotCount; ++slotIndex)
    {
        if(!CreateSlot(device, descriptorHeap, m_slots[slotIndex], slotIndex, width, height))
        {
            Shutdown();
            return false;
        }
    }

    m_width = width;
    m_height = height;
    m_initialized = true;
    return true;
}

bool PreviousGBufferHistory::Resize(
    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    if(!m_initialized)
    {
        return true;
    }

    return Initialize(device, descriptorHeap, width, height);
}

void PreviousGBufferHistory::Shutdown()
{
    for(Slot& slot : m_slots)
    {
        slot.depth.Shutdown();
        slot.surface.Shutdown();
        slot.geometricNormal.Shutdown();
        slot.material.Shutdown();
    }

    m_width = 0;
    m_height = 0;
    m_initialized = false;
}

bool PreviousGBufferHistory::RecordCopy(
    ID3D12GraphicsCommandList* commandList,
    GBuffer& gBuffer,
    unsigned int slot)
{
    DepthBuffer& depth = gBuffer.GetDepth();
    RenderTarget& surface = gBuffer.GetSurface();
    RenderTarget& geometricNormal = gBuffer.GetGeometricNormal();
    RenderTarget& material = gBuffer.GetMaterial();

    const bool extentMatches = depth.GetWidth() == m_width && depth.GetHeight() == m_height;

    if(!m_initialized || !commandList || slot >= SlotCount || !extentMatches)
    {
        return false;
    }

    Slot& destination = m_slots[slot];

    // Callers record this between other G-buffer consumers; restore exactly what they left.
    const D3D12_RESOURCE_STATES depthState = depth.GetState();
    const D3D12_RESOURCE_STATES surfaceState = surface.GetState();
    const D3D12_RESOURCE_STATES geometricNormalState = geometricNormal.GetState();
    const D3D12_RESOURCE_STATES materialState = material.GetState();

    depth.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    surface.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    geometricNormal.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    material.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);

    destination.depth.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    destination.surface.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    destination.geometricNormal.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    destination.material.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST);

    // R32_TYPELESS depth into R32_FLOAT: same typeless group, whole subresource, so the bits are unchanged.
    commandList->CopyResource(destination.depth.GetResource(), depth.GetResource());
    commandList->CopyResource(destination.surface.GetResource(), surface.GetResource());
    commandList->CopyResource(destination.geometricNormal.GetResource(), geometricNormal.GetResource());
    commandList->CopyResource(destination.material.GetResource(), material.GetResource());

    destination.depth.Transition(commandList, RestingState);
    destination.surface.Transition(commandList, RestingState);
    destination.geometricNormal.Transition(commandList, RestingState);
    destination.material.Transition(commandList, RestingState);

    depth.Transition(commandList, depthState);
    surface.Transition(commandList, surfaceState);
    geometricNormal.Transition(commandList, geometricNormalState);
    material.Transition(commandList, materialState);
    return true;
}

std::uint64_t PreviousGBufferHistory::GetResourceBytes() const noexcept
{
    if(!m_initialized)
    {
        return 0;
    }

    const std::uint64_t pixels = static_cast<std::uint64_t>(m_width) * m_height;
    return pixels * BytesPerPixelPerSlot * SlotCount;
}

bool PreviousGBufferHistory::CreateSlot(
    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    Slot& slot,
    unsigned int slotIndex,
    unsigned int width,
    unsigned int height)
{
    struct SlotTexture
    {
        Texture& texture;
        DXGI_FORMAT format;
        const wchar_t* name;
    };

    const bool firstSlot = slotIndex == 0;

    const SlotTexture textures[] = {
        {slot.depth, DXGI_FORMAT_R32_FLOAT,
            firstSlot ? L"Previous GBuffer Depth 0" : L"Previous GBuffer Depth 1"},
        {slot.surface, DXGI_FORMAT_R16G16B16A16_FLOAT,
            firstSlot ? L"Previous GBuffer Surface 0" : L"Previous GBuffer Surface 1"},
        {slot.geometricNormal, DXGI_FORMAT_R16G16B16A16_FLOAT,
            firstSlot ? L"Previous GBuffer Geometric Normal 0" : L"Previous GBuffer Geometric Normal 1"},
        {slot.material, GBuffer::MaterialFormat,
            firstSlot ? L"Previous GBuffer Material 0" : L"Previous GBuffer Material 1"},
    };

    for(const SlotTexture& entry : textures)
    {
        if(!entry.texture.Initialize(device, width, height, 1, 1, entry.format, D3D12_RESOURCE_FLAG_NONE,
            RestingState) ||
            !entry.texture.CreateSRV(device, descriptorHeap))
        {
            Log::Error("Failed to create previous G-buffer slot " + std::to_string(slotIndex) + ".");
            return false;
        }

        entry.texture.GetResource()->SetName(entry.name);
    }

    return true;
}
