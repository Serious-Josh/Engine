#include "assets/ProjectMaterialPreparationService.h"
#include "assets/MaterialAssetSerializer.h"
#include <Windows.h>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

namespace
{
    using Service = ProjectMaterialPreparationService;
    void Check(bool ok, const std::string& message) { if(!ok) throw std::runtime_error(message); }
    bool Writable(const std::filesystem::path& path)
    {
        auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        if(handle == INVALID_HANDLE_VALUE) return false;
        CloseHandle(handle); return true;
    }
    struct Gate
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool reached = false, released = false;
        void Hold()
        {
            std::unique_lock lock(mutex);
            reached = true; changed.notify_all();
            changed.wait(lock, [&] { return released; });
        }
        void Wait()
        {
            std::unique_lock lock(mutex);
            Check(changed.wait_for(lock, std::chrono::seconds(5), [&] { return reached; }), "Worker pin gate timeout.");
        }
        void Release() { std::lock_guard lock(mutex); released = true; changed.notify_all(); }
        bool Reached() { std::lock_guard lock(mutex); return reached; }
    };
    struct ReleaseGate { std::shared_ptr<Gate> gate; ~ReleaseGate() { gate->Release(); } };
    Service::Result Wait(Service& service, std::uint64_t ticket, const ProjectPaths& paths,
        const ProjectAssetRegistry& registry)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while(std::chrono::steady_clock::now() < deadline)
        {
            auto result = service.Poll(ticket, paths, registry, 42);
            if(result.status != Service::Status::Pending && result.status != Service::Status::Retry) return result;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("Material preparation timed out.");
    }
}

int main()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("engine-material-preparation-" + std::to_string(GetCurrentProcessId()));
    try
    {
        std::filesystem::create_directories(root / "Materials");
        std::filesystem::create_directories(root / "Textures");
        struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(root, ec); } } cleanup{root};
        ProjectPaths paths(root);
        ProjectAssetRegistry registry;
        const auto material = *AssetID::Generate(), texture = *AssetID::Generate();
        const auto materialPath = root / "Materials/a.material";
        const auto texturePath = root / "Textures/a.tga";
        std::array<unsigned char, 18 + 16 * 16 * 3> pixels{};
        pixels[2] = 2; pixels[12] = pixels[14] = 16; pixels[16] = 24;
        { std::ofstream out(texturePath, std::ios::binary); out.write(reinterpret_cast<const char*>(pixels.data()), pixels.size()); }
        Check(registry.Register({material, AssetType::Material, "Materials/a.material"}), "Register material.");
        Check(registry.Register({texture, AssetType::Texture, "Textures/a.tga"}), "Register texture.");
        MaterialAssetData data;
        std::string error;
        Check(MaterialAssetSerializer::SaveToFile(data, materialPath, &error), error);
        ProjectTexturePreparationService textures;
        Check(textures.Start({}, error), error);
        Service service(textures);
        const auto request = [&]
        {
            std::uint64_t ticket = 0;
            Check(service.Request(paths, registry, material, 42, ticket, error) == Service::Status::Pending, error);
            return ticket;
        };
        auto gate = std::make_shared<Gate>();
        ReleaseGate release{gate};
        service.SetPinnedTestHook([gate] { gate->Hold(); });
        auto ticket = request();
        gate->Wait();
        Check(service.Poll(ticket, paths, registry, 42).status == Service::Status::Pending,
            "Poll waited for material worker.");
        Check(!Writable(materialPath) && !DeleteFileW(materialPath.c_str()), "Material mutation was not excluded during preparation.");
        gate->Release();
        service.SetPinnedTestHook({});
        auto result = Wait(service, ticket, paths, registry);
        Check(result.status == Service::Status::Ready && result.candidate, result.message);
        Check(textures.GetStatistics().workerHashes == 0 && !Writable(materialPath), "No-texture material evidence.");
        Check(service.Validate(*result.candidate, paths, registry, 42, error), error);
        Check(!service.Validate(*result.candidate, paths, registry, 43, error), "Wrong session accepted.");
        auto forged = *result.candidate;
        Check(!service.Validate(forged, paths, registry, 42, error), "Copied candidate authorized.");
        service.Cancel(ticket);
        Check(Writable(materialPath) && !service.Validate(*result.candidate, paths, registry, 42, error), "Cancel pin lifetime.");

        data.baseColorTexture = texture.ToReference();
        Check(MaterialAssetSerializer::SaveToFile(data, materialPath, &error), error);
        auto textureGate = std::make_shared<Gate>();
        ReleaseGate releaseTexture{textureGate};
        textures.SetTestHook([textureGate](auto stage)
        { if(stage == ProjectTexturePreparationService::TestStage::Pinned) textureGate->Hold(); });
        ticket = request();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while(!textureGate->Reached() && std::chrono::steady_clock::now() < deadline)
        {
            Check(service.Poll(ticket, paths, registry, 42).status == Service::Status::Pending, "Dependency scheduling failed.");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Check(textureGate->Reached() && !Writable(texturePath) && !DeleteFileW(texturePath.c_str()),
            "Texture mutation was not excluded during preparation.");
        Check(service.Poll(ticket, paths, registry, 42).status == Service::Status::Pending, "Poll waited for texture worker.");
        textureGate->Release(); textures.SetTestHook({});
        result = Wait(service, ticket, paths, registry);
        Check(result.status == Service::Status::Ready && result.candidate->textures[0] && !Writable(texturePath), result.message);
        service.Cancel(ticket);
        Check(Writable(texturePath), "Texture cancellation did not unpin.");
        result = {};

        data.roughnessTexture = data.metallicTexture = data.aoTexture = texture.ToReference();
        const auto normal = *AssetID::Generate();
        std::filesystem::copy_file(texturePath, root / "Textures/normal.tga");
        Check(registry.Register({normal, AssetType::Texture, "Textures/normal.tga"}), "Register normal texture.");
        data.normalTexture = normal.ToReference();
        Check(MaterialAssetSerializer::SaveToFile(data, materialPath, &error), error);
        const auto stats = textures.GetStatistics();
        ticket = request(); result = Wait(service, ticket, paths, registry);
        Check(result.status == Service::Status::Ready && result.candidate->textures[1] &&
            result.candidate->textures[2] && result.candidate->textures[3], result.message);
        Check(result.candidate->textures[2]->prepared == result.candidate->textures[3]->prepared &&
            textures.GetStatistics().deduplicated > stats.deduplicated, "Shared linear dependency did not deduplicate.");
        Check(!DeleteFileW(texturePath.c_str()) && !Writable(materialPath), "Completed source pins allowed mutation.");
        const auto moved = root / "Materials/moved.material";
        Check(!MoveFileExW(materialPath.c_str(), moved.c_str(), 0), "Pinned material moved.");
        service.Cancel(ticket); result = {};
        std::filesystem::rename(materialPath, moved);
        Check(registry.Unregister(material) && registry.Register({material, AssetType::Material, "Materials/moved.material"}), "Move registry.");
        ticket = request(); result = Wait(service, ticket, paths, registry);
        Check(result.status == Service::Status::Ready && result.candidate->assetID == material.ToString(), "Move lost stable identity.");
        Check(registry.Unregister(texture), "Unregister texture.");
        Check(!service.Validate(*result.candidate, paths, registry, 42, error), "Changed dependency registry accepted.");
        service.Cancel(ticket); result = {};
        std::filesystem::remove(texturePath);
        ticket = request(); result = Wait(service, ticket, paths, registry);
        Check(result.status == Service::Status::Failed, "Missing texture accepted.");
        service.Cancel(ticket);

        data = {};
        Check(MaterialAssetSerializer::SaveToFile(data, moved, &error), error);
        ticket = request(); result = Wait(service, ticket, paths, registry);
        Check(result.status == Service::Status::Ready, result.message);
        auto oldCandidate = result.candidate;
        const auto oldDigest = oldCandidate->digest;
        service.Cancel(ticket); result = {};
        const auto timestamp = std::filesystem::last_write_time(moved);
        data.roughness = 0.7f;
        Check(MaterialAssetSerializer::SaveToFile(data, moved, &error), error);
        std::filesystem::last_write_time(moved, timestamp);
        ticket = request(); result = Wait(service, ticket, paths, registry);
        Check(result.status == Service::Status::Ready && result.candidate->digest != oldDigest &&
            result.candidate->parameters.roughness == 0.7f &&
            !service.Validate(*oldCandidate, paths, registry, 42, error), "Material edit reused stale evidence.");
        Check(registry.Unregister(material), "Unregister material.");
        Check(service.Poll(ticket, paths, registry, 42).status == Service::Status::Stale && Writable(moved),
            "Material registration change retained candidate pin.");
        service.Cancel(ticket); result = {};
        Check(registry.Register({material, AssetType::Material, "Materials/moved.material"}), "Restore material.");
        std::filesystem::remove(moved);
        std::uint64_t missing = 0;
        Check(service.Request(paths, registry, material, 42, missing, error) == Service::Status::Failed,
            "Deleted material accepted.");
        Check(MaterialAssetSerializer::SaveToFile(data, moved, &error), error);

        // Other clients can exhaust the shared texture budget. Material Poll must
        // release partial subscriptions and report retry rather than wait.
        { std::ofstream out(texturePath, std::ios::binary); out.write(reinterpret_cast<const char*>(pixels.data()), pixels.size()); }
        Check(registry.Register({texture, AssetType::Texture, "Textures/a.tga"}), "Restore texture.");
        data.baseColorTexture = texture.ToReference();
        Check(MaterialAssetSerializer::SaveToFile(data, moved, &error), error);
        std::array<std::uint64_t, 16> occupied{};
        for(auto& dependency : occupied)
            Check(textures.Request(paths, registry, texture, ProjectTextureInterpretation::LinearData, {},
                textures.Generation(), dependency, error), error);
        ticket = request();
        const auto capacityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do
        {
            result = service.Poll(ticket, paths, registry, 42);
            if(result.status != Service::Status::Pending) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while(std::chrono::steady_clock::now() < capacityDeadline);
        Check(result.status == Service::Status::Retry, "Texture pressure did not report retry.");
        for(auto dependency : occupied) textures.Cancel(dependency);
        result = Wait(service, ticket, paths, registry);
        Check(result.status == Service::Status::Ready, result.message);
        service.Cancel(ticket); result = {};
        oldCandidate.reset();
        data = {};
        Check(MaterialAssetSerializer::SaveToFile(data, moved, &error), error);
        std::array<std::uint64_t, Service::Capacity> tickets{};
        for(auto& pending : tickets) pending = request();
        std::uint64_t extra = 0;
        Check(service.Request(paths, registry, material, 42, extra, error) == Service::Status::Retry && !extra, "Unbounded request capacity.");
        service.Invalidate();
        for(auto pending : tickets) Check(service.Poll(pending, paths, registry, 42).status == Service::Status::Stale, "Invalidate.");
        // Cancellation revokes authorization immediately, but consumer-held CPU
        // candidates remain charged until their last shared owner releases them.
        std::vector<std::shared_ptr<const Service::Candidate>> retained;
        for(size_t index = 0; index < Service::Capacity; ++index)
        {
            // Canceled queued work may still be returning to its worker boundary.
            const auto retryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            Service::Status status;
            do
            {
                status = service.Request(paths, registry, material, 42, ticket, error);
                if(status != Service::Status::Retry) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while(std::chrono::steady_clock::now() < retryDeadline);
            Check(status == Service::Status::Pending, error);
            result = Wait(service, ticket, paths, registry);
            Check(result.status == Service::Status::Ready, result.message);
            retained.push_back(result.candidate);
            service.Cancel(ticket); result = {};
        }
        Check(service.Request(paths, registry, material, 42, extra, error) == Service::Status::Retry,
            "Consumer-retained candidates escaped material capacity.");
        retained.clear();
        ticket = request(); result = Wait(service, ticket, paths, registry);
        Check(result.status == Service::Status::Ready, result.message);
        service.Shutdown();
        Check(Writable(moved) && !service.Validate(*result.candidate, paths, registry, 42, error), "Shutdown retained authorization.");
        textures.Shutdown();
        std::cout << "Unbound material preparation passed.\n";
        return 0;
    }
    catch(const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
}
