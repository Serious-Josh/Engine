#include "Editor/TextureThumbnailCache.h"
#include "Renderer/DescriptorHeap.h"
#include <Windows.h>
#include <dxgi1_4.h>
#include <d3d12sdklayers.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

static void Check(bool condition, const char* message)
{
    if(!condition) throw std::runtime_error(message);
}

struct GPU
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> messages;
    UINT64 serial = 0;

    GPU()
    {
        ComPtr<ID3D12Debug> debug;
        if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        else std::cout << "D3D12 debug layer unavailable\n";
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
        Check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))), "WARP adapter");
        Check(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "D3D12 device");
        device.As(&messages);
        D3D12_COMMAND_QUEUE_DESC desc{};
        Check(SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))), "Queue");
        Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))), "Allocator");
        Check(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))), "List");
        list->Close();
        Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "Fence");
    }

    void Frame(TextureThumbnailCache& cache)
    {
        Check(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr)), "Reset recording");
        ++serial;
        cache.BeginRecording(serial);
        Check(cache.Record(list.Get(), serial), "Record thumbnails");
        Check(SUCCEEDED(list->Close()), "Close recording");
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        Check(SUCCEEDED(queue->Signal(fence.Get(), serial)), "Signal");
        cache.Submitted(serial, serial);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while(fence->GetCompletedValue() < serial)
        {
            Check(std::chrono::steady_clock::now() < deadline, "Fence timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        cache.SetCompletedFence(serial);
    }

    void Validate()
    {
        if(!messages) return;
        for(UINT64 index = 0; index < messages->GetNumStoredMessages(); ++index)
        {
            SIZE_T bytes = 0;
            messages->GetMessage(index, nullptr, &bytes);
            std::vector<unsigned char> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            messages->GetMessage(index, message, &bytes);
            if(message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            {
                std::cerr << message->pDescription << '\n';
                Check(false, "D3D12 validation error");
            }
        }
        std::cout << "D3D12 debug layer: no errors\n";
    }
};

static size_t ReadyCount(TextureThumbnailCache& cache, const std::vector<TextureThumbnailCache::Ticket>& tickets)
{
    size_t ready = 0;
    for(const auto ticket : tickets)
        if(const auto* preview = cache.Query(ticket))
            ready += preview->texture.GetResource() && !preview->invalidated && preview->error.empty();
    return ready;
}

static void WaitReady(GPU& gpu, TextureThumbnailCache& cache,
    const std::vector<TextureThumbnailCache::Ticket>& tickets)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while(std::chrono::steady_clock::now() < deadline)
    {
        const bool ready = ReadyCount(cache, tickets) == tickets.size();
        gpu.Frame(cache); // Submit every Query pin, including on the final iteration.
        if(ready) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for(const auto ticket : tickets)
        if(const auto* preview = cache.Query(ticket); preview && !preview->error.empty())
            std::cerr << preview->error << '\n';
    Check(false, "Timed out waiting for ready thumbnails");
}

int main()
{
    const auto root = fs::absolute("thumbnail-fixture-" + AssetID::Generate()->ToString());
    try
    {
        fs::create_directories(root / "Cache");
        // Keep disk-cache writes in this isolated fixture, not the real LOCALAPPDATA.
        SetEnvironmentVariableW(L"LOCALAPPDATA", (root / "Cache").c_str());
        ProjectPaths paths(root);
        Check(paths.EnsureDirectories(), "Fixture project directories");
        ProjectAssetRegistry registry;
        std::vector<AssetRecord> records;
        std::vector<unsigned char> image(54 + 8 * 8 * 3, 200);
        std::fill(image.begin(), image.begin() + 54, 0);
        const auto put32 = [&](size_t offset, unsigned int value)
        {
            for(size_t byte = 0; byte < 4; ++byte) image[offset + byte] = static_cast<unsigned char>(value >> (8 * byte));
        };
        image[0] = 'B'; image[1] = 'M';
        put32(2, static_cast<unsigned int>(image.size()));
        put32(10, 54); put32(14, 40); put32(18, 8); put32(22, 8);
        image[26] = 1; image[28] = 24;
        for(size_t index = 0; index < 80; ++index)
        {
            const auto path = fs::path("Textures") / ("test-" + std::to_string(index) + ".bmp");
            std::ofstream file(root / path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(image.data()), image.size());
            file.close();
            Check(file.good(), "Write fixture texture");
            records.push_back({*AssetID::Generate(), AssetType::Texture, path});
            Check(registry.Register(records.back()), "Register fixture texture");
            if(index < 20)
            {
                TextureThumbnailDiskCache::Image cached{8, 8, std::vector<unsigned char>(8 * 8 * 4, 200)};
                TextureThumbnailDiskCache::Store(TextureThumbnailDiskCache::Describe(paths, records.back(), root / path), cached);
            }
        }
        GPU gpu;
        DescriptorHeap heap;
        Check(heap.Initialize(gpu.device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 32, true), "Heap");
        std::vector<unsigned int> reserved;
        for(size_t index = 0; index < 20; ++index) reserved.push_back(heap.Allocate());
        TextureThumbnailCache cache;
        cache.Configure(gpu.device.Get(), heap);
        std::vector<TextureThumbnailCache::Ticket> tickets;
        for(size_t index = 0; index < 20; ++index)
            tickets.push_back(cache.Request(paths, registry, records[index], TextureThumbnailCache::Consumer::MaterialInspector));

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        size_t ready = 0, failed = 0;
        while(std::chrono::steady_clock::now() < deadline)
        {
            ready = failed = 0;
            for(const auto ticket : tickets)
            {
                if(const auto* preview = cache.Query(ticket))
                {
                    ready += preview->texture.GetResource() && !preview->invalidated && preview->error.empty();
                    failed += !preview->error.empty();
                }
            }
            gpu.Frame(cache);
            if(ready == 12 && failed == 8) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::cout << "Shared-heap reproduction: " << ready << " ready, " << failed << " waiting/failed; 20 descriptors reserved, 12 available\n";
        Check(ready == 12 && failed == 8, "Shared heap pressure did not produce bounded waiting");
        for(const auto ticket : tickets)
            if(const auto* preview = cache.Query(ticket); preview && !preview->error.empty())
                Check(preview->failure == TextureThumbnailCache::Failure::DescriptorCapacity && preview->retryable,
                    "Capacity failure classification");
        gpu.Frame(cache);
        Check(cache.GetDiagnostics().pixelBytes == 8 * 8 * 8 * 4, "Waiting uploads dropped prepared pixels");
        const auto attempts = cache.GetDiagnostics().uploadAttempts;
        for(int frame = 0; frame < 10; ++frame) gpu.Frame(cache);
        Check(cache.GetDiagnostics().uploadAttempts == attempts, "Retry busy loop");
        for(size_t index = 0; index < 8; ++index) heap.Free(reserved[index]);
        reserved.erase(reserved.begin(), reserved.begin() + 8);
        WaitReady(gpu, cache, tickets);
        Check(cache.GetDiagnostics().pixelPreparations == 20, "Capacity recovery prepared pixels again");
        Check(heap.GetAvailableCount() == 0, "Unexpected descriptor count after recovery");
        std::cout << "PASS: 20 thumbnails recover without pixel preparation or limit increases\n";

        // Navigation must reclaim idle descriptors below the 64-entry cache limit.
        for(size_t batch = 1; batch < 4; ++batch)
        {
            for(const auto ticket : tickets) cache.Release(ticket, TextureThumbnailCache::Consumer::MaterialInspector);
            tickets.clear();
            for(size_t index = batch * 20; index < (batch + 1) * 20; ++index)
                tickets.push_back(cache.Request(paths, registry, records[index], TextureThumbnailCache::Consumer::MaterialInspector));
            WaitReady(gpu, cache, tickets);
            Check(cache.GetDiagnostics().resident == 20 && heap.GetAvailableCount() == 0, "Navigation leaked descriptors");
        }
        Check(cache.GetDiagnostics().evictions >= 60, "Idle descriptors were not reclaimed below cache capacity");
        std::cout << "PASS: 80 cumulative thumbnails, navigation eviction, uncached decode/resize\n";

        // Query pins must protect invalidated allocations until their submission completes.
        cache.Query(tickets[0]);
        const auto freeBeforeInvalidation = heap.GetAvailableCount();
        cache.Invalidate(root / records[60].relativePath);
        Check(heap.GetAvailableCount() == freeBeforeInvalidation && cache.GetDiagnostics().pendingRetirement > 0,
            "Invalidation released a displayed resource prematurely");
        gpu.Frame(cache);
        WaitReady(gpu, cache, tickets);
        for(int refresh = 0; refresh < 3; ++refresh)
        {
            cache.ReleaseConsumer(TextureThumbnailCache::Consumer::MaterialInspector, true);
            for(size_t index = 0; index < tickets.size(); ++index)
                Check(tickets[index] == cache.Request(paths, registry, records[60 + index],
                    TextureThumbnailCache::Consumer::MaterialInspector), "Repeated request allocated a duplicate entry");
            WaitReady(gpu, cache, tickets);
            Check(cache.GetDiagnostics().resident == 20 && heap.GetAvailableCount() == 0, "Refresh leaked descriptors");
        }

        // Model the browser's move invalidation on an isolated source and preserve AssetID.
        auto moved = records[60];
        const auto oldPath = root / moved.relativePath;
        fs::create_directory(root / "Textures" / "Moved");
        moved.relativePath = fs::path("Textures") / "Moved" / oldPath.filename();
        fs::rename(oldPath, root / moved.relativePath);
        Check(registry.UpdatePath(moved.id, moved.relativePath), "Fixture move registry update");
        cache.Invalidate(oldPath);
        cache.Release(tickets[0], TextureThumbnailCache::Consumer::MaterialInspector);
        tickets[0] = cache.Request(paths, registry, moved, TextureThumbnailCache::Consumer::MaterialInspector);
        WaitReady(gpu, cache, tickets);
        Check(cache.GetDiagnostics().resident == 20 && heap.GetAvailableCount() == 0, "Move leaked old allocation");
        std::cout << "PASS: displayed invalidation fences, repeated refresh/requests, source move\n";

        // Permanent decode failure is cached until explicit invalidation, not retried each second.
        const AssetRecord invalid{*AssetID::Generate(), AssetType::Texture, "Textures/invalid.bmp"};
        { std::ofstream bad(root / invalid.relativePath); bad << "not an image"; }
        Check(registry.Register(invalid), "Register invalid fixture");
        const auto badTicket = cache.Request(paths, registry, invalid, TextureThumbnailCache::Consumer::MaterialInspector);
        const auto badDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while(cache.GetDiagnostics().terminal == 0 && std::chrono::steady_clock::now() < badDeadline)
        {
            gpu.Frame(cache);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Check(cache.GetDiagnostics().terminal == 1, "Decode failure not terminal");
        const auto preparations = cache.GetDiagnostics().pixelPreparations;
        const auto noRetryUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
        while(std::chrono::steady_clock::now() < noRetryUntil)
        {
            cache.Request(paths, registry, invalid, TextureThumbnailCache::Consumer::MaterialInspector);
            gpu.Frame(cache);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Check(cache.GetDiagnostics().pixelPreparations == preparations, "Invalid texture repeatedly decoded");
        { std::ofstream repaired(root / invalid.relativePath, std::ios::binary);
          repaired.write(reinterpret_cast<const char*>(image.data()), image.size()); }
        cache.Invalidate(root / invalid.relativePath);
        cache.Release(tickets.back(), TextureThumbnailCache::Consumer::MaterialInspector);
        tickets.back() = badTicket;
        WaitReady(gpu, cache, tickets);
        Check(cache.GetDiagnostics().terminal == 0, "Explicit invalidation did not recover terminal failure");

        // Hold the cache's completed-fence observation back: idle GPU references must stay alive.
        for(const auto ticket : tickets) cache.Release(ticket, TextureThumbnailCache::Consumer::MaterialInspector);
        cache.SetCompletedFence(0);
        const auto evictions = cache.GetDiagnostics().evictions;
        const auto pending = cache.Request(paths, registry, records[0], TextureThumbnailCache::Consumer::MaterialInspector);
        // Prepare without advancing the observed GPU completion fence.
        const auto prepareDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while(cache.GetDiagnostics().pixelBytes == 0 && std::chrono::steady_clock::now() < prepareDeadline)
        {
            cache.Request(paths, registry, records[0], TextureThumbnailCache::Consumer::MaterialInspector);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        gpu.Frame(cache);
        Check(cache.GetDiagnostics().evictions == evictions, "Evicted an allocation before its observed fence completed");
        WaitReady(gpu, cache, {pending});
        std::cout << "PASS: terminal-error recovery and fence-protected descriptor retirement\n";

        cache.ClearAfterGPUDrain();
        Check(heap.GetAvailableCount() == 20, "GPU-drained clear leaked thumbnail descriptors");
        // Entry admission is independently bounded even before GPU allocation.
        std::vector<TextureThumbnailCache::Ticket> admitted;
        for(size_t index = 0; index < TextureThumbnailCache::kCapacity; ++index)
            admitted.push_back(cache.Request(paths, registry, index == 60 ? moved : records[index],
                TextureThumbnailCache::Consumer::MaterialInspector));
        Check(cache.GetDiagnostics().entries == TextureThumbnailCache::kCapacity &&
            cache.Request(paths, registry, records[64], TextureThumbnailCache::Consumer::MaterialInspector) == 0,
            "Entry capacity was not bounded");
        cache.Release(admitted[0], TextureThumbnailCache::Consumer::MaterialInspector);
        Check(cache.Request(paths, registry, records[64], TextureThumbnailCache::Consumer::MaterialInspector) != 0,
            "Entry admission did not recover after release");
        cache.ClearAfterGPUDrain();
        for(const auto index : reserved) heap.Free(index);
        Check(heap.GetAvailableCount() == heap.GetCapacity(), "Shutdown leaked descriptors");
        gpu.Validate();
        heap.Shutdown();
        // The unique fixture is the only filesystem content removed by this test.
        fs::remove_all(root);
        return 0;
    }
    catch(const std::exception& exception)
    {
        std::cerr << exception.what() << "\nFixture retained: " << root << '\n';
        return 1;
    }
}
