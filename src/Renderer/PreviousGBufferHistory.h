#pragma once

#include "Renderer/Texture.h"

#include <cstdint>

class DescriptorHeap;
class GBuffer;

// RTXPT.2 R4: renderer-owned previous-frame G-buffer, used only by the path-traced modes (decision D4).
// Two slots, each holding a bit-exact copy of the final G-buffer: depth as R32_FLOAT (a CopyResource from
// the R32_TYPELESS depth buffer, same typeless group), surface and geometric normal as RGBA16F, material
// as RGBA8. Motion is not kept. Each texture has exactly one SRV for its lifetime.
//
// This class owns only the resources and the copy recording. Slot selection, validity, camera snapshots
// and submission tracking belong to the Renderer (R4.3). Lifetime follows the WaitForGPU model: Resize
// and Shutdown release immediately, so the caller guarantees the queue is idle.
class PreviousGBufferHistory
{
    public:
    static constexpr unsigned int SlotCount = 2;

    // Depth 4 + surface 8 + geometric normal 8 + material 4 bytes.
    static constexpr std::uint64_t BytesPerPixelPerSlot = 24;

    // Copies rest here so future shader consumers need no transition before reading.
    static constexpr D3D12_RESOURCE_STATES RestingState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    struct Slot
    {
        Texture depth;
        Texture surface;
        Texture geometricNormal;
        Texture material;
    };

    PreviousGBufferHistory() = default;

    PreviousGBufferHistory(const PreviousGBufferHistory&) = delete;
    PreviousGBufferHistory& operator=(const PreviousGBufferHistory&) = delete;

    bool Initialize(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap,
        unsigned int width,
        unsigned int height);

    // Recreates both slots at the new extent; does nothing while unallocated. Caller guarantees the queue
    // is idle. A failure leaves the history unallocated.
    bool Resize(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap,
        unsigned int width,
        unsigned int height);

    void Shutdown();

    // Records a copy of the final G-buffer into the given slot. The G-buffer targets return to the states
    // they had before the call; the slot returns to RestingState. Records nothing and returns false when
    // unallocated, the slot is out of range or the G-buffer extent differs.
    bool RecordCopy(
        ID3D12GraphicsCommandList* commandList,
        GBuffer& gBuffer,
        unsigned int slot);

    bool IsInitialized() const noexcept
    {
        return m_initialized;
    }

    const Slot& GetSlot(unsigned int slot) const
    {
        return m_slots[slot];
    }

    unsigned int GetWidth() const noexcept
    {
        return m_width;
    }

    unsigned int GetHeight() const noexcept
    {
        return m_height;
    }

    // Bytes of GPU memory owned by both slots; 0 while unallocated.
    std::uint64_t GetResourceBytes() const noexcept;

    private:
    bool CreateSlot(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap,
        Slot& slot,
        unsigned int slotIndex,
        unsigned int width,
        unsigned int height);

    Slot m_slots[SlotCount];

    unsigned int m_width = 0;
    unsigned int m_height = 0;
    bool m_initialized = false;
};
