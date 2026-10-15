#include "Renderer/StructuredBuffer.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/UploadBuffer.h"
#include "Renderer/MaterialTablePublication.h"

#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <iostream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
static void Require(bool value, const char* message)
{
    if(!value) throw std::runtime_error(message);
}

int main()
{
    try
    {
        ComPtr<ID3D12Debug> debug;
        Require(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))), "Debug layer unavailable");
        debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<ID3D12Device> device;
        Require(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))), "WARP device failed");
        DescriptorHeap heap;
        Require(heap.Initialize(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, true), "Descriptor heap failed");
        StructuredBuffer first, second, retired, invalid;
        const std::array<unsigned int, 3> oldSlots{1, 2, 3}, newSlots{4, 4, 4};
        Require(!invalid.InitializeReadOnlyUpload(device.Get(), nullptr, sizeof(unsigned int), 3, heap) &&
            !invalid.InitializeReadOnlyUpload(device.Get(), oldSlots.data(), 0, 3, heap) &&
            !invalid.InitializeReadOnlyUpload(device.Get(), oldSlots.data(), SIZE_MAX, 3, heap) &&
            heap.GetAvailableCount() == 2, "Invalid buffer preparation consumed resources");
        Require(first.InitializeReadOnlyUpload(device.Get(), oldSlots.data(), sizeof(unsigned int), 3, heap) &&
            second.InitializeReadOnlyUpload(device.Get(), newSlots.data(), sizeof(unsigned int), 3, heap), "Immutable lookup allocation failed");
        Require(!invalid.InitializeReadOnlyUpload(device.Get(), oldSlots.data(), sizeof(unsigned int), 3, heap) &&
            !invalid.GetResource(), "Descriptor exhaustion leaked a lookup resource");
        const auto original = first.GetResource();
        const auto descriptor = first.GetDescriptorIndex();
        retired.Swap(first);
        first.Swap(second);
        Require(retired.GetResource() == original && retired.GetDescriptorIndex() == descriptor &&
            !second.GetResource() && first.GetResource() != original && heap.GetAvailableCount() == 0,
            "Lookup swap freed or aliased old descriptors");

        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12Fence> fence;
        D3D12_COMMAND_QUEUE_DESC queueDescription{};
        Require(SUCCEEDED(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue))) &&
            SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
            SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands))) &&
            SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "Readback submission setup failed");
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = D3D12_HEAP_TYPE_READBACK;
        auto description = original->GetDesc();
        description.Width *= 2;
        ComPtr<ID3D12Resource> readback;
        Require(SUCCEEDED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))), "Readback allocation failed");
        commands->CopyBufferRegion(readback.Get(), 0, retired.GetResource(), 0, sizeof(oldSlots));
        commands->CopyBufferRegion(readback.Get(), sizeof(oldSlots), first.GetResource(), 0, sizeof(newSlots));
        Require(SUCCEEDED(commands->Close()), "Readback close failed");
        ID3D12CommandList* lists[]{commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        Require(SUCCEEDED(queue->Signal(fence.Get(), 1)), "Readback fence failed");
        const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Require(event && SUCCEEDED(fence->SetEventOnCompletion(1, event)), "Readback event failed");
        Require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "Readback timed out");
        CloseHandle(event);
        void* mapped = nullptr;
        const D3D12_RANGE range{0, sizeof(oldSlots) + sizeof(newSlots)};
        Require(SUCCEEDED(readback->Map(0, &range, &mapped)), "Readback map failed");
        const auto* values = static_cast<const unsigned int*>(mapped);
        Require(values[0] == 1 && values[1] == 2 && values[2] == 3 &&
            values[3] == 4 && values[4] == 4 && values[5] == 4, "GPU lookup contents changed during replacement");
        const D3D12_RANGE written{0, 0};
        readback->Unmap(0, &written);
        retired.Shutdown();
        first.Shutdown();
        Require(heap.GetAvailableCount() == 2, "Completed lookup descriptors were not reclaimed");
        MaterialTablePublication<2> table;
        GPUMaterial record{};
        Require(table.SetSnapshot({record}) && table.Append(record, 2) && table.desired.size() == 2 &&
            !table.Append(record, 2) && !table.SetSnapshot({record}), "Append bounds or fixed-count revision contract failed");
        const std::array<unsigned int, 4> grownSlots{1, 2, 3, 9};
        UploadBuffer oldUpload, grownUpload;
        Require(first.InitializeStorage(device.Get(), sizeof(unsigned int), 3, heap) &&
            second.InitializeStorage(device.Get(), sizeof(unsigned int), 4, heap) &&
            oldUpload.Initialize(device.Get(), sizeof(oldSlots), 1) &&
            grownUpload.Initialize(device.Get(), sizeof(grownSlots), 1), "Growth storage preparation failed");
        const auto oldUploadResource = oldUpload.GetResource();
        oldUpload.Swap(grownUpload);
        Require(grownUpload.GetResource() == oldUploadResource && oldUpload.GetResource() != oldUploadResource,
            "Upload replacement lost ownership");
        first.Swap(second); // Only after the completed fence, just like slot acquisition.
        oldUpload.Reset(0);
        const auto address = oldUpload.Allocate(grownSlots.data(), sizeof(grownSlots), 4);
        Require(address != 0, "Grown upload has insufficient capacity");
        Require(SUCCEEDED(allocator->Reset()) && SUCCEEDED(commands->Reset(allocator.Get(), nullptr)), "Growth recording reset failed");
        commands->CopyBufferRegion(first.GetResource(), 0, oldUpload.GetResource(),
            address - oldUpload.GetResource()->GetGPUVirtualAddress(), sizeof(grownSlots));
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = first.GetResource();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commands->ResourceBarrier(1, &barrier);
        commands->CopyBufferRegion(readback.Get(), 0, first.GetResource(), 0, sizeof(grownSlots));
        Require(SUCCEEDED(commands->Close()), "Growth recording close failed");
        queue->ExecuteCommandLists(1, lists);
        Require(SUCCEEDED(queue->Signal(fence.Get(), 2)), "Growth submission fence failed");
        const HANDLE growthEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Require(growthEvent && SUCCEEDED(fence->SetEventOnCompletion(2, growthEvent)) &&
            WaitForSingleObject(growthEvent, 10000) == WAIT_OBJECT_0, "Growth completion timed out");
        CloseHandle(growthEvent);
        Require(SUCCEEDED(readback->Map(0, &range, &mapped)), "Growth readback map failed");
        values = static_cast<const unsigned int*>(mapped);
        Require(values[0] == 1 && values[1] == 2 && values[2] == 3 && values[3] == 9,
            "Growth failed to preserve prefix and append record");
        readback->Unmap(0, &written);
        first.Shutdown(); second.Shutdown(); oldUpload.Shutdown(); grownUpload.Shutdown();
        Require(heap.GetAvailableCount() == 2, "Growth descriptors leaked");
        ComPtr<ID3D12InfoQueue> info;
        Require(SUCCEEDED(device.As(&info)), "Debug queue unavailable");
        for(UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
        {
            SIZE_T bytes = 0;
            info->GetMessage(i, nullptr, &bytes);
            std::vector<char> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            info->GetMessage(i, message, &bytes);
            Require(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR &&
                message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION, message->pDescription);
        }
        heap.Shutdown();
        std::cout << "WARP immutable material lookup allocation, swap, GPU readback, capacity and cleanup passed.\n";
        return 0;
    }
    catch(const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
