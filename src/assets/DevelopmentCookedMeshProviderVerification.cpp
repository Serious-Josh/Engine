#include "assets/DevelopmentCookedMeshProvider.h"
#include "Project/ProjectLocator.h"
#include "Renderer/Preview/MaterialThumbnailValidation.h"

#include <Windows.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
    namespace fs = std::filesystem;
    using Json = nlohmann::ordered_json;
    using namespace DevelopmentCookedMesh;

    void Require(bool condition, const char* message)
    {
        if(!condition) { throw std::runtime_error(message); }
    }

    Json Load(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        Require(bool(file), "Cannot read verification JSON.");
        return Json::parse(file);
    }

    void Write(const fs::path& path, std::span<const std::byte> bytes)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.close();
        Require(bool(file), "Cannot write verification fixture.");
    }

    void Write(const fs::path& path, const std::string& text)
    {
        Write(path, std::as_bytes(std::span(text.data(), text.size())));
    }

    std::string Digest(const fs::path& path)
    {
        MaterialThumbnailValidation validation;
        return validation.Digest(path);
    }
}

int VerifyDevelopmentCookedMeshProvider(const std::filesystem::path& requestedRoot)
{
    try
    {
        const auto root = requestedRoot.empty() ? ProjectLocator::GetDefaultProjectRoot() : requestedRoot;
        auto opened = Provider::Open(root, {});

        if(opened.status != Status::Success) { std::cerr << opened.message << '\n'; }
        Require(opened.status == Status::Success, "Cannot open actual published generation.");
        Require(opened.provider->CheckCurrent().status == Status::Success, "Actual mapping is stale.");
        const auto actualCooked = root / "Cooked";
        const auto actualManifest = Load(actualCooked / opened.provider->Mapping().publishedGeneration / "mesh-manifest.json");
        Require(!actualManifest.at("assets").empty(), "Provider verification requires at least one cooked entry.");
        Require(opened.provider->EntryCount() == actualManifest.at("assets").size(), "Entry count differs.");

        for(const auto& entry : actualManifest.at("assets"))
        {
            const auto asset = AssetID::Parse(entry.at("assetID").get<std::string>());
            Require(bool(asset), "Invalid actual AssetID.");
            const auto lookup = opened.provider->Lookup(*asset);
            const auto payload = opened.provider->Acquire(*asset);
            const auto metadata = opened.provider->ReadMetadata(*asset);
            Require(lookup.status == Status::Success && payload.status == Status::Success && metadata.status == Status::Success,
                "Actual lookup/payload/metadata retrieval failed.");
            const auto full = CookedMesh::Decode(payload.lease->Bytes(), {});
            Require(full.status == CookedMesh::Status::Success && full.evidence == lookup.evidence->payload &&
                metadata.metadata->evidence == full.evidence && metadata.evidence->mapping == opened.provider->Mapping(),
                "Actual full/metadata/provider evidence differs.");
            Require(Digest(actualCooked / opened.provider->Mapping().publishedGeneration / entry.at("payloadPath").get<std::string>()) ==
                entry.at("payloadSHA256").get<std::string>(), "Independent actual payload digest differs.");
        }

        const auto missing = *AssetID::Parse("00000000-0000-0000-0000-000000000000");
        Require(opened.provider->Acquire(missing).status == Status::Missing, "Missing AssetID was not classified.");
        std::cout << "Actual generation: " << opened.provider->EntryCount() << " AssetID lookups, payload digests, full/metadata agreement passed.\n";

        wchar_t executable[32768]{};
        const auto length = GetModuleFileNameW(nullptr, executable, 32768);
        Require(length && length < 32768, "Cannot locate development executable.");
        const auto fixture = fs::path(executable).parent_path() /
            ("provider-check-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        const auto cooked = fixture / "Cooked";
        const std::string generation1 = "generations/generation-0-0-1";
        const std::string generation2 = "generations/generation-0-0-2";
        fs::create_directories(cooked / generation1 / "Meshes");
        Json manifest = actualManifest;
        manifest["assets"] = Json::array({actualManifest.at("assets").front()});
        const auto asset = *AssetID::Parse(manifest.at("assets").front().at("assetID").get<std::string>());
        const auto relative = manifest.at("assets").front().at("payloadPath").get<std::string>();
        const auto actualPayload = opened.provider->Acquire(asset);
        Require(actualPayload.status == Status::Success, "Cannot acquire fixture bytes.");
        Write(cooked / generation1 / relative, actualPayload.lease->Bytes());
        const auto manifestPath = cooked / generation1 / "mesh-manifest.json";
        const auto pointer = cooked / "active-mesh-generation.json";

        const auto publish = [&](const Json& document, const std::string& generation = "generations/generation-0-0-1")
        {
            const auto path = cooked / generation / "mesh-manifest.json";
            Write(path, document.dump(2) + "\n");
            const Json activation = {{"schemaVersion", 1}, {"generation", generation}, {"manifestSHA256", Digest(path)}};
            Write(pointer, activation.dump(2) + "\n");
        };

        publish(manifest);
        auto snapshot = Provider::Open(fixture, {});
        Require(snapshot.status == Status::Success, "Source-independent fixture did not open.");
        auto retained = snapshot.provider->Acquire(asset);
        Require(retained.status == Status::Success, "Cannot acquire owned fixture lease.");

        auto acceptance = snapshot.provider->PinAcceptance(retained.lease->Evidence());
        Require(acceptance.status == Status::Success && acceptance.lease, "Cannot pin acceptance mapping.");
        const auto denied = [&](const fs::path& path, DWORD access)
        {
            HANDLE handle = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            const DWORD failure = GetLastError();
            if(handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); }
            if(handle != INVALID_HANDLE_VALUE || failure != ERROR_SHARING_VIOLATION)
            {
                std::cerr << "Acceptance pin probe: " << path.string() << ", access=" << access <<
                    ", opened=" << (handle != INVALID_HANDLE_VALUE) << ", error=" << failure << '\n';
            }
            return handle == INVALID_HANDLE_VALUE && failure == ERROR_SHARING_VIOLATION;
        };
        Require(denied(pointer, GENERIC_WRITE) && denied(pointer, DELETE) &&
            denied(manifestPath, GENERIC_WRITE) && denied(manifestPath, DELETE) && denied(cooked / generation1, DELETE),
            "Acceptance did not exclude pointer/manifest writes and ancestor replacement.");
        auto wrongEvidence = retained.lease->Evidence();
        ++wrongEvidence.mapping.mappingGeneration;
        Require(snapshot.provider->PinAcceptance(wrongEvidence).status == Status::Stale,
            "Acceptance substituted mapping evidence.");
        acceptance.lease.reset();
        fs::rename(cooked / generation1, cooked / "generations/released-pin");
        fs::rename(cooked / "generations/released-pin", cooked / generation1);

        const auto openStatus = [&](Status expected)
        {
            const auto result = Provider::Open(fixture, {});
            if(result.status != expected) { std::cerr << result.message << '\n'; }
            Require(result.status == expected, "Malformed generation classification differs.");
        };

        Write(pointer, R"({"schemaVersion":1,"generation":"../escape","manifestSHA256":"bad"})");
        openStatus(Status::Corrupt);
        publish(manifest);
        auto activation = Load(pointer);
        activation["schemaVersion"] = 2;
        Write(pointer, activation.dump());
        openStatus(Status::Unsupported);
        Write(pointer, R"({"schemaVersion":1,"schemaVersion":1,"generation":"generations/generation-0-0-1","manifestSHA256":"bad"})");
        openStatus(Status::Corrupt);

        publish(manifest);
        Write(manifestPath, "{}"); // Pointer hash unchanged.
        openStatus(Status::Corrupt);
        Require(snapshot.provider->PinAcceptance(retained.lease->Evidence()).status == Status::Stale,
            "Final acceptance accepted a changed manifest behind an unchanged pointer.");
        auto duplicate = manifest;
        duplicate["assets"].push_back(duplicate.at("assets").front());
        publish(duplicate);
        openStatus(Status::Corrupt);

        auto badPath = manifest;
        badPath["assets"][0]["payloadPath"] = "../outside.bin";
        publish(badPath);
        openStatus(Status::Corrupt);
        auto futureRecipe = manifest;
        futureRecipe["recipe"]["importerRevision"] = 999;
        publish(futureRecipe);
        openStatus(Status::Unsupported);
        auto oversized = manifest;
        oversized["assets"][0]["payloadBytes"] = UINT64_MAX;
        publish(oversized);
        openStatus(Status::AdmissionLimit);

        publish(manifest);
        auto damaged = std::vector<std::byte>(retained.lease->Bytes().begin(), retained.lease->Bytes().end());
        damaged.back() ^= std::byte{1};
        Write(cooked / generation1 / relative, damaged);
        Require(snapshot.provider->Acquire(asset).status == Status::Corrupt, "Payload corruption was accepted.");
        damaged.push_back(std::byte{0});
        Write(cooked / generation1 / relative, damaged);
        Require(snapshot.provider->Acquire(asset).status == Status::Corrupt, "Payload size mismatch was accepted.");
        Write(cooked / generation1 / relative, retained.lease->Bytes());

        HANDLE writer = CreateFileW((cooked / generation1 / relative).c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(writer != INVALID_HANDLE_VALUE, "Cannot establish verification sharing conflict.");
        const auto blockedRead = snapshot.provider->Acquire(asset).status;
        CloseHandle(writer);
        Require(blockedRead == Status::IOFailure, "Sharing conflict was not an explicit I/O failure.");

        // Opening a mapping does not read payloads; only acquisition needs the file.
        fs::rename(cooked / generation1 / relative, cooked / generation1 / "Meshes/held.bin");
        openStatus(Status::Success);
        Require(snapshot.provider->Acquire(asset).status == Status::Stale, "Unavailable mapped payload was not stale.");
        fs::rename(cooked / generation1 / "Meshes/held.bin", cooked / generation1 / relative);

        fs::create_directories(cooked / generation2 / "Meshes");
        Write(cooked / generation2 / relative, retained.lease->Bytes());
        publish(manifest, generation2);
        auto replacement = Provider::Open(fixture, {});
        Require(replacement.status == Status::Success && snapshot.provider->CheckCurrent().status == Status::Stale &&
            snapshot.provider->Mapping().mappingGeneration != replacement.provider->Mapping().mappingGeneration,
            "Replacement mapping freshness/token validation failed.");
        Require(snapshot.provider->Acquire(asset).status == Status::Success, "Replacement mutated the old mapping.");
        Require(snapshot.provider->PinAcceptance(retained.lease->Evidence()).status == Status::Stale,
            "Old generation acquired final acceptance after replacement.");
        snapshot.provider.reset();
        replacement.provider.reset();
        const auto stillValid = CookedMesh::Decode(retained.lease->Bytes(), {});
        Require(stillValid.status == CookedMesh::Status::Success && stillValid.evidence == retained.lease->Evidence().payload,
            "Acquired lease was invalidated by mapping replacement/destruction.");
        Require(retained.lease->Metadata()->evidence == stillValid.evidence, "Retained metadata evidence changed.");
        std::weak_ptr<const PayloadLease> weakLease = retained.lease;
        retained.lease.reset();
        Require(weakLease.expired(), "Provider unnecessarily retained released payload bytes.");

        std::cout << "Malformed pointer/manifest/duplicate/path/recipe/size/payload checks, lazy reads, missing/stale outcomes, "
            "acceptance write/delete/ancestor exclusion, stale acceptance rejection, "
            "and lease survival across replacement/destruction passed.\nFixture retained: " << fixture.string() << '\n';
        return 0;
    }
    catch(const std::exception& error)
    {
        std::cerr << "Development cooked provider verification failed: " << error.what() << '\n';
        return 1;
    }
}
