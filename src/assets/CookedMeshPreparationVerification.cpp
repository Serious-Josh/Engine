#include "assets/ProjectMeshPreparationService.h"
#include "assets/AssetRegistrySerializer.h"
#include "assets/MeshCookValidation.h"
#include "Scene/MeshResidency.h"
#include "Project/ProjectLocator.h"
#include "Renderer/Preview/MaterialThumbnailValidation.h"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
    namespace fs = std::filesystem;
    using Service = ProjectMeshPreparationService;
    using Status = Service::Status;
    using Json = nlohmann::ordered_json;

    void Require(bool condition, const char* message)
    {
        if(!condition) { throw std::runtime_error(message); }
    }

    Service::Completion Wait(Service& service, std::uint64_t ticket, const ProjectPaths* paths = nullptr,
        const ProjectAssetRegistry* registry = nullptr, ProjectMeshCPUCache* cache = nullptr)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

        while(std::chrono::steady_clock::now() < deadline)
        {
            auto result = paths ? service.Poll(ticket, *paths, *registry, *cache) : service.PollCooked(ticket);
            if(result.status != Status::Pending && result.status != Status::Retry) { return result; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        throw std::runtime_error("CPU preparation diagnostic timed out.");
    }

    Service::Completion Cooked(Service& service, const std::shared_ptr<const DevelopmentCookedMesh::Provider>& provider,
        const AssetID& asset, ProjectMeshCPUCache& cache, Service::PreparationMode mode = Service::PreparationMode::Geometry)
    {
        const auto request = service.RequestCooked({provider, asset, service.Generation(), mode}, cache);
        if(request.status != Status::Accepted && request.status != Status::Available)
        {
            std::cerr << "Submission " << asset.ToString() << " (mapping " << provider->Mapping().publishedGeneration << "): status "
                << int(request.status) << ": " << request.error << '\n';
        }
        Require(request.status == Status::Accepted || request.status == Status::Available, "Cooked submission failed.");
        auto result = Wait(service, request.ticket);
        if(result.status != Status::Available) { std::cerr << result.error << '\n'; }
        Require(result.status == Status::Available && service.IsCurrent(result), "Cooked completion failed/currentness differs.");
        return result;
    }

    void Write(const fs::path& path, std::span<const std::byte> bytes)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.close();
        Require(bool(file), "Cannot write CPU diagnostic fixture.");
    }

    void Write(const fs::path& path, const std::string& text)
    {
        Write(path, std::as_bytes(std::span(text.data(), text.size())));
    }

    Json Load(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return Json::parse(file);
    }

    std::string Digest(const fs::path& path)
    {
        MaterialThumbnailValidation validation;
        return validation.Digest(path);
    }
}

int VerifyCookedMeshPreparation(const std::filesystem::path& requestedRoot)
{
    try
    {
        const auto root = requestedRoot.empty() ? ProjectLocator::GetDefaultProjectRoot() : requestedRoot;
        const ProjectPaths paths(fs::canonical(root));
        ProjectAssetRegistry registry;
        std::string error;
        Require(AssetRegistrySerializer::LoadFromFile(paths.GetAssetRegistryPath(), registry, &error), "Cannot load diagnostic registry.");
        const auto opened = DevelopmentCookedMesh::Provider::Open(root, {});
        Require(opened.status == DevelopmentCookedMesh::Status::Success, "Cannot open published cooked mapping.");
        const auto provider = opened.provider;
        auto assets = registry.GetAssets(AssetType::Mesh);
        std::erase_if(assets, [&](const AssetRecord& asset)
        {
            if(provider->Lookup(asset.id).status == DevelopmentCookedMesh::Status::Missing)
            {
                std::cout << asset.id.ToString() << ": registered but not in the published cooked mapping; omitted from round trips.\n";
                return true;
            }

            return false;
        });
        Require(!assets.empty(), "Diagnostic requires registered meshes.");
        Service service;
        ProjectMeshCPUCache cache;
        Require(service.Start({}, error), "Cannot start existing preparation worker.");

        for(const auto& asset : assets)
        {
            auto cooked = Cooked(service, provider, asset.id, cache);
            Require(cooked.identity.sourceKind == MeshPreparationSourceKind::Cooked && cooked.identity.digest.empty() &&
                cooked.geometry && cooked.metadata && cooked.metadata->evidence == cooked.identity.cooked->payload,
                "Cooked source/payload/metadata evidence differs.");
            CookedMeshResidencyEvidence residency{cooked.provider, cooked.identity.cooked, cooked.metadata, cooked.current};
            Require(ValidateCookedMeshResidencyEvidence(residency, asset.id.ToReference(), &error),
                "Cooked catalog preparation evidence did not validate.");
            Require(!ValidateCookedMeshResidencyEvidence(residency, "builtin:sphere", &error),
                "Cooked evidence accepted a mismatched authored reference.");
            auto alteredMetadata = std::make_shared<CookedMesh::MetadataSnapshot>(*cooked.metadata);
            alteredMetadata->evidence.payloadSHA256[0] ^= 1;
            auto inconsistent = residency;
            inconsistent.metadata = std::move(alteredMetadata);
            Require(!ValidateCookedMeshResidencyEvidence(inconsistent, asset.id.ToReference(), &error),
                "Cooked evidence accepted metadata for a different payload.");
            Require(service.IntegrateCache(cooked, cache, error), "Cooked cache integration failed.");
            auto hit = Cooked(service, provider, asset.id, cache);
            Require(hit.cacheHit && hit.geometry.get() == cooked.geometry.get(), "Cooked CPU cache did not reuse geometry.");
            Require(service.IntegrateCache(hit, cache, error), "Cooked cache-hit integration failed.");
            const auto beforeMetadata = service.RetainedBytes();
            auto metadata = Cooked(service, provider, asset.id, cache, Service::PreparationMode::MetadataOnly);
            Require(!metadata.geometry && metadata.metadata->metadata == cooked.geometry->metadata &&
                metadata.metadata->evidence == cooked.identity.cooked->payload &&
                service.RetainedBytes() < beforeMetadata + 1024 * 1024, "Metadata-only preparation retained geometry/payload bytes.");

            const auto fbxRequest = service.Request(paths, registry, asset.id, service.Generation());
            Require(fbxRequest.status == Status::Accepted || fbxRequest.status == Status::Available, "Original FBX submission failed.");
            auto fbx = Wait(service, fbxRequest.ticket, &paths, &registry, &cache);
            Require(fbx.status == Status::Available && service.IsCurrent(fbx) &&
                MeshCookValidation::ExactlyEqual(*cooked.geometry, *fbx.geometry), "Cooked/FBX exact CPU geometry differs.");
            Require(fbx.identity.CacheKey() != cooked.identity.CacheKey() &&
                std::get<5>(fbx.identity.CacheKey()) == MeshPreparationSourceKind::FBX &&
                std::get<5>(cooked.identity.CacheKey()) == MeshPreparationSourceKind::Cooked, "FBX/cooked cache identities collide.");
            Require(service.IntegrateCache(fbx, cache, error), "Original FBX cache integration failed.");
            const auto fbxAgain = service.Request(paths, registry, asset.id, service.Generation());
            auto fbxHit = Wait(service, fbxAgain.ticket, &paths, &registry, &cache);
            Require(fbxHit.status == Status::Available && fbxHit.cacheHit, "Original FBX cache reuse failed.");

            ProjectMeshCPUCache bypass({1, 1});
            auto uncached = Cooked(service, provider, asset.id, bypass);
            Require(service.IntegrateCache(uncached, bypass, error) && bypass.GetStatistics().bypasses == 1 &&
                bypass.GetStatistics().retainedBytes == 0 && bypass.GetStatistics().pinnedOutsideCacheBytes > 0,
                "Cache bypass lost consumer allocation accounting.");
            service.Cancel(uncached.ticket);
            service.Cancel(cooked.ticket);
            Require(!ValidateCookedMeshResidencyEvidence(residency, asset.id.ToReference(), &error),
                "Canceled cooked preparation evidence remained eligible.");
            service.Cancel(hit.ticket);
            service.Cancel(metadata.ticket);
            service.Cancel(fbx.ticket);
            service.Cancel(fbxHit.ticket);
            std::cout << asset.id.ToString() << ": background cooked/FBX exact agreement, both cache hits, metadata-only and bypass passed.\n";
        }

        const auto first = assets.front().id;
        const auto canceled = service.RequestCooked({provider, first, service.Generation()}, cache);
        Require(canceled.status == Status::Accepted || canceled.status == Status::Available, "Cancellation submission failed.");
        service.Cancel(canceled.ticket);
        Require(service.PollCooked(canceled.ticket).status == Status::Canceled, "Cooked cancellation failed.");
        auto invalidated = Cooked(service, provider, first, cache);
        service.Invalidate();
        Require(!service.IsCurrent(invalidated) && !service.IntegrateCache(invalidated, cache, error), "Invalidated completion entered cache.");
        invalidated = {};
        cache.Clear();
        service.Shutdown();
        Require(service.RetainedBytes() == 0, "Released completion/cache allocations remained charged.");

        Service limited;
        Service::Limits limits;
        limits.sourceBytes = 1024 * 1024;
        limits.geometryBytes = 1024 * 1024;
        limits.preparationBytes = 2 * 1024 * 1024;
        limits.retainedBytes = limits.preparationBytes;
        Require(limited.Start(limits, error), "Cannot start memory-limit verification.");
        Require(limited.RequestCooked({provider, first, limited.Generation()}, cache).status == Status::AdmissionLimit,
            "Cooked peak allocation admission was bypassed.");
        limited.Shutdown();

        Service outstanding;
        limits = {};
        limits.requests = 1;
        Require(outstanding.Start(limits, error), "Cannot start shutdown verification.");
        const auto active = outstanding.RequestCooked({provider, first, outstanding.Generation()}, cache);
        Require(active.status == Status::Accepted, "Outstanding request submission failed.");
        Require(outstanding.RequestCooked({provider, first, outstanding.Generation()}, cache).status == Status::Retry,
            "Cooked subscriber bound was bypassed.");
        outstanding.Shutdown();
        Require(outstanding.PollCooked(active.ticket).status == Status::Canceled && outstanding.RetainedBytes() == 0,
            "Shutdown did not cancel/join/release outstanding work.");

        wchar_t executable[32768]{};
        Require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "Cannot locate diagnostic executable.");
        const auto fixture = fs::path(executable).parent_path() /
            ("preparation-check-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        const auto cookedRoot = fixture / "Cooked";
        const auto actualManifest = Load(root / "Cooked" / provider->Mapping().publishedGeneration / "mesh-manifest.json");
        auto manifest = actualManifest;
        manifest["assets"] = Json::array();

        for(const auto& entry : actualManifest.at("assets"))
        {
            if(entry.at("assetID").get<std::string>() == first.ToString()) { manifest["assets"].push_back(entry); }
        }

        Require(manifest.at("assets").size() == 1, "Cannot find fixture entry.");
        const auto payloadPath = manifest.at("assets").front().at("payloadPath").get<std::string>();
        const auto payload = provider->Acquire(first);
        Require(payload.status == DevelopmentCookedMesh::Status::Success, "Cannot acquire fixture payload.");

        const auto publish = [&](const std::string& generation, std::span<const std::byte> replacementPayload = {}, bool activate = true)
        {
            const auto directory = cookedRoot / generation;
            fs::create_directories(directory / "Meshes");
            const auto bytes = replacementPayload.empty() ? payload.lease->Bytes() : replacementPayload;
            Write(directory / payloadPath, bytes);
            manifest["assets"][0]["payloadSHA256"] = Digest(directory / payloadPath);
            manifest["assets"][0]["payloadBytes"] = bytes.size();
            Write(directory / "mesh-manifest.json", manifest.dump(2) + "\n");
            const Json activation = {{"schemaVersion", 1}, {"generation", generation},
                {"manifestSHA256", Digest(directory / "mesh-manifest.json")}};
            Write(activate ? cookedRoot / "active-mesh-generation.json" : directory / "pending-activation.json", activation.dump(2) + "\n");
        };

        // Observe actual worker phases without pausing or changing its policy.
        // One unconsumed completion supplies real bounded-queue backpressure.
        publish("generations/generation-0-0-10");
        const auto phaseProvider = DevelopmentCookedMesh::Provider::Open(fixture, {}).provider;
        Require(bool(phaseProvider), "Cannot open phase fixture.");
        Service phases;
        Service::Limits phaseLimits;
        phaseLimits.completed = 1;
        Require(phases.Start(phaseLimits, error), "Cannot start phase verification.");
        ProjectMeshCPUCache phaseCache;
        const auto completedRequest = phases.RequestCooked({phaseProvider, first, phases.Generation()}, phaseCache);
        Require(completedRequest.status == Status::Accepted, "Cannot submit phase blocker.");
        const auto queued = phases.RequestCooked({phaseProvider, first, phases.Generation(), Service::PreparationMode::MetadataOnly}, phaseCache);
        Require(queued.status == Status::Accepted, "Cannot queue second phase request before completed-job backpressure.");
        auto phaseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while(phases.ObserveDiagnosticStage(completedRequest.ticket) != Service::DiagnosticStage::Completed &&
            std::chrono::steady_clock::now() < phaseDeadline) { std::this_thread::yield(); }
        Require(phases.ObserveDiagnosticStage(completedRequest.ticket) == Service::DiagnosticStage::Completed,
            "Unconsumed completion phase not observed.");
        Require(phases.ObserveDiagnosticStage(queued.ticket) == Service::DiagnosticStage::Queued,
            "Completed-job backpressure did not hold queued work.");
        publish("generations/generation-0-0-11");
        Require(phases.ObserveDiagnosticStage(queued.ticket) == Service::DiagnosticStage::Queued &&
            phases.PollCooked(completedRequest.ticket).status == Status::Stale && Wait(phases, queued.ticket).status == Status::Stale,
            "Queued or completed-but-unconsumed generation replacement was accepted.");
        phases.Shutdown();

        publish("generations/generation-0-0-12");
        const auto runningProvider = DevelopmentCookedMesh::Provider::Open(fixture, {}).provider;
        Require(bool(runningProvider), "Cannot open running-phase fixture.");
        publish("generations/generation-0-0-13", {}, false);
        Require(phases.Start({}, error), "Cannot restart phase worker.");
        const auto running = phases.RequestCooked({runningProvider, first, phases.Generation()}, phaseCache);
        Require(running.status == Status::Accepted, "Cannot submit running-phase request.");
        phaseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while(phases.ObserveDiagnosticStage(running.ticket) == Service::DiagnosticStage::Queued &&
            std::chrono::steady_clock::now() < phaseDeadline) { std::this_thread::yield(); }
        Require(phases.ObserveDiagnosticStage(running.ticket) == Service::DiagnosticStage::Running,
            "Running worker phase was not observed; do not infer it from submission.");
        Require(MoveFileExW((cookedRoot / "generations/generation-0-0-13/pending-activation.json").c_str(),
            (cookedRoot / "active-mesh-generation.json").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
            "Cannot replace activation during running work.");
        Require(phases.ObserveDiagnosticStage(running.ticket) == Service::DiagnosticStage::Running,
            "Worker completed before replacement could be confirmed; running-stage test is inconclusive.");
        Require(Wait(phases, running.ticket).status == Status::Stale, "Running old generation was accepted.");
        phases.Shutdown();
        std::cout << "Observed queued, running and completed-but-unconsumed generation replacement rejection passed.\n";

        publish("generations/generation-0-0-1");
        const auto old = DevelopmentCookedMesh::Provider::Open(fixture, {}).provider;
        Require(bool(old), "Cannot open fixture generation.");
        Service replacement;
        Require(replacement.Start({}, error), "Cannot start replacement verification.");
        auto oldCompletion = Cooked(replacement, old, first, cache);
        Require(replacement.IntegrateCache(oldCompletion, cache, error), "Cannot cache fixture generation.");
        const auto inFlight = replacement.RequestCooked({old, first, replacement.Generation()}, cache);
        publish("generations/generation-0-0-2");
        CookedMeshResidencyEvidence oldResidency{oldCompletion.provider, oldCompletion.identity.cooked,
            oldCompletion.metadata, oldCompletion.current};
        Require(!ValidateCookedMeshResidencyEvidence(oldResidency, first.ToReference(), &error),
            "Superseded cooked catalog evidence remained eligible.");
        Require(Wait(replacement, inFlight.ticket).status == Status::Stale && !replacement.IsCurrent(oldCompletion) &&
            !replacement.IntegrateCache(oldCompletion, cache, error), "Superseded cooked generation was accepted.");
        const auto newer = DevelopmentCookedMesh::Provider::Open(fixture, {}).provider;
        auto newCompletion = Cooked(replacement, newer, first, cache);
        Require(newCompletion.cacheHit && newCompletion.geometry.get() == oldCompletion.geometry.get() &&
            newCompletion.identity.cooked->mapping != oldCompletion.identity.cooked->mapping,
            "Exact payload reuse across mapping generations failed.");

        auto corrupt = std::vector<std::byte>(payload.lease->Bytes().begin(), payload.lease->Bytes().end());
        corrupt.back() ^= std::byte{1};
        Write(cookedRoot / "generations/generation-0-0-2" / payloadPath, corrupt);
        const auto failedRequest = replacement.RequestCooked({newer, first, replacement.Generation()}, cache);
        Require(Wait(replacement, failedRequest.ticket).status == Status::Corrupt,
            "Corrupt cooked cache hit fell back/retried or bypassed payload integrity.");
        ImportedMesh changed = *oldCompletion.geometry;
        for(auto& vertex : changed.vertices) { vertex.position[0] += 0.25f; }
        changed.boundsMin.x += 0.25f;
        changed.boundsMax.x += 0.25f;
        const auto encoded = CookedMesh::Encode(changed, {});
        Require(encoded.status == CookedMesh::Status::Success, "Cannot encode changed valid cache fixture.");
        publish("generations/generation-0-0-3", encoded.payload);
        const auto changedProvider = DevelopmentCookedMesh::Provider::Open(fixture, {}).provider;
        Require(bool(changedProvider), "Cannot open changed valid payload mapping.");
        auto changedCompletion = Cooked(replacement, changedProvider, first, cache);
        Require(!changedCompletion.cacheHit && changedCompletion.geometry != oldCompletion.geometry &&
            changedCompletion.identity.CacheKey() != oldCompletion.identity.CacheKey() &&
            MeshCookValidation::ExactlyEqual(*changedCompletion.geometry, changed),
            "Changed valid payload reused stale cached geometry.");
        const auto geometryBytes = ProjectMeshCPUCache::GeometryBytes(*changedCompletion.geometry);
        const auto evictionLimit = geometryBytes + geometryBytes / 2;
        ProjectMeshCPUCache eviction({evictionLimit, 128 * 1024 * 1024});
        const auto pinned = eviction.StorePreparedVerified(oldCompletion.identity.CacheKey(), oldCompletion.geometry);
        const auto second = eviction.StorePreparedVerified(changedCompletion.identity.CacheKey(), changedCompletion.geometry);
        Require(pinned && second && eviction.GetStatistics().evictions == 1 &&
            eviction.GetStatistics().retainedBytes <= evictionLimit &&
            MeshCookValidation::ExactlyEqual(*pinned, *oldCompletion.geometry),
            "Bounded eviction invalidated a consumer lease or exceeded retention.");
        const auto missing = *AssetID::Parse("00000000-0000-0000-0000-000000000000");
        Require(replacement.RequestCooked({newer, missing, replacement.Generation()}, cache).status == Status::Missing,
            "Missing cooked asset classification differs.");
        replacement.Shutdown();
        std::cout << "Stale delivery/integration rejection, cross-generation cache reuse, changed valid payload isolation, bounded eviction with pinned consumers, terminal corruption, missing asset, cancellation, "
            "invalidation, admission, subscriber bounds and outstanding shutdown passed.\nFixture retained: " << fixture.string() << '\n';
        return 0;
    }
    catch(const std::exception& error)
    {
        std::cerr << "Cooked preparation verification failed: " << error.what() << '\n';
        return 1;
    }
}
