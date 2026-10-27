#include "assets/PrimitiveMeshAssets.h"
#include "assets/MaterialAssetSerializer.h"
#include "assets/AssetRegistrySerializer.h"
#include "Project/ProjectPaths.h"
#include "Scene/LegacyLightingMigration.h"
#include "Scene/SceneDocumentAdapter.h"
#include "Scene/SceneHierarchyEdits.h"
#include "Scene/FileSceneRuntime.h"
#include "Scene/SceneAssetResolver.h"
#include "Editor/ScenePrimitiveCreation.h"
#include "Renderer/Material.h"
#include <bit>
#include <limits>
#include <algorithm>
#include "Scene/SceneSerializer.h"
#include "assets/CookedScene.h"
#include "assets/CookedEnvironment.h"
#include "assets/EngineAssetPak.h"
#include "Renderer/BuiltinEnvironment.h"
#include "Renderer/EnvironmentMap.h"
#include "Renderer/ResourceUploader.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/FrameLighting.h"
#include "Renderer/Rtxdi/RtxdiLightBridge.h"
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace
{
int checks = 0;
void Check(bool ok, const std::string& message)
{
    ++checks;
    if(!ok) throw std::runtime_error(message);
}
bool Near(const Vector3& a, const Vector3& b)
{
    return (a-b).Length() < 2e-5f;
}
FrameLighting Lighting(const SceneDocument& document)
{
    SceneLightingSnapshot snapshot;
    std::string error;
    Check(BuildSceneLightingSnapshot(document, [](const auto& object) -> std::optional<Matrix4>
        { return object.transform.GetModelMatrix(); }, snapshot, &error), error);
    FrameLighting frame;
    // Deliberately bright input proves even an empty snapshot never falls back.
    DirectionalLight legacy;
    legacy.intensity = 100;
    Check(BuildFrameLighting(legacy, &snapshot, frame, &error), error);
    return frame;
}
std::vector<std::byte> HistoricalCooked(SceneDocument document, unsigned version, const DirectionalLight& sun)
{
    // Independent historical layout fixture: no objects, source scene header/camera followed
    // by the old nine-float scene sun. No reinterpretation of new component records.
    document.objects.clear();
    document.environmentReference.clear();
    ProjectAssetRegistry registry;
    auto bytes = CookedScene::Cook(document, {}, registry);
    bytes.resize(bytes.size() - sizeof(float));
    const float fields[]{sun.direction.x,sun.direction.y,sun.direction.z,sun.color.x,sun.color.y,sun.color.z,
        sun.intensity,sun.ambientStrength,sun.angularRadius};
    for(float field : fields)
    {
        const auto bits = std::bit_cast<std::uint32_t>(field);
        for(unsigned i = 0; i < 4; ++i) bytes.push_back(std::byte((bits >> (i*8)) & 255));
    }
    bytes[6] = std::byte('0' + version/10);
    bytes[7] = std::byte('0' + version%10);
    bytes[8] = std::byte(version);
    for(unsigned i = 0; i < 8; ++i) bytes[16+i] = std::byte((bytes.size() >> (i*8)) & 255);
    return bytes;
}
class MeshlessLightingResolver final : public SceneAssetResolver
{
    Material material;
public:
    Mesh* ResolveMesh(std::string_view) override { throw std::runtime_error("Unexpected lighting-fixture mesh."); }
    Material* ResolveMaterial(std::string_view) override { return &material; }
};
void CheckLightingMigration(const std::filesystem::path& output)
{
    using Json = nlohmann::json;
    std::string error;
    SceneDocument original;
    original.id = "directional-migration";
    Check(SaveSceneDocument(original, output/"template.scene", &error), error);
    Json historical;
    { std::ifstream file(output/"template.scene"); file >> historical; }
    historical.erase("lightingSettings");
    DirectionalLight sun;
    sun.direction = {0.3f,-0.8f,-0.5f};
    sun.color = {2.0f,-0.25f,0.7f}; // Outside both the old component color and intensity limits.
    sun.intensity = 2000000.0f;
    sun.angularRadius = 0.04f;
    sun.ambientStrength = 0.63f;
    historical["directionalLight"] = {{"direction",{sun.direction.x,sun.direction.y,sun.direction.z}},
        {"color",{sun.color.x,sun.color.y,sun.color.z}}, {"intensity",sun.intensity},
        {"angularRadius",sun.angularRadius}, {"ambientStrength",sun.ambientStrength}};
    ProjectAssetRegistry registry;
    SceneDocument migrated;
    for(unsigned version = 1; version <= 12; ++version)
    {
        historical["schemaVersion"] = version;
        const auto text = historical.dump(2);
        { std::ofstream file(output/"historical.scene"); file << text; }
        Check(LoadSceneDocument(output/"historical.scene", migrated, &error), error);
        std::string after;
        { std::ifstream file(output/"historical.scene"); after.assign(std::istreambuf_iterator<char>(file), {}); }
        Check(after == text, "Loading rewrote a historical scene on disk.");
        Check(migrated.objects.size() == 1 && migrated.objects[0].light.has_value(), "Legacy sun was not migrated exactly once.");
        Check(Near(migrated.objects[0].transform.position, {}), "Migrated sun is not at origin.");
        Check(migrated.objects[0].light->color.x == sun.color.x && migrated.objects[0].light->color.y == sun.color.y &&
            migrated.objects[0].light->intensity == sun.intensity, "Legacy radiometry was clamped or rescaled.");
        const auto frame = Lighting(migrated);
        Check(Near(frame.sun.direction, sun.direction.Normalized()) && frame.sun.angularRadius == sun.angularRadius &&
            frame.sun.ambientStrength == sun.ambientStrength, "Legacy direction/radius/ambient changed.");
        const auto source = MakeRtxdiSunSource(frame.sun);
        Check(source.cosAngularRadius == std::cos(sun.angularRadius), "RTXDI ignored component radius.");
        const auto oldBytes = HistoricalCooked(original, version, sun);
        const auto decoded = CookedScene::Decode(oldBytes);
        Check(decoded.meshes.size() == decoded.document.objects.size() && decoded.document.objects.size() == 1,
            "Historical cooked migration lost object/binding correspondence.");
        Check(SceneDocumentsEqual(decoded.document, migrated), "Loose/cooked migration disagrees.");
        const auto sceneID = *AssetID::Parse("00000000-0000-4000-8000-000000000002");
        const EngineAssetPak::PreparedAsset entry{sceneID,EngineAssetPak::Type::Scene,version,1,oldBytes,16};
        EngineAssetPak::Write(output/"legacy.toc",output/"legacy.eap",std::span(&entry,1));
        Check(CookedScene::OpenArchive(output/"legacy.toc",output/"legacy.eap",sceneID)->ValidateReferences(&error),error);
    }
    auto darkSun = sun; darkSun.intensity = 0.0f;
    auto darkLegacy = original;
    darkLegacy.environmentReference = SceneDocument::NoneEnvironmentReference;
    MigrateLegacyLighting(darkLegacy,darkSun);
    Check(Lighting(darkLegacy).sun.intensity == 0 &&
        Lighting(darkLegacy).sun.ambientStrength == sun.ambientStrength, "Zero-intensity sun lost independent ambient settings.");
    // Pole and cardinal directions exercise the Engine +Z Euler convention.
    for(const Vector3 direction : {Vector3{0,1,0},Vector3{0,-1,0},Vector3{1,0,0},Vector3{0,0,-1},Vector3{0,0,1}})
    {
        auto cardinal = original; auto light = sun; light.direction = direction;
        MigrateLegacyLighting(cardinal,light);
        Check(Near(Lighting(cardinal).sun.direction,direction),"Cardinal legacy direction changed during migration.");
    }
    auto collision = original;
    SceneObjectDocument occupied; occupied.id = "legacy-directional-light"; collision.objects.push_back(occupied);
    MigrateLegacyLighting(collision,sun);
    Check(collision.objects.size() == 2 && collision.objects[1].id == "legacy-directional-light-1",
        "Migration reused an existing object ID.");
    for(int cycle = 0; cycle < 3; ++cycle)
    {
        auto before = migrated;
        Check(SaveSceneDocument(migrated,output/"migrated.scene",&error),error);
        Json saved;
        { std::ifstream file(output/"migrated.scene"); file >> saved; }
        Check(!saved.contains("directionalLight") && saved.contains("lightingSettings"), "Saved a second authoritative sun.");
        Check(LoadSceneDocument(output/"migrated.scene",migrated,&error),error);
        Check(SceneDocumentsEqual(before,migrated), "Repeated migration changed IDs, settings or objects.");
        Check(SceneDocumentsEqual(before,CookedScene::Decode(CookedScene::Cook(migrated,{},registry)).document),
            "Current cooked radius/settings roundtrip failed.");
    }
    // Existing directional IDs, parent relationships and pose win over the old scene sun.
    Json component;
    { std::ifstream file(output/"migrated.scene"); file >> component; }
    auto existing = component["objects"][0];
    existing["id"] = "existing-directional";
    existing["light"]["intensity"] = 7.0;
    existing["light"]["color"] = {0.2,0.4,0.6};
    existing["light"]["enabled"] = false;
    existing["light"].erase("angularRadius");
    existing["parentID"] = "parent";
    auto parent = existing;
    parent["id"] = "parent"; parent["parentID"] = ""; parent.erase("light");
    historical["schemaVersion"] = 12;
    historical["objects"] = Json::array({parent,existing});
    { std::ofstream file(output/"existing.scene"); file << historical; }
    SceneDocument existingScene;
    Check(LoadSceneDocument(output/"existing.scene",existingScene,&error),error);
    Check(existingScene.objects.size() == 2 && existingScene.objects[1].id == "existing-directional" &&
        existingScene.objects[1].parentID == "parent" && existingScene.objects[1].light->intensity == 7 &&
        existingScene.objects[1].light->angularRadius == sun.angularRadius && !existingScene.objects[1].light->enabled,
        "Existing directional objects lost precedence, identity or inherited radius.");
    Check(Lighting(existingScene).sun.intensity == 0, "Disabled existing directional reactivated the legacy sun.");

    // Historical component record (36 bytes) still inherits the scene sun radius on decode.
    SceneDocument oneComponent = original;
    oneComponent.objects = {migrated.objects[0]};
    oneComponent.objects[0].light->color = {0.2f,0.4f,0.6f};
    oneComponent.objects[0].light->intensity = 7;
    for(unsigned version : {11u,12u})
    {
        auto legacyBytes = CookedScene::Cook(oneComponent,{},registry);
        const size_t ambientOffset = 32 + 4 + oneComponent.id.size() + 4 + oneComponent.name.size() + 40;
        const auto oldHeader = HistoricalCooked(original,version,sun);
        legacyBytes.erase(legacyBytes.begin()+ambientOffset,legacyBytes.begin()+ambientOffset+4);
        legacyBytes.insert(legacyBytes.begin()+ambientOffset,oldHeader.end()-36,oldHeader.end());
        legacyBytes.resize(legacyBytes.size()-4); // V11/12 components did not store angular radius.
        legacyBytes[legacyBytes.size()-40] = std::byte{36};
        legacyBytes[6] = std::byte{'1'}; legacyBytes[7] = std::byte('0'+version%10); legacyBytes[8] = std::byte(version);
        for(unsigned i = 0; i < 8; ++i) legacyBytes[16+i] = std::byte((legacyBytes.size() >> (i*8)) & 255);
        const auto oldComponent = CookedScene::Decode(legacyBytes).document;
        Check(oldComponent.objects.size() == 1 && oldComponent.objects[0].id == oneComponent.objects[0].id &&
            oldComponent.objects[0].light->intensity == 7 && oldComponent.objects[0].light->angularRadius == sun.angularRadius,
            "Historical cooked component lost identity, precedence or angular radius.");
    }

    // Current data takes deterministic precedence over a stale legacy root.
    component["directionalLight"] = historical["directionalLight"];
    component["objects"] = Json::array();
    component["lightingSettings"]["ambientStrength"] = 0.28f;
    { std::ofstream file(output/"mixed.scene"); file << component; }
    Check(LoadSceneDocument(output/"mixed.scene",existingScene,&error) && existingScene.objects.empty() &&
        existingScene.lightingSettings.ambientStrength == 0.28f, "Stale legacy root overrode current object ownership.");
    historical.erase("directionalLight"); historical["objects"] = Json::array();
    { std::ofstream file(output/"absent.scene"); file << historical; }
    Check(LoadSceneDocument(output/"absent.scene",existingScene,&error) && existingScene.objects.empty(),
        "Missing legacy sun fabricated a default directional.");

    // Exercise actual committed runtime capture and component edit/undo/redo value transactions.
    MeshlessLightingResolver resolver;
    FileSceneRuntime runtime;
    auto runtimeDocument = migrated;
    runtimeDocument.environmentReference = SceneDocument::NoneEnvironmentReference;
    SceneObjectDocument runtimePoint;
    runtimePoint.id = "runtime-point"; runtimePoint.light = LightComponent{};
    runtimeDocument.objects.push_back(runtimePoint);
    Check(runtime.LoadUnsavedDocument(runtimeDocument,&error) && runtime.Prepare(resolver,&error) && runtime.Commit(&error),error);
    Check(runtime.GetScene().UsesObjectLighting(), "File-scene commit retained demo sun storage.");
    auto selection = runtime.GetObjectMetadata(migrated.objects[0].id);
    Check(selection.has_value(), "Migrated object is missing from runtime hierarchy.");
    const auto initial = migrated.objects[0].light;
    auto edited = initial; edited->angularRadius = 0.13f; edited->intensity = 3.0f;
    Check(runtime.SetObjectLight(*selection,initial,edited,&error),error);
    SceneLightingSnapshot snapshot;
    Check(runtime.CaptureSceneLighting(snapshot,error),error);
    FrameLighting frame;
    Check(BuildFrameLighting(DirectionalLight{},&snapshot,frame,&error) && frame.sun.angularRadius == edited->angularRadius,error);
    RtxdiLightSet setBefore;
    Check(BuildRtxdiLightSet(frame,{},setBefore) && setBefore.records.size() == 2 && setBefore.identities[1].kind == RtxdiLightKind::Sun,"Runtime sun missing from RTXDI.");
    auto changedRadius = frame;
    changedRadius.sun.angularRadius += 0.1f;
    RtxdiLightSet radiusSet;
    Check(BuildRtxdiLightSet(changedRadius,{},radiusSet), "Cannot build edited-radius RTXDI set.");
    std::vector<std::int32_t> radiusForward,radiusBackward;
    BuildRtxdiLightTranslation(setBefore,radiusSet,radiusForward,radiusBackward);
    Check(radiusBackward.size() == 2 && radiusBackward[0] == 0 && radiusBackward[1] == -1,
        "Changing angular radius retained an incompatible RTXDI sun history mapping.");
    Check(runtime.SetObjectLight(*selection,edited,initial,&error) && runtime.SetObjectLight(*selection,initial,edited,&error),
        "Component undo/redo values failed.");
    auto disabled = edited; disabled->enabled = false;
    Check(runtime.SetObjectLight(*selection,edited,disabled,&error) && runtime.CaptureSceneLighting(snapshot,error),error);
    Check(BuildFrameLighting(DirectionalLight{},&snapshot,frame,&error) && frame.sun.intensity == 0,
        "Disabling final directional left sun energy.");
    Check(runtime.SetObjectLight(*selection,disabled,edited,&error),error);
    auto transform = migrated.objects[0].transform;
    transform.rotation = {0.2f,1.1f,0.0f};
    runtime.GetScene().SetRenderObjectTransform(selection->runtimeIndex,transform);
    Check(runtime.CaptureSceneLighting(snapshot,error) && BuildFrameLighting(DirectionalLight{},&snapshot,frame,&error),error);
    const auto rotatedDirection = frame.sun.direction;
    Check(!Near(rotatedDirection,sun.direction.Normalized()), "Runtime rotation did not update light direction.");
    transform.position = {50,-20,100};
    runtime.GetScene().SetRenderObjectTransform(selection->runtimeIndex,transform);
    Check(runtime.CaptureSceneLighting(snapshot,error) && BuildFrameLighting(DirectionalLight{},&snapshot,frame,&error) &&
        Near(frame.sun.direction,rotatedDirection), "Moving directional light changed its illumination direction.");
    auto settings = runtime.GetScene().GetLightingSettings(); settings.ambientStrength = 0.91f;
    runtime.GetScene().SetLightingSettings(settings);
    Check(runtime.SetObjectLight(*selection,edited,std::nullopt,&error) && runtime.CaptureSceneLighting(snapshot,error),error);
    Check(BuildFrameLighting(DirectionalLight{},&snapshot,frame,&error) && frame.sun.intensity == 0 &&
        frame.sun.ambientStrength == settings.ambientStrength, "Removing final light lost independent ambient or resurrected sun.");
    RtxdiLightSet removed;
    Check(BuildRtxdiLightSet(frame,{},removed) && removed.records.size() == 1 &&
        removed.identities[0].kind == RtxdiLightKind::Point && removed.identities[0].objectID == runtimePoint.id, "Deleted directional survived RTXDI light construction.");
    std::vector<std::int32_t> toPrevious,toCurrent;
    BuildRtxdiLightTranslation(setBefore,removed,toPrevious,toCurrent);
    Check(toCurrent.size() == 2 && toCurrent[0] == 0 && toCurrent[1] == -1,"Deleted sun retained an RTXDI history mapping.");
    Check(runtime.SaveAs(output/"runtime-deleted.scene",&error),error);
    SceneDocument afterDelete;
    Check(LoadSceneDocument(output/"runtime-deleted.scene",afterDelete,&error) && Lighting(afterDelete).sun.intensity == 0 &&
        afterDelete.lightingSettings == settings,"Save/reload resurrected removed light or lost scene ambient.");

    // Ordinary hierarchy creation, duplication and whole-object deletion retain existing workflows.
    SceneDocument lifecycle = original;
    lifecycle.objects.push_back(PrepareLightObject(lifecycle,LightType::Directional,{0,0,0},{0,-1,1}));
    lifecycle.objects[0].light->angularRadius = 0.08f;
    SceneDocument duplicated;
    std::string duplicateID;
    Check(SceneHierarchy::DuplicateSubtree(lifecycle,lifecycle.objects[0].id,duplicated,duplicateID,error),error);
    Check(duplicated.objects.size() == 2 && duplicated.objects[0].id != duplicateID &&
        duplicated.objects[0].light == duplicated.objects[1].light, "Duplicating light lost component values or identity.");
    auto multiple = Lighting(duplicated);
    Check(multiple.table.lightCount == 1 && multiple.sunObjectID == lifecycle.objects[0].id,
        "Multiple-directional owner/table semantics changed.");
    Check(SceneHierarchy::DeleteSubtree(duplicated,lifecycle.objects[0].id,lifecycle,error),error);
    Check(Lighting(lifecycle).sunObjectID == duplicateID,"Deleting owner did not promote the remaining directional.");
    Check(SceneHierarchy::DeleteSubtree(lifecycle,duplicateID,duplicated,error),error);
    Check(Lighting(duplicated).sun.intensity == 0,"Deleting final object resurrected legacy illumination.");

    // Preserve the former 64 table lights plus legacy sun at maximum scene capacity.
    SceneDocument crowded = original;
    for(unsigned i = 0; i < MaximumSceneLights; ++i)
    {
        SceneObjectDocument point; point.id = "point-" + std::to_string(i); point.light = LightComponent{};
        crowded.objects.push_back(point);
    }
    MigrateLegacyLighting(crowded,sun);
    Check(ValidateSceneDocument(crowded,&error) && Lighting(crowded).table.lightCount == MaximumSceneLights,
        "Migrating a full legacy light table exceeded capacity.");
    auto radiometry = DefaultLightComponent(LightType::Directional);
    radiometry.color = {-3,2,4}; radiometry.intensity = (std::numeric_limits<float>::max)();
    Check(ValidateLightComponent(radiometry), "Finite legacy directional radiometry was narrowed.");
    radiometry.type = LightType::Point;
    Check(!ValidateLightComponent(radiometry), "Point/Spot radiometry bounds were broadened.");
}

void CreateControlledFixture(const std::filesystem::path& root)
{
    using Json = nlohmann::json;
    std::string error;
    ProjectPaths paths(root);
    Check(paths.EnsureDirectories(&error),error);
    ProjectAssetRegistry registry;
    if(std::filesystem::exists(paths.GetAssetRegistryPath()))
        Check(AssetRegistrySerializer::LoadFromFile(paths.GetAssetRegistryPath(),registry,&error),error);
    std::string cube;
    Check(PrimitiveMeshAssets::Ensure(paths,registry,ScenePrimitive::Cube,cube,error),error);
    const auto materialID = *AssetID::Parse("00000000-0000-4000-8000-000000000003");
    if(!registry.Find(materialID))
        Check(registry.Register({materialID,AssetType::Material,"Materials/RoughFloor.material"}), "Cannot register controlled material.");
    MaterialAssetData material;
    material.baseColor = {0.6f,0.6f,0.6f}; material.roughness = 0.8f; material.metallic = 0;
    Check(MaterialAssetSerializer::SaveToFile(material,root/"Materials/RoughFloor.material",&error),error);
    Check(AssetRegistrySerializer::SaveToFile(registry,paths.GetAssetRegistryPath(),&error),error);
    SceneDocument scene;
    scene.id = "00000000-0000-4000-8000-000000000004";
    scene.name = "Controlled Point Light Only";
    scene.environmentReference = SceneDocument::NoneEnvironmentReference;
    scene.lightingSettings.ambientStrength = 0;
    scene.camera.position = {5,3,-6}; scene.camera.target = {0,1,0};
    SceneObjectDocument floor;
    floor.id = "floor"; floor.name = "Rough Nonmetallic Floor"; floor.meshReference = cube;
    floor.materialReference = materialID.ToReference();
    floor.transform.position = {0,-0.1f,0}; floor.transform.scale = {10,0.2f,10};
    scene.objects.push_back(floor);
    auto occluder = floor;
    occluder.id = "movable-occluder"; occluder.name = "Movable Occluder";
    occluder.transform.position = {0,1,0}; occluder.transform.scale = {1,2,1};
    scene.objects.push_back(occluder);
    SceneObjectDocument point;
    point.id = "controlled-point-light"; point.name = "Only Point Light";
    point.transform.position = {-2,3,-1}; point.light = DefaultLightComponent(LightType::Point);
    point.light->intensity = 60; point.light->range = 12;
    scene.objects.push_back(point);
    scene.objects.push_back(PrepareCameraObject(scene,scene.camera.position,scene.camera.target));
    Check(SaveSceneDocument(scene,root/"Scenes/ControlledPointOnly.scene",&error),error);
    const Json project = {{"format","EngineProject"},{"formatVersion",1},{"name","Controlled Lighting"},
        {"projectId","00000000-0000-4000-8000-000000000005"},
        {"contentPaths",{{"scenes","Scenes"},{"materials","Materials"},{"meshes","Meshes"},{"textures","Textures"}}},
        {"settings",{{"startupScene","Scenes/ControlledPointOnly.scene"}}}};
    { std::ofstream file(root/"ControlledLighting.engineproj"); file << project.dump(4); Check(bool(file),"Cannot save controlled project."); }
    SceneDocument reloaded;
    Check(LoadSceneDocument(root/"Scenes/ControlledPointOnly.scene",reloaded,&error),error);
    RtxdiLightSet actual;
    Check(BuildRtxdiLightSet(Lighting(reloaded),{},actual) && actual.records.size() == 1 &&
        actual.identities[0].objectID == point.id && actual.identities[0].kind == RtxdiLightKind::Point,
        "Controlled floor/occluder/None scene has an unintended RTXDI light.");
}

}
int main(int argc, char** argv)
{
    try
    {
        Check(argc == 2, "Pass a scratch output directory.");
        const auto output = std::filesystem::absolute(argv[1]);
        std::filesystem::create_directories(output);
        CheckLightingMigration(output);
        CreateControlledFixture(output/"ControlledProject");
        SceneDocument scene;
        scene.id = "lighting-environment-test";
        scene.environmentReference = SceneDocument::NoneEnvironmentReference;
        SceneObjectDocument point;
        point.id = "point";
        point.light = DefaultLightComponent(LightType::Point);
        scene.objects.push_back(point);
        std::string error;
        Check(SaveSceneDocument(scene, output / "none.scene", &error), error);
        SceneDocument loaded;
        Check(LoadSceneDocument(output / "none.scene", loaded, &error), error);
        Check(loaded.environmentReference == scene.environmentReference, "Loose None roundtrip failed.");
        ProjectAssetRegistry registry;
        auto bytes = CookedScene::Cook(loaded, {}, registry);
        const auto cooked = CookedScene::Decode(bytes);
        Check(cooked.document.environmentReference == scene.environmentReference && !cooked.environment,
            "Cooked None must roundtrip without an asset binding.");
        auto corrupt = bytes;
        corrupt[28] = std::byte{3};
        bool rejected = false;
        try { CookedScene::Decode(corrupt); } catch(const std::exception&) { rejected = true; }
        Check(rejected, "Mutually exclusive environment flags accepted.");
        const auto id = *AssetID::ParseReference("asset:00000000-0000-4000-8000-000000000001");
        const EngineAssetPak::PreparedAsset entry{id, EngineAssetPak::Type::Scene, CookedScene::Version, 1, bytes, 16};
        EngineAssetPak::Write(output / "Game.toc", output / "Game.eap", std::span(&entry, 1));
        auto archive = CookedScene::OpenArchive(output / "Game.toc", output / "Game.eap", id);
        Check(archive->ValidateReferences(&error), error);
        Check(CookedEnvironment::ArchiveIdentity(*archive) == SceneDocument::NoneEnvironmentReference,
            "Packaged None identity failed.");
        const auto black = CookedEnvironment::ReadArchive(*archive);
        Check(black && black->pixels == MakeBlackEnvironmentImage().pixels, "Packaged None is not built-in black.");
        for(size_t i = 0; i < black->pixels.size(); ++i)
            Check(black->pixels[i] == (i % 4 == 3 ? 1.0f : 0.0f), "Nonzero black source radiance.");

        // Historical schema/reference semantics survive the transitional reader unchanged.
        nlohmann::json old;
        { std::ifstream file(output / "none.scene"); file >> old; }
        old["schemaVersion"] = 11;
        for(const std::string reference : {std::string{}, id.ToReference()})
        {
            old["environmentReference"] = reference;
            { std::ofstream file(output / "legacy.scene"); file << old; }
            Check(LoadSceneDocument(output / "legacy.scene", loaded, &error), error);
            Check(loaded.environmentReference == reference, "Historical environment reference changed.");
        }
        loaded.environmentReference.clear();
        bytes = CookedScene::Cook(loaded, {}, registry);
        const auto hdrPath = output / "assigned.hdr";
        {
            std::ofstream file(hdrPath, std::ios::binary);
            file << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 8 +X 16\n";
            const unsigned char pixel[]{128, 64, 32, 130};
            for(int i = 0; i < 128; ++i) file.write(reinterpret_cast<const char*>(pixel), 4);
        }
        Check(registry.Register({id, AssetType::Texture, "assigned.hdr"}), "HDR registry fixture failed.");
        const auto hdrPayload = CookedEnvironment::Cook(hdrPath, id, 1);
        const CookedScene::EnvironmentInput hdrInput{id, 1, hdrPayload};
        loaded.environmentReference = id.ToReference();
        const auto assignedBytes = CookedScene::Cook(loaded, {}, registry, {}, &hdrInput);
        const auto assigned = CookedScene::Decode(assignedBytes);
        Check(assigned.document.environmentReference == id.ToReference() && assigned.environment.has_value(),
            "Assigned HDRI cooked roundtrip changed.");
        const auto hdrImage = CookedEnvironment::ReadLoose(hdrPath);

        ComPtr<ID3D12Debug> debug;
        if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        ComPtr<ID3D12Device> device;
        Check(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))), "D3D12 device failed.");
        ComPtr<ID3D12InfoQueue> info;
        device.As(&info);
        ResourceUploader uploader;
        DescriptorHeap heap;
        Check(uploader.Initialize(device.Get()), "Uploader initialization failed.");
        Check(heap.Initialize(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 128, true), "Heap initialization failed.");
        const auto available = heap.GetAvailableCount();
        EnvironmentMap environment;
        for(int cycle = 0; cycle < 3; ++cycle)
        {
            Check(environment.Initialize(device.Get(), *black, uploader, heap), "Black environment GPU upload failed.");
            Check(environment.GetImportanceTotalWeight() == 0, "Black environment has sampling energy.");
            RtxdiEnvironmentSource source;
            source.textureIndex = environment.GetSourceTexture().GetDescriptorIndex();
            source.conditionalCDFIndex = environment.GetImportanceConditionalCDF().GetDescriptorIndex();
            source.marginalCDFIndex = environment.GetImportanceMarginalCDF().GetDescriptorIndex();
            source.width = environment.GetImportanceWidth(); source.height = environment.GetImportanceHeight();
            source.totalWeight = environment.GetImportanceTotalWeight();
            Check(source.textureIndex != UINT_MAX && source.conditionalCDFIndex != UINT_MAX && source.marginalCDFIndex != UINT_MAX,
                "Invalid black environment descriptors.");
            Check(environment.GetCubemap().GetDescriptorIndex() != UINT_MAX &&
                environment.GetDiffuseIrradianceArrayIndex() != UINT_MAX &&
                environment.GetGGXMultiscatterIrradiance().GetDescriptorIndex() != UINT_MAX, "Missing derived resource bindings.");
            SceneLightingSnapshot snapshot;
            Check(BuildSceneLightingSnapshot(scene, [](const SceneObjectDocument& object) -> std::optional<Matrix4>
                { return object.transform.GetModelMatrix(); }, snapshot, &error), error);
            FrameLighting frame;
            Check(BuildFrameLighting(DirectionalLight{}, &snapshot, frame, &error), error);
            RtxdiLightSet set;
            Check(BuildRtxdiLightSet(frame, source, set), "RTXDI set construction failed.");
            Check(set.records.size() == 1 && set.identities[0].kind == RtxdiLightKind::Point &&
                set.identities[0].objectID == point.id, "Actual RTXDI set contains an unintended light.");
            environment.Shutdown();
            Check(heap.GetAvailableCount() == available, "Black descriptors leaked.");
            Check(environment.Initialize(device.Get(), std::filesystem::path{}, uploader, heap), "Neutral switch failed.");
            Check(environment.GetImportanceTotalWeight() > 0, "Neutral environment lost its energy.");
            source.totalWeight = environment.GetImportanceTotalWeight();
            environment.Shutdown();
            Check(environment.Initialize(device.Get(), hdrPath, uploader, heap), "HDRI switch failed.");
            const auto* sourcePixels = static_cast<const float*>(environment.GetSourceTexture().GetInitialData());
            Check(std::equal(hdrImage.pixels.begin(), hdrImage.pixels.end(), sourcePixels), "Assigned HDRI pixels changed.");
            source.textureIndex = environment.GetSourceTexture().GetDescriptorIndex();
            source.conditionalCDFIndex = environment.GetImportanceConditionalCDF().GetDescriptorIndex();
            source.marginalCDFIndex = environment.GetImportanceMarginalCDF().GetDescriptorIndex();
            source.totalWeight = environment.GetImportanceTotalWeight();
            RtxdiLightSet withEnvironment;
            Check(BuildRtxdiLightSet(frame, source, withEnvironment) && withEnvironment.records.size() == 2,
                "RTXDI environment restoration failed.");
            std::vector<std::int32_t> forward, backward;
            BuildRtxdiLightTranslation(withEnvironment, set, forward, backward);
            Check(backward.size() == 2 && backward[1] == -1, "Removed RTXDI environment retained a history mapping.");
            environment.Shutdown();
            Check(heap.GetAvailableCount() == available, "Switching environments leaked descriptors.");
        }
        heap.Shutdown(); uploader.Shutdown();
        if(info)
        {
            for(UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
            {
                SIZE_T size = 0; info->GetMessage(i, nullptr, &size);
                std::vector<std::byte> storage(size);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                info->GetMessage(i, message, &size);
                Check(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR && message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION,
                    message->pDescription);
            }
        }
        std::cout << checks << " checks passed (migration, object ownership, runtime edits/deletion, loose/cooked/archive persistence, GPU resources, RTXDI set/history).\n";
        return 0;
    }
    catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
