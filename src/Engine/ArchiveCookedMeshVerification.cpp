#include "assets/DevelopmentCookedMeshProvider.h"
#include "Engine/Engine.h"
#include "Core/Log.h"
#include "Project/ProjectPaths.h"
#include "Scene/SceneSerializer.h"
#include "assets/ArchiveCookedMeshProvider.h"
#include "assets/AssetRegistrySerializer.h"
#include "assets/MaterialAssetSerializer.h"
#include "assets/ProjectAssetResolver.h"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace
{
    void RequireArchive(bool condition, const std::string& message)
    { if(!condition) throw std::runtime_error("Archive cooked diagnostic: " + message); }
}

bool Engine::VerifyArchiveCookedMeshPublication(const std::filesystem::path& scenePath)
{
    namespace fs = std::filesystem;
    using Service = ProjectMeshPreparationService;
    using Status = Service::Status;
    using SourceStatus = CookedMeshSource::Status;
    std::string error;
    auto project = fs::canonical(scenePath).parent_path();
    while(!fs::exists(project/"Assets.registry.json"))
    {
        RequireArchive(project != project.parent_path(),"Cannot locate input project registry.");
        project = project.parent_path();
    }
    ProjectAssetRegistry originalRegistry;
    RequireArchive(AssetRegistrySerializer::LoadFromFile(ProjectPaths(project).GetAssetRegistryPath(),originalRegistry,&error),error);
    SceneDocument document;
    RequireArchive(LoadSceneDocument(scenePath,document,&error),error);
    auto development = DevelopmentCookedMesh::Provider::Open(project,{});
    RequireArchive(bool(development.provider),std::string(development.message));
    auto found = std::find_if(document.objects.begin(),document.objects.end(),[&](const auto& object)
    {
        auto id = AssetID::ParseReference(object.meshReference);
        return id && development.provider->Lookup(*id).status == SourceStatus::Success;
    });
    RequireArchive(found != document.objects.end(),"Input scene has no published cooked mesh.");
    auto object = *found;
    const auto id = *AssetID::ParseReference(object.meshReference);
    auto payload = development.provider->Acquire(id);
    RequireArchive(bool(payload.lease),std::string(payload.message));
    const auto expected = payload.lease->Metadata();

    const auto fixture = fs::absolute(fs::path("build/Debug") /
        ("archive-cooked-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())));
    RequireArchive(fs::create_directory(fixture),"Fixture already exists.");
    const ProjectPaths paths(fixture);
    RequireArchive(paths.EnsureDirectories(&error),error);
    ProjectAssetRegistry registry;
    const auto* meshRecord = originalRegistry.Find(id);
    RequireArchive(meshRecord && registry.Register(*meshRecord),"Cannot register isolated mesh.");
    const auto source = ProjectAssetResolver::Resolve(ProjectPaths(project),originalRegistry,id);
    RequireArchive(source.status == AssetAvailability::Available,"Metadata scene activation still requires an FBX registration/source.");
    fs::create_directories((fixture/meshRecord->relativePath).parent_path());
    fs::copy_file(source.resolvedPath,fixture/meshRecord->relativePath);
    const auto material = *AssetID::Parse("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa");
    RequireArchive(registry.Register({material,AssetType::Material,"Materials/ArchiveFixture.material"}),"Cannot register fixture material.");
    RequireArchive(MaterialAssetSerializer::SaveToFile({},fixture/"Materials/ArchiveFixture.material",&error),error);
    object.materialReference = "builtin:default";
    object.materialSlotBindings.clear();
    object.materialSlotBindings[payload.lease->Metadata()->metadata.materialSlots.front().id] = material.ToReference();
    document.objects = {object}; document.id = "archive-cooked-fixture";
    const auto fixtureScene = paths.GetScenesDirectory()/"Archive.scene";
    RequireArchive(SaveSceneDocument(document,fixtureScene,&error),error);
    RequireArchive(AssetRegistrySerializer::SaveToFile(registry,paths.GetAssetRegistryPath(),&error),error);
    const auto toc = fixture/"mesh.toc", pak = fixture/"mesh.eap";
    EngineAssetPak::PreparedAsset asset{id,EngineAssetPak::Type::Mesh,CookedMesh::V1::Version,41,payload.lease->Bytes(),16};
    EngineAssetPak::Write(toc,pak,{&asset,1});
    RequireArchive(!fs::exists(fixture/"Cooked"),"Isolated fixture unexpectedly contains loose cooked files.");
    development.provider.reset(); // Source generation plays no part in the following preparation/publication.

    auto opened = ArchiveCookedMesh::Provider::Open(fixture,toc,pak);
    RequireArchive(bool(opened.provider),std::string(opened.message));
    auto provider = opened.provider; opened.provider.reset();
    RequireArchive(provider->EntryCount()==1 && provider->Entries().size()==1 &&
        provider->Lookup(id).evidence->contentGeneration==41,"Archive metadata enumeration lost identity/generation.");
    auto wait = [&](std::uint64_t ticket)
    {
        auto completion = m_meshPreparation.PollCooked(ticket);
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while(completion.status==Status::Pending && std::chrono::steady_clock::now()<deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2)); completion=m_meshPreparation.PollCooked(ticket);
        }
        RequireArchive(completion.status!=Status::Pending,"Background mesh preparation timed out.");
        return completion;
    };
    const auto metadataRequest = m_meshPreparation.RequestCooked(
        {provider,id,m_meshPreparation.Generation(),Service::PreparationMode::MetadataOnly},m_meshCPUCache);
    RequireArchive(metadataRequest.status==Status::Accepted,"Metadata request rejected: "+metadataRequest.error);
    auto metadata = wait(metadataRequest.ticket);
    RequireArchive(metadata.status==Status::Available && !metadata.geometry && metadata.metadata &&
        metadata.metadata->metadata==expected->metadata,"Metadata-first worker result differs or eagerly decoded geometry.");
    const auto fullRequest = m_meshPreparation.RequestCooked({provider,id,m_meshPreparation.Generation()},m_meshCPUCache);
    RequireArchive(fullRequest.status==Status::Accepted,"Geometry request rejected: "+fullRequest.error);
    auto full = wait(fullRequest.ticket);
    RequireArchive(full.status==Status::Available && full.geometry,"On-demand archived geometry failed.");
    const auto reencoded = CookedMesh::Encode(*full.geometry,{});
    RequireArchive(reencoded.status==CookedMesh::Status::Success &&
        std::equal(reencoded.payload.begin(),reencoded.payload.end(),payload.lease->Bytes().begin(),payload.lease->Bytes().end()),
        "On-demand archived geometry differs from exact source payload.");
    const auto* ownedGeometry = full.geometry.get();
    provider.reset(); // Completion retains the source after caller closure.
    RequireArchive(m_meshPreparation.ValidateCookedCompletion(full,error)==Status::Available,"Caller closure invalidated completion.");
    provider = std::static_pointer_cast<const CookedMeshSource::ArchiveProvider>(full.provider);
    auto pin = provider->PinAcceptance(*full.identity.cooked);
    RequireArchive(pin.status==SourceStatus::Success && pin.lease,"Archive acceptance pin failed.");
    auto other = ArchiveCookedMesh::Provider::Open(fixture,toc,pak);
    RequireArchive(bool(other.provider),"Cannot open second snapshot.");
    auto forged = full; forged.provider = other.provider;
    RequireArchive(m_meshPreparation.ValidateCookedCompletion(forged,error)==Status::Stale &&
        other.provider->PinAcceptance(*full.identity.cooked).status==SourceStatus::Stale,"Cross-snapshot evidence was accepted.");
    other.provider.reset(); forged = {};
    const auto missing = *AssetID::Parse("00000000-0000-0000-0000-000000000000");
    RequireArchive(m_meshPreparation.RequestCooked({provider,missing,m_meshPreparation.Generation()},m_meshCPUCache).status==Status::Missing,
        "Missing archived asset was accepted.");
    auto stale = m_meshPreparation.RequestCooked({provider,id,m_meshPreparation.Generation()-1},m_meshCPUCache);
    RequireArchive(stale.status==Status::Stale,"Stale service generation accepted.");
    m_meshPreparation.Cancel(full.ticket);
    RequireArchive(m_meshPreparation.ValidateCookedCompletion(full,error)==Status::Canceled &&
        full.geometry.get()==ownedGeometry && full.geometry->metadata==expected->metadata,"Cancellation lost geometry ownership or authorization.");
    m_meshPreparation.Cancel(metadata.ticket);
    full = {}; metadata = {}; pin = {};

    // A valid index with corrupt payload must open lazily, then fail on worker acquisition.
    EngineAssetPak::Write(fixture/"corrupt.toc",fixture/"corrupt.eap",{&asset,1});
    {
        std::fstream file(fixture/"corrupt.eap",std::ios::binary|std::ios::in|std::ios::out);
        file.seekg(64); char byte{}; file.read(&byte,1); byte^=1; file.seekp(64); file.write(&byte,1);
        RequireArchive(bool(file),"Cannot corrupt isolated payload.");
    }
    auto corrupt = ArchiveCookedMesh::Provider::Open(fixture,fixture/"corrupt.toc",fixture/"corrupt.eap");
    RequireArchive(bool(corrupt.provider),"Open eagerly inspected a mesh payload.");
    auto corruptRequest = m_meshPreparation.RequestCooked(
        {corrupt.provider,id,m_meshPreparation.Generation(),Service::PreparationMode::MetadataOnly},m_meshCPUCache);
    RequireArchive(corruptRequest.status==Status::Accepted && wait(corruptRequest.ticket).status==Status::Corrupt,
        "Corrupt archived payload bypassed worker integrity validation.");
    corrupt.provider.reset();
    auto wrong = asset; wrong.type = EngineAssetPak::Type::Texture;
    EngineAssetPak::Write(fixture/"type.toc",fixture/"type.eap",{&wrong,1});
    auto type = ArchiveCookedMesh::Provider::Open(fixture,fixture/"type.toc",fixture/"type.eap");
    RequireArchive(type.provider && type.provider->Lookup(id).status==SourceStatus::Unsupported,"Incorrect entry type accepted.");
    wrong = asset; wrong.payloadVersion = 2;
    EngineAssetPak::Write(fixture/"version.toc",fixture/"version.eap",{&wrong,1});
    auto version = ArchiveCookedMesh::Provider::Open(fixture,fixture/"version.toc",fixture/"version.eap");
    RequireArchive(version.provider && version.provider->Lookup(id).status==SourceStatus::Unsupported,"Unsupported mesh payload version accepted.");
    bool replacementBlocked = false;
    try { EngineAssetPak::Write(toc,pak,{&asset,1}); }
    catch(const std::runtime_error&) { replacementBlocked = true; }
    RequireArchive(replacementBlocked && provider->CheckCurrent().status==SourceStatus::Success,"Pinned archive replacement was not excluded.");
    Log::Info("Archive CPU diagnostic passed: lazy enumeration, metadata-only worker, exact geometry, content generation, missing/corrupt/type/version, stale snapshot, cancellation and pinned replacement.");
    payload.lease.reset();

    // Same C.2 catalog upload, candidate validation, BLAS/TLAS adoption and final
    // publication coordinator; only the explicitly supplied CPU provider differs.
    // The manager/catalog borrow Engine's registry, never this stack-local fixture
    // registry. This command exits through checked shutdown after verification.
    m_projectAssetRegistry = registry;
    m_demoSceneManager.SetProjectAssets(fixture,m_projectAssetRegistry);
    RequireArchive(VerifyCookedMeshPublication(fixtureScene,false,provider),"Existing checked residency publication rejected archive geometry.");
    RequireArchive(!fs::exists(fixture/"Cooked"),"Publication created or required loose cooked files.");
    Log::Info("Archive cooked mesh loaded and published from .eap without any loose cooked-mesh file. Fixture retained: "+fixture.string());
    return true;
}
