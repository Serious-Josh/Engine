#include "Engine/BuildGame.h"
#include "Platform/Windows/WindowsWindow.h"
#include "Scene/SceneDocumentAdapter.h"
#include "Scene/SceneSerializer.h"
#include "assets/CookedScene.h"
#include "assets/MaterialAssetSerializer.h"
#include "assets/ModelImporter.h"

#include <nlohmann/json.hpp>

#include <bit>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

int VerifyScenePhysicsData();

namespace
{
using Json = nlohmann::json;
namespace fs = std::filesystem;

void Check(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

template <typename Operation>
void ExpectRejected(Operation operation, const char* message)
{
    bool rejected = false;
    try
    {
        operation();
    }
    catch (const std::runtime_error&)
    {
        rejected = true;
    }
    Check(rejected, message);
}

std::vector<std::byte> ReadBytes(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    Check(input.is_open(), "Cannot read diagnostic file: " + path.string());
    const auto length = input.tellg();
    Check(length >= 0 && length <= 16 * 1024 * 1024, "Unexpected diagnostic file length");
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Check(static_cast<bool>(input), "Cannot read complete diagnostic file");
    return bytes;
}

Json ReadJson(const fs::path& path)
{
    std::ifstream input(path);
    Check(input.is_open(), "Cannot read diagnostic JSON");
    return Json::parse(input);
}

void WriteText(const fs::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    output.close();
    Check(static_cast<bool>(output), "Cannot write diagnostic file: " + path.string());
}

struct Fixture
{
    fs::path root;
    fs::path project;
    SceneDocument document;
    ProjectAssetRegistry registry;
    AssetID meshID;
    std::vector<std::byte> meshBytes;
};

Fixture MakeFixture()
{
    const auto token = AssetID::Generate();
    Check(token.has_value(), "Cannot generate fixture identity");
    const auto root = fs::absolute(fs::path("build") / ("physics-persistence-" + token->ToString()));
    const auto project = root / "Project";
    fs::create_directories(project / "Meshes");
    fs::create_directories(project / "Materials");
    fs::create_directories(project / "Scenes");

    const auto meshID = *AssetID::Parse("11111111-1111-1111-1111-111111111111");
    const auto materialID = *AssetID::Parse("22222222-2222-2222-2222-222222222222");
    WriteText(project / "Meshes/Triangle.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    const auto geometry = ModelImporter::LoadFlattened(project / "Meshes/Triangle.obj", MeshUVOrientation::FlipV);
    const auto encoded = CookedMesh::Encode(geometry, {});
    Check(encoded.status == CookedMesh::Status::Success, std::string(encoded.message));
    Check(!geometry.metadata.materialSlots.empty(), "Fixture needs a stable mesh material slot");

    std::string error;
    Check(MaterialAssetSerializer::SaveToFile(MaterialAssetData{}, project / "Materials/Physics.material", &error), error);
    ProjectAssetRegistry registry;
    Check(registry.Register({meshID, AssetType::Mesh, "Meshes/Triangle.obj"}) &&
        registry.Register({materialID, AssetType::Material, "Materials/Physics.material"}), "Cannot register fixture assets");

    SceneDocument document;
    document.id = "physics-persistence-verification";
    document.name = "Physics Persistence";
    SceneObjectDocument plainObject;
    plainObject.id = "plain-object";
    document.objects.push_back(plainObject);

    for (const auto bodyType : {Physics::BodyType::Static, Physics::BodyType::Dynamic, Physics::BodyType::Kinematic})
    {
        SceneObjectDocument object;
        object.id = "physics-object-" + std::to_string(document.objects.size());
        object.name = "Authored Physics Box";
        object.transform.position = {1.25f, -2.5f, 3.75f};
        object.transform.rotation = {0.125f, 0.25f, 0.5f};
        object.transform.scale = {2.0f, 3.0f, 4.0f};
        object.meshReference = meshID.ToReference();
        object.materialReference = "builtin:default";
        object.materialSlotBindings.emplace(geometry.metadata.materialSlots.front().id, materialID.ToReference());
        auto& physics = object.physics.emplace();
        physics.bodyType = bodyType;
        physics.boxDimensions = {2.5f, 3.25f, 4.75f};
        physics.colliderLocalPositionOffset = {0.125f, -0.25f, 0.5f};
        physics.density = 997.25f;
        physics.friction = 0.625f;
        physics.restitution = 0.375f;
        document.objects.push_back(object);
    }
    return {root, project, document, registry, meshID, encoded.payload};
}

void ExpectLooseRejected(const fs::path& path, const Json& malformed, const SceneDocument& unchanged)
{
    WriteText(path, malformed.dump());
    SceneDocument loaded = unchanged;
    std::string error;
    Check(!LoadSceneDocument(path, loaded, &error) && !error.empty(), "Malformed loose scene accepted");
    Check(SceneDocumentsEqual(loaded, unchanged), "Rejected loose load changed destination document");
    Check(!LoadSceneDocumentForCooking(path, loaded, 1024 * 1024, &error) && !error.empty(),
        "Cooker accepted malformed loose scene");
    Check(SceneDocumentsEqual(loaded, unchanged), "Rejected cooker input changed destination document");
}

void VerifyLoosePersistence(const Fixture& fixture)
{
    const auto path = fixture.project / "Scenes/Physics.scene";
    std::string error;
    Check(SaveSceneDocument(fixture.document, path, &error), error);
    SceneDocument loaded;
    Check(LoadSceneDocument(path, loaded, &error), error);
    Check(SceneDocumentsEqual(fixture.document, loaded), "Physics loose round-trip changed authored data");
    Check(loaded.camera.aspectRatio == fixture.document.camera.aspectRatio, "Aspect ratio changed");
    Check(LoadSceneDocumentForCooking(path, loaded, 1024 * 1024, &error), error);
    Check(SceneDocumentsEqual(fixture.document, loaded), "Strict cooker lost loose physics");

    const Json serialized = ReadJson(path);
    Check(serialized.at("schemaVersion") == 2 && !serialized.at("objects")[0].contains("physics"),
        "New schema or absent physics encoding differs");
    const auto originalBytes = ReadBytes(path);
    auto invalidDocument = fixture.document;
    invalidDocument.objects.back().physics->density = 0.0f;
    Check(!SaveSceneDocument(invalidDocument, path, &error) && !error.empty(), "Invalid physics save accepted");
    Check(ReadBytes(path) == originalBytes, "Failed save changed an existing destination");

    auto withoutPhysics = fixture.document;
    for (auto& object : withoutPhysics.objects)
    {
        object.physics.reset();
    }
    const auto plainPath = fixture.root / "NoPhysics.scene";
    Check(SaveSceneDocument(withoutPhysics, plainPath, &error), error);
    Check(LoadSceneDocument(plainPath, loaded, &error) && SceneDocumentsEqual(withoutPhysics, loaded),
        "Nonphysics round-trip changed existing scene data");

    const auto malformedPath = fixture.root / "Malformed.scene";
    const auto rejectPhysics = [&](const Json& physics)
    {
        auto malformed = serialized;
        malformed["objects"][1]["physics"] = physics;
        ExpectLooseRejected(malformedPath, malformed, fixture.document);
    };
    const Json validPhysics = serialized["objects"][1]["physics"];
    for (auto field = validPhysics.begin(); field != validPhysics.end(); ++field)
    {
        auto incomplete = validPhysics;
        incomplete.erase(field.key());
        rejectPhysics(incomplete);
    }
    for (const auto& invalidObject : {Json(nullptr), Json::array(), Json("Box")})
    {
        rejectPhysics(invalidObject);
    }
    auto malformedPhysics = validPhysics;
    malformedPhysics["bodyType"] = "Unknown";
    rejectPhysics(malformedPhysics);
    malformedPhysics = validPhysics;
    malformedPhysics["colliderType"] = "Sphere";
    rejectPhysics(malformedPhysics);
    malformedPhysics = validPhysics;
    malformedPhysics["unknownField"] = 1;
    rejectPhysics(malformedPhysics);
    for (const char* dimension : {"width", "height", "depth"})
    {
        for (const auto& invalidValue : {Json(0), Json(-1), Json(nullptr), Json("1"), Json(1.0e39)})
        {
            malformedPhysics = validPhysics;
            malformedPhysics["boxDimensions"][dimension] = invalidValue;
            rejectPhysics(malformedPhysics);
        }
        malformedPhysics = validPhysics;
        malformedPhysics["boxDimensions"].erase(dimension);
        rejectPhysics(malformedPhysics);
    }
    malformedPhysics = validPhysics;
    malformedPhysics["boxDimensions"]["radius"] = 1;
    rejectPhysics(malformedPhysics);
    for (const auto& invalidOffset : {Json::array({0, 0}), Json::array({0, nullptr, 0}), Json::array({0, 1.0e39, 0})})
    {
        malformedPhysics = validPhysics;
        malformedPhysics["colliderLocalPositionOffset"] = invalidOffset;
        rejectPhysics(malformedPhysics);
    }
    for (const char* field : {"density", "friction", "restitution"})
    {
        for (const auto& invalidValue : {Json(-1), Json(nullptr), Json(true), Json(1.0e39)})
        {
            malformedPhysics = validPhysics;
            malformedPhysics[field] = invalidValue;
            rejectPhysics(malformedPhysics);
        }
    }
    malformedPhysics = validPhysics;
    malformedPhysics["density"] = 0;
    rejectPhysics(malformedPhysics);
    malformedPhysics = validPhysics;
    malformedPhysics["restitution"] = 1.01;
    rejectPhysics(malformedPhysics);

    auto malformed = serialized;
    malformed["unexpected"] = 1;
    ExpectLooseRejected(malformedPath, malformed, fixture.document);
    malformed = serialized;
    malformed["objects"][0]["unexpected"] = 1;
    ExpectLooseRejected(malformedPath, malformed, fixture.document);
    for (const auto& version : {Json(1), Json(3), Json(2.0), Json(-1), Json(true)})
    {
        malformed = serialized;
        malformed["schemaVersion"] = version;
        ExpectLooseRejected(malformedPath, malformed, fixture.document);
    }

    auto duplicatedKey = serialized.dump();
    const auto bodyTypePosition = duplicatedKey.find("\"bodyType\":");
    Check(bodyTypePosition != std::string::npos, "Cannot find physics enum in fixture JSON");
    duplicatedKey.insert(bodyTypePosition, "\"bodyType\":\"Static\",");
    WriteText(malformedPath, duplicatedKey);
    Check(!LoadSceneDocument(malformedPath, loaded, &error) && error.find("Duplicate") != std::string::npos,
        "Duplicate physics key accepted");
    Check(!LoadSceneDocumentForCooking(malformedPath, loaded, 1024 * 1024, &error),
        "Cooker accepted duplicate physics key");
}

void WriteUnsigned(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value, std::size_t width)
{
    Check(offset + width <= bytes.size(), "Diagnostic mutation is outside the fixture");
    for (std::size_t index = 0; index < width; ++index)
    {
        bytes[offset + index] = std::byte((value >> (8 * index)) & 0xff);
    }
}

void VerifyCookedPersistence(const Fixture& fixture)
{
    const CookedScene::MeshInput mesh{fixture.meshID, 1, fixture.meshBytes};
    const auto bytes = CookedScene::Cook(fixture.document, {&mesh, 1}, fixture.registry);
    const auto decoded = CookedScene::Decode(bytes);
    Check(decoded.payloadVersion == 2 && SceneDocumentsEqual(fixture.document, decoded.document),
        "Cooked physics round-trip changed authored data");
    Check(bytes == CookedScene::Cook(fixture.document, {&mesh, 1}, fixture.registry), "Repeated cooking changed bytes");
    Check(bytes == CookedScene::CookFile(fixture.project / "Scenes/Physics.scene", {&mesh, 1}, fixture.registry),
        "File cooker differs from document cooker");
    Check(decoded.meshes.size() == fixture.document.objects.size() && !decoded.meshes.front() && decoded.meshes.back(),
        "Cooked mesh presence changed");

    // The final object has physics and no environment follows it: its fixed v2 record is the last 48 bytes.
    constexpr std::size_t physicsRecordBytes = 48;
    const auto physicsOffset = bytes.size() - physicsRecordBytes;
    const auto rejectUnsigned = [&](std::size_t offset, std::uint32_t value)
    {
        auto malformed = bytes;
        WriteUnsigned(malformed, offset, value, 4);
        ExpectRejected([&] { (void)CookedScene::Decode(malformed); }, "Malformed cooked scene accepted");
    };
    rejectUnsigned(physicsOffset, 2);
    rejectUnsigned(physicsOffset + 4, 99);
    rejectUnsigned(physicsOffset + 8, 99);
    for (std::size_t field = 0; field < 9; ++field)
    {
        rejectUnsigned(physicsOffset + 12 + field * 4, 0x7fc00000); // Quiet NaN.
        rejectUnsigned(physicsOffset + 12 + field * 4, 0x7f800000); // Infinity.
    }
    for (const std::size_t field : {0, 1, 2, 6})
    {
        rejectUnsigned(physicsOffset + 12 + field * 4, 0);
    }
    rejectUnsigned(physicsOffset + 40, std::bit_cast<std::uint32_t>(-0.5f));
    rejectUnsigned(physicsOffset + 44, std::bit_cast<std::uint32_t>(1.5f));
    rejectUnsigned(8, 3); // Unsupported future payload version.
    rejectUnsigned(24, 0xffffffff); // Object count exceeds admission.
    rejectUnsigned(32, 0xffffffff); // Scene ID length exceeds remaining bytes.
    rejectUnsigned(physicsOffset, 0); // Claimed absence leaves trailing component bytes.
    auto truncated = bytes;
    truncated.pop_back();
    WriteUnsigned(truncated, 16, truncated.size(), 8);
    ExpectRejected([&] { (void)CookedScene::Decode(truncated); }, "Truncated physics record accepted");
    auto invalidDocument = fixture.document;
    invalidDocument.objects.back().physics->friction = -1.0f;
    ExpectRejected([&] { (void)CookedScene::Cook(invalidDocument, {&mesh, 1}, fixture.registry); },
        "Cooker accepted invalid authored physics");
}

void VerifyLegacyCompatibility(const Fixture& fixture)
{
    const auto fixtures = fs::path(ENGINE_SOURCE_ROOT) / "src/Physics/tests/fixtures";
    SceneDocument legacy;
    std::string error;
    Check(LoadSceneDocument(fixtures / "LegacySceneV1.scene", legacy, &error), error);
    Check(legacy.schemaVersion == 2 && !legacy.objects.front().physics, "Loose v1 migration changed physics defaults");
    SceneDocument cookerInput;
    Check(LoadSceneDocumentForCooking(fixtures / "LegacySceneV1.scene", cookerInput, 1024 * 1024, &error), error);
    Check(SceneDocumentsEqual(legacy, cookerInput), "Editor/cooker legacy migrations differ");
    const auto migratedPath = fixture.root / "MigratedV2.scene";
    Check(SaveSceneDocument(legacy, migratedPath, &error), error);
    Check(ReadJson(migratedPath).at("schemaVersion") == 2, "Legacy scene was not saved as v2");

    const auto legacyBytes = ReadBytes(fixtures / "LegacySceneV1.bin");
    const auto decoded = CookedScene::Decode(legacyBytes);
    Check(decoded.payloadVersion == 1 && SceneDocumentsEqual(legacy, decoded.document),
        "Independent cooked v1 fixture changed data");
    const auto sceneID = BuildGame::SceneIdentity(legacy.id);
    const EngineAssetPak::PreparedAsset scene{sceneID, EngineAssetPak::Type::Scene, 1, 7, legacyBytes, 16};
    EngineAssetPak::Write(fixture.root / "Legacy.toc", fixture.root / "Legacy.eap", {&scene, 1});
    const auto archive = CookedScene::OpenArchive(fixture.root / "Legacy.toc", fixture.root / "Legacy.eap", sceneID);
    Check(SceneDocumentsEqual(legacy, archive->metadata.document), "Version 1 archive failed admission");

    auto mismatchedEntry = scene;
    mismatchedEntry.payloadVersion = 2;
    EngineAssetPak::Write(fixture.root / "Mismatch.toc", fixture.root / "Mismatch.eap", {&mismatchedEntry, 1});
    ExpectRejected([&] { (void)CookedScene::OpenArchive(fixture.root / "Mismatch.toc", fixture.root / "Mismatch.eap", sceneID); },
        "Archive entry/payload version mismatch accepted");
    mismatchedEntry.payloadVersion = 3;
    EngineAssetPak::Write(fixture.root / "Future.toc", fixture.root / "Future.eap", {&mismatchedEntry, 1});
    ExpectRejected([&] { (void)CookedScene::OpenArchive(fixture.root / "Future.toc", fixture.root / "Future.eap", sceneID); },
        "Future archive scene version accepted");
    auto mismatchedPayload = legacyBytes;
    WriteUnsigned(mismatchedPayload, 8, 2, 4);
    ExpectRejected([&] { (void)CookedScene::Decode(mismatchedPayload); }, "Magic/version mismatch accepted");
}

BuildGame::Status WaitForBuild(BuildGame::Service& service)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(3);
    auto status = service.Poll();
    while (status.busy && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        status = service.Poll();
    }
    Check(!status.busy, "Focused Build Game verification timed out");
    return status;
}

void VerifyBuildGameArchive(const Fixture& fixture)
{
    std::cout << "Verifying Build Game physics archive content (offline shader compilation)...\n";
    const auto output = fixture.root / "Game";
    BuildGame::Request request{
        {"PhysicsPersistence", output, false}, fixture.document, fixture.registry, fixture.project,
        WindowsWindow::GetExecutableDirectory() / "assets/Shaders", fs::path(ENGINE_RUNTIME_DEPLOY_DIR)
    };
    BuildGame::Service service;
    std::string error;
    Check(service.Start(request, error), error);
    const auto status = WaitForBuild(service);
    Check(status.success, status.message);
    const auto tocBytes = ReadBytes(output / "Game.toc");
    const auto pakBytes = ReadBytes(output / "Game.eap");
    const auto sceneID = BuildGame::SceneIdentity(fixture.document.id);
    const auto archive = CookedScene::OpenArchive(output / "Game.toc", output / "Game.eap", sceneID);
    Check(archive->metadata.payloadVersion == 2 && SceneDocumentsEqual(fixture.document, archive->metadata.document),
        "Build Game archive lost physics or authored object data");
    Check(archive->ValidateReferences(&error), error);
    Check(fs::exists(output / "PhysicsPersistence.exe"), "Build Game executable was not deployed");

    auto invalidRequest = request;
    invalidRequest.document.objects.back().physics->density = -1.0f;
    Check(service.Start(std::move(invalidRequest), error), error);
    Check(!WaitForBuild(service).success, "Invalid physics build unexpectedly succeeded");
    Check(ReadBytes(output / "Game.toc") == tocBytes && ReadBytes(output / "Game.eap") == pakBytes,
        "Failed Build Game replaced a successful archive pair");

    // Remove the original project path before reopening through the packaged archive boundary.
    fs::rename(fixture.project, fixture.root / "RetainedProject");
    const auto archiveOnly = CookedScene::OpenArchive(output / "Game.toc", output / "Game.eap", sceneID);
    Check(SceneDocumentsEqual(fixture.document, archiveOnly->metadata.document),
        "Archive decoding depended on loose project files");
    std::cout << "Build Game archive verified without the original loose project: " << output.string() << '\n';
}
}

int VerifyScenePhysicsPersistence()
{
    try
    {
        Check(VerifyScenePhysicsData() == 0, "Existing component/history diagnostic failed");
        const auto fixture = MakeFixture();
        VerifyLoosePersistence(fixture);
        VerifyCookedPersistence(fixture);
        VerifyLegacyCompatibility(fixture);
        VerifyBuildGameArchive(fixture);
        std::cout << "PASS: physics loose/cooked v2, v1 compatibility, strict rejection, deterministic cook, Build Game archive content\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAIL: physics persistence: " << exception.what() << '\n';
        return 1;
    }
}
