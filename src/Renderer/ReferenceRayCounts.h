#pragma once

#include "Buffer.h"
#include "CommandContext.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

struct ReferenceRayCountResult
{
    bool valid = false;
    bool pending = false;
    std::uint64_t camera = 0, continuation = 0, shadow = 0, frame = 0;
    std::uint32_t width = 0, height = 0, spp = 0, depth = 0;
};

// Renderer-owned, single queue. Shutdown only after the renderer's checked GPU drain.
// Failed/uncertain submissions quarantine their slot until shutdown; no speculative reuse.
class ReferenceRayCounts
{
    static constexpr UINT Bytes = 24; // three (low, high) uint32 pairs
    struct Slot
    {
        Buffer counters;
        ID3D12Resource* readback = nullptr;
        std::uint64_t fence = 0, lifetime = 0, epoch = 0, generation = 0;
        bool recorded = false;
        bool copied = false;
        ReferenceRayCountResult metadata{};
    };
    std::array<Slot, CommandContext::FrameCount> slots{};
    ID3D12Resource* zero = nullptr;
    std::uint64_t epoch = 1;
    ReferenceRayCountResult result{};
    bool activeFrame = false;
    bool submittedFrame = false;

    static bool HostBuffer(ID3D12Device* device, D3D12_HEAP_TYPE type, ID3D12Resource** out)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = Bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(out)));
    }
public:
    void StartFrame() { activeFrame = false; submittedFrame = false; }
    void FinishFrame() { if(!activeFrame || !submittedFrame) Invalidate(); }
    const ReferenceRayCountResult& Get() const { return result; }
    void Invalidate() { ++epoch; result = {}; }
    void Poll(const CommandContext& context)
    {
        const auto completed = context.GetCompletedFenceValue();
        if(completed == (std::numeric_limits<std::uint64_t>::max)()) { Invalidate(); return; }
        for(auto& slot : slots)
        {
            if(!slot.fence || slot.lifetime != context.GetSubmissionLifetime() || slot.fence > completed) continue;
            if(slot.epoch == epoch)
            {
                void* data = nullptr;
                D3D12_RANGE range{0, Bytes};
                if(SUCCEEDED(slot.readback->Map(0, &range, &data)))
                {
                    std::uint32_t words[6];
                    std::memcpy(words, data, Bytes);
                    D3D12_RANGE written{0, 0};
                    slot.readback->Unmap(0, &written);
                    if(!result.valid || slot.metadata.frame > result.frame)
                    {
                        result = slot.metadata;
                        result.valid = true;
                        result.camera = words[0] | (std::uint64_t(words[1]) << 32);
                        result.continuation = words[2] | (std::uint64_t(words[3]) << 32);
                        result.shadow = words[4] | (std::uint64_t(words[5]) << 32);
                    }
                }
            }
            slot.fence = 0;
        }
        result.pending = false;
        for(const auto& slot : slots)
            result.pending |= slot.epoch == epoch && (slot.recorded || slot.fence != 0);
    }
    UINT Begin(ID3D12Device* device, DescriptorHeap& heap, ID3D12GraphicsCommandList* list,
        unsigned index, std::uint64_t generation, ReferenceRayCountResult metadata)
    {
        activeFrame = true;
        auto& slot = slots[index];
        if(slot.recorded || slot.fence) return UINT_MAX;
        if(!zero)
        {
            if(!HostBuffer(device, D3D12_HEAP_TYPE_UPLOAD, &zero)) return UINT_MAX;
            void* data = nullptr;
            D3D12_RANGE read{0, 0};
            if(FAILED(zero->Map(0, &read, &data))) { zero->Release(); zero = nullptr; return UINT_MAX; }
            std::memset(data, 0, Bytes);
            D3D12_RANGE written{0, Bytes};
            zero->Unmap(0, &written);
        }
        if(!slot.counters.GetResource() &&
            !slot.counters.InitializeGPUWritable(device, Bytes)) return UINT_MAX;
        if(slot.counters.GetUAVDescriptorIndex() == UINT_MAX && !slot.counters.CreateRawUAV(device, heap)) return UINT_MAX;
        if(!slot.readback && !HostBuffer(device, D3D12_HEAP_TYPE_READBACK, &slot.readback)) return UINT_MAX;
        slot.counters.Transition(list, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(slot.counters.GetResource(), 0, zero, 0, Bytes);
        slot.counters.Transition(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        slot.recorded = true;
        slot.copied = false;
        slot.generation = generation;
        slot.epoch = epoch;
        slot.metadata = metadata;
        result.pending = true;
        return slot.counters.GetUAVDescriptorIndex();
    }
    void Copy(ID3D12GraphicsCommandList* list, unsigned index, std::uint64_t generation)
    {
        auto& slot = slots[index];
        if(!slot.recorded || slot.generation != generation) return;
        slot.copied = true;
        // UAV -> COPY_SOURCE transition orders all counter writes before the copy.
        slot.counters.Transition(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(slot.readback, 0, slot.counters.GetResource(), 0, Bytes);
        // End each submitted/discarded recording in the same tracked state.
        slot.counters.Transition(list, D3D12_RESOURCE_STATE_COMMON);
    }
    void Cancel(ID3D12GraphicsCommandList* list, unsigned index, std::uint64_t generation)
    {
        auto& slot = slots[index];
        if(!slot.recorded || slot.generation != generation) return;
        slot.counters.Transition(list, D3D12_RESOURCE_STATE_COMMON);
        slot.epoch = 0;
        activeFrame = false;
        Invalidate();
    }
    void Commit(unsigned index, std::uint64_t generation, const CommandContext& context)
    {
        auto& slot = slots[index];
        if(!slot.recorded || slot.generation != generation) return;
        submittedFrame = slot.copied;
        if(!slot.copied) slot.epoch = 0; // retire submitted clear work without publishing unread bytes
        slot.fence = context.GetLastSubmissionFenceValue();
        slot.lifetime = context.GetSubmissionLifetime();
        slot.recorded = false;
    }
    void Abort(unsigned index, std::uint64_t generation) // after this recording was discarded
    {
        auto& slot = slots[index];
        if(slot.recorded && slot.generation == generation)
        {
            slot.recorded = false;
            slot.counters.RestoreStateAfterAbort(D3D12_RESOURCE_STATE_COMMON);
        }
        Invalidate();
    }
    void Shutdown()
    {
        for(auto& slot : slots)
        {
            slot.counters.Shutdown();
            if(slot.readback) slot.readback->Release();
            slot.readback = nullptr;
            slot.fence = slot.lifetime = 0;
            slot.recorded = false;
        }
        if(zero) zero->Release();
        zero = nullptr;
        Invalidate();
    }
};
