#include "assets/ProjectTexturePreparationService.h"
#include "Project/ProjectPaths.h"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <stdexcept>

namespace
{
    using Service = ProjectTexturePreparationService;
    using Clock = std::chrono::steady_clock;
    void Require(bool valid, const std::string& message)
    { if(!valid) throw std::runtime_error(message); }

    void WriteTGA(const std::filesystem::path& path, unsigned size, unsigned char color)
    {
        std::vector<unsigned char> data(18 + size_t(size) * size * 3, color);
        std::fill_n(data.begin(), 18, 0);
        data[2] = 2;
        data[12] = data[14] = static_cast<unsigned char>(size);
        data[13] = data[15] = static_cast<unsigned char>(size >> 8);
        data[16] = 24;
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(data.data()), data.size());
        stream.close();
        Require(bool(stream), "Cannot write isolated TGA.");
    }

    bool CanWrite(const std::filesystem::path& path)
    {
        const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        if(handle == INVALID_HANDLE_VALUE) return false;
        CloseHandle(handle);
        return true;
    }

    struct Gate
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool reached = false, released = false;
        void Hold()
        {
            std::unique_lock lock(mutex);
            if(reached) return;
            reached = true;
            changed.notify_all();
            changed.wait(lock, [&] { return released; });
        }
        void Wait()
        {
            std::unique_lock lock(mutex);
            Require(changed.wait_for(lock, std::chrono::seconds(5), [&] { return reached; }), "Worker gate timed out.");
        }
        void Release()
        {
            std::lock_guard lock(mutex);
            released = true;
            changed.notify_all();
        }
    };
    struct ReleaseGate
    {
        std::shared_ptr<Gate> gate;
        ~ReleaseGate() { gate->Release(); }
    };

    Service::Completion Wait(Service& service, std::uint64_t ticket, const ProjectPaths& paths,
        const ProjectAssetRegistry& registry)
    {
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while(Clock::now() < deadline)
        {
            if(auto result = service.Poll(ticket, paths, registry)) return *result;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        throw std::runtime_error("Texture completion timed out.");
    }
}

int main()
{
    try
    {
        const auto root = std::filesystem::temp_directory_path() / ("EngineTexturePreparation-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(root / "Textures");
        const ProjectPaths paths(root);
        const auto image = root / "Textures/large.tga";
        WriteTGA(image, 2048, 31); // 12 MiB of source data; no real project assets.
        const auto id = AssetID::Generate();
        Require(id.has_value(), "Cannot generate fixture ID.");
        ProjectAssetRegistry registry;
        Require(registry.Register({*id, AssetType::Texture, "Textures/large.tga"}), "Registration failed.");
        Service service;
        std::string error;
        Require(service.Start({}, error), error);
        const auto request = [&]()
        {
            std::uint64_t ticket = 0;
            Require(service.Request(paths, registry, *id, ProjectTextureInterpretation::SRGBColor,
                {}, service.Generation(), ticket, error), error);
            return ticket;
        };
        auto gate = std::make_shared<Gate>();
        ReleaseGate release{gate};
        service.SetTestHook([gate](Service::TestStage stage) { if(stage == Service::TestStage::Pinned) gate->Hold(); });
        const auto first = request();
        gate->Wait();
        Require(service.GetStatistics().workerHashes == 0, "Request read/hash ran before the worker stage.");
        Require(!service.Poll(first, paths, registry) && service.GetStatistics().workerHashes == 0,
            "Pending Poll hashed or waited for the worker.");
        const auto duplicate = request();
        Require(service.GetStatistics().workerHashes == 0 && service.GetStatistics().deduplicated == 1,
            "Duplicate request hashed or failed to share the job.");
        Require(!CanWrite(image), "Worker source pin did not exclude mutation during preparation.");
        gate->Release();
        auto original = Wait(service, first, paths, registry);
        auto shared = Wait(service, duplicate, paths, registry);
        Require(original.prepared && shared.prepared == original.prepared && original.error.empty(), original.error);
        Require(service.GetStatistics().workerHashes == 2 && service.GetStatistics().decodes == 1,
            "Poll rehashed or duplicate decoded twice.");
        const auto statistics = service.GetStatistics();
        Require(service.ValidateSource(original, paths, registry, error) &&
            service.GetStatistics().workerHashes == statistics.workerHashes && !CanWrite(image),
            "Acceptance rehashed or allowed source mutation after preparation.");
        auto forged = original;
        forged.identity.digest = "forged";
        Require(!service.ValidateSource(forged, paths, registry, error), "Mutable completion bypassed immutable worker evidence.");
        service.Cancel(first);
        Require(!service.IsCurrent(original) && service.IsCurrent(shared) && !CanWrite(image),
            "Cancel invalidated a different subscriber or released its pin.");
        service.Cancel(duplicate);
        Require(CanWrite(image) && !service.ValidateSource(shared, paths, registry, error), "Last cancellation retained source pin or accepted stale data.");

        auto cached = Wait(service, request(), paths, registry);
        Require(cached.prepared == original.prepared && service.GetStatistics().cacheHits == 1 &&
            service.GetStatistics().decodes == 1, "Content-validated weak cache miss.");
        service.Cancel(cached.ticket);
        const auto previousTime = std::filesystem::last_write_time(image);
        WriteTGA(image, 2048, 93);
        std::filesystem::last_write_time(image, previousTime);
        auto changed = Wait(service, request(), paths, registry);
        Require(changed.prepared && changed.identity.digest != original.identity.digest &&
            changed.prepared != original.prepared && service.GetStatistics().decodes == 2,
            "Same-size/timestamp mutation bypassed content validation.");
        service.Invalidate();
        Require(!service.IsCurrent(changed) && CanWrite(image), "Invalidation retained stale ownership.");

        // A writable mapped view must also prevent a worker pin, even after its
        // original file and mapping handles have closed. No stale digest may escape.
        const auto writer = CreateFileW(image.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        Require(writer != INVALID_HANDLE_VALUE, "Mapped-writer fixture open failed.");
        const auto mapping = CreateFileMappingW(writer, nullptr, PAGE_READWRITE, 0, 0, nullptr);
        const auto view = mapping ? MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0) : nullptr;
        CloseHandle(writer);
        if(mapping) CloseHandle(mapping);
        Require(view != nullptr, "Mapped-writer fixture view failed.");
        struct Unmap { void* view; ~Unmap() { if(view) UnmapViewOfFile(view); } } unmap{view};
        auto mapped = Wait(service, request(), paths, registry);
        Require(!mapped.prepared && mapped.error.find("pin") != std::string::npos, "Writable mapping bypassed source pin.");
        UnmapViewOfFile(view);
        unmap.view = nullptr;

        auto renamed = Wait(service, request(), paths, registry);
        const auto alternate = root / "Textures/alternate.tga";
        WriteTGA(alternate, 16, 11);
        Require(registry.UpdatePath(*id, "Textures/alternate.tga") &&
            !service.ValidateSource(renamed, paths, registry, error), "Registry source-path replacement accepted stale data.");
        service.Cancel(renamed.ticket);
        Require(registry.UpdatePath(*id, "Textures/large.tga"), "Fixture path restore failed.");

        const auto removed = request();
        Require(registry.Unregister(*id), "Fixture unregister failed.");
        auto stale = Wait(service, removed, paths, registry);
        Require(!stale.prepared && !stale.error.empty(), "Deleted registration published a completion.");
        Require(registry.Register({*id, AssetType::Texture, "Textures/large.tga"}), "Fixture re-register failed.");
        auto final = Wait(service, request(), paths, registry);
        Require(final.prepared && service.ValidateSource(final, paths, registry, error), error);
        service.Shutdown();
        Require(!service.IsCurrent(final) && CanWrite(image) && final.prepared->width == 2048,
            "Shutdown failed to revoke source evidence or destroyed consumer-owned pixels.");
        std::cout << "PASS: no Request/Poll/acceptance hashing; 12 MiB source, worker pin, duplicate requests, weak cache, same-metadata mutation, cancellation, stale registry, shutdown.\n";
        return 0;
    }
    catch(const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
