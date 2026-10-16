#include "DemoContent/FileBackedDemoScene.h"
#include "Editor/Editor.h"
#include "Editor/SceneObjectDuplication.h"
#include "Scene/SceneDocumentAdapter.h"
#include "Scene/SceneSerializer.h"
#include "Scene/SceneAssetResolver.h"
#include "Renderer/Material.h"
#include "Project/ProjectPaths.h"
#include "assets/CookedScene.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
using Physics::BodyType;
using Physics::ColliderType;
using Physics::PhysicsComponent;

void Check(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// CPU-only material storage for empty-mesh scene objects; no asset catalog is initialized.
class DiagnosticResolver final : public SceneAssetResolver
{
public:
    Mesh* ResolveMesh(std::string_view) override
    {
        return nullptr;
    }

    Material* ResolveMaterial(std::string_view) override
    {
        return &m_material;
    }

private:
    Material m_material;
};

SceneDocument MakeDocument()
{
    SceneDocument document;
    document.id = "scene-physics-data-verification";

    SceneObjectDocument plainObject;
    plainObject.id = "plain-object";
    document.objects.push_back(plainObject);

    SceneObjectDocument physicsObject;
    physicsObject.id = "physics-object";
    physicsObject.name = "Physics Box";
    physicsObject.transform.position = {1.0f, 2.0f, 3.0f};
    physicsObject.transform.rotation = {0.2f, 0.3f, 0.4f};
    physicsObject.transform.scale = {2.0f, 3.0f, 4.0f};
    physicsObject.physics.emplace();
    physicsObject.physics->bodyType = BodyType::Kinematic;
    physicsObject.physics->boxDimensions = {2.5f, 3.5f, 4.5f};
    physicsObject.physics->colliderLocalPositionOffset = {0.1f, -0.2f, 0.3f};
    physicsObject.physics->density = 750.0f;
    physicsObject.physics->friction = 0.7f;
    physicsObject.physics->restitution = 0.25f;
    document.objects.push_back(physicsObject);
    return document;
}

void VerifyDefaultsAndValidation()
{
    const SceneObjectDocument plainObject;
    Check(!plainObject.physics, "Physics must be absent by default");

    const PhysicsComponent defaults;
    Check(defaults.bodyType == BodyType::Static && defaults.colliderType == ColliderType::Box,
        "Unexpected default body/collider type");
    Check(defaults.boxDimensions == Physics::BoxDimensions{1.0f, 1.0f, 1.0f}, "Default box dimensions");
    Check(defaults.colliderLocalPositionOffset.Length() == 0.0f, "Default collider offset");
    Check(defaults.density == 1000.0f && defaults.friction == 0.5f && defaults.restitution == 0.0f,
        "Default material values");

    std::string error;
    for (const BodyType bodyType : {BodyType::Static, BodyType::Dynamic, BodyType::Kinematic})
    {
        auto component = defaults;
        component.bodyType = bodyType;
        Check(Physics::ValidatePhysicsComponent(component, &error), error);
    }

    // Exercise every numeric field with both nonfinite values.
    for (const float invalidValue : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
    {
        for (int field = 0; field < 9; ++field)
        {
            auto document = MakeDocument();
            auto& component = *document.objects.back().physics;
            float* numericFields[] = {
                &component.boxDimensions.width, &component.boxDimensions.height, &component.boxDimensions.depth,
                &component.colliderLocalPositionOffset.x, &component.colliderLocalPositionOffset.y,
                &component.colliderLocalPositionOffset.z, &component.density, &component.friction, &component.restitution
            };
            *numericFields[field] = invalidValue;
            Check(!ValidateSceneDocument(document, &error) && error.find("physics-object") != std::string::npos,
                "Nonfinite authored physics was accepted or lacked object context");
        }
    }

    for (const float invalidValue : {0.0f, -1.0f})
    {
        for (int field = 0; field < 4; ++field)
        {
            auto component = defaults;
            float* positiveFields[] = {
                &component.boxDimensions.width, &component.boxDimensions.height, &component.boxDimensions.depth,
                &component.density
            };
            *positiveFields[field] = invalidValue;
            Check(!Physics::ValidatePhysicsComponent(component, &error) && !error.empty(),
                "Nonpositive dimension/density accepted");
            Check(*positiveFields[field] == invalidValue, "Validation clamped authored values");
        }
    }

    auto component = defaults;
    component.friction = -0.1f;
    Check(!Physics::ValidatePhysicsComponent(component, &error), "Negative friction accepted");
    component = defaults;
    for (const float restitution : {-0.1f, 1.1f})
    {
        component.restitution = restitution;
        Check(!Physics::ValidatePhysicsComponent(component, &error), "Invalid restitution accepted");
    }
    component.restitution = 1.0f;
    component.friction = 0.0f;
    Check(Physics::ValidatePhysicsComponent(component, &error), "Valid material boundary rejected");
    component.bodyType = static_cast<BodyType>(99);
    Check(!Physics::ValidatePhysicsComponent(component, &error), "Unknown body type accepted");
    component = defaults;
    component.colliderType = static_cast<ColliderType>(99);
    Check(!Physics::ValidatePhysicsComponent(component, &error), "Unknown collider type accepted");

    const auto original = MakeDocument();
    auto copy = original;
    Check(SceneDocumentsEqual(original, copy), "Document copy lost physics");
    copy.objects.back().physics->density += 1.0f;
    Check(!SceneDocumentsEqual(original, copy), "Document equality ignored physics changes");
    copy = original;
    copy.objects.back().physics.reset();
    Check(!SceneDocumentsEqual(original, copy), "Document equality ignored component removal");
}

void VerifyPersistenceBoundary()
{
    auto document = MakeDocument();
    document.objects.back().physics.reset();
    const auto uniqueID = AssetID::Generate();
    Check(uniqueID.has_value(), "Cannot generate diagnostic filename");
    const auto path = std::filesystem::temp_directory_path() / ("Engine-physics-" + uniqueID->ToString() + ".scene");
    struct TemporarySceneFile
    {
        std::filesystem::path path;
        ~TemporarySceneFile()
        {
            std::error_code cleanupError;
            std::filesystem::remove(path, cleanupError);
        }
    } cleanup{path};

    std::string error;
    Check(SaveSceneDocument(document, path, &error), error);
    SceneDocument loaded;
    Check(LoadSceneDocument(path, loaded, &error), error);
    Check(SceneDocumentsEqual(document, loaded), "Nonphysics save/reload changed existing behavior");

    const auto physicsDocument = MakeDocument();
    Check(SaveSceneDocument(physicsDocument, path, &error), error);
    Check(LoadSceneDocument(path, loaded, &error) && SceneDocumentsEqual(physicsDocument, loaded),
        "Physics save/reload changed authored data");

    const ProjectAssetRegistry registry;
    const auto cooked = CookedScene::Cook(physicsDocument, {}, registry);
    Check(SceneDocumentsEqual(physicsDocument, CookedScene::Decode(cooked).document),
        "Cooked scene lost physics");

    nlohmann::json unsupportedScene;
    {
        std::ifstream input(path);
        input >> unsupportedScene;
    }
    unsupportedScene["objects"][0]["physics"] = nlohmann::json::object();
    {
        std::ofstream output(path, std::ios::trunc);
        output << unsupportedScene.dump();
    }
    Check(!LoadSceneDocument(path, loaded, &error) && error.find("physics") != std::string::npos,
        "Loose loading silently ignored unsupported physics fields");
    Check(SceneDocumentsEqual(physicsDocument, loaded), "Rejected load replaced the destination document");
}
}

// Matches existing diagnostic friends: access is limited to constructing CPU fixtures
// and driving the real editor snapshot/history code without window or GPU initialization.
class ScenePhysicsDataVerification
{
public:
    static void Run()
    {
        DiagnosticResolver resolver;
        FileBackedDemoScene owner(std::filesystem::path{});
        owner.m_projectRoot = std::filesystem::current_path();
        auto& runtime = *owner.m_runtime;
        const auto original = MakeDocument();
        std::string error;
        Check(runtime.m_fileScene.LoadDocument(original, &error), error);
        PreparedScene prepared;
        Check(PrepareScene(original, resolver, prepared, &error), error);
        Check(CommitPreparedEditorScene(prepared, original, runtime.m_scene, runtime.m_objectMapping, &error), error);

        SceneObjectDocument duplicate;
        std::size_t insertionIndex = 0;
        Check(PrepareSceneObjectDuplicate(runtime, "physics-object", duplicate, insertionIndex, error), error);
        Check(duplicate.id != "physics-object" && duplicate.physics == original.objects.back().physics,
            "Duplication lost physics or reused stable ID");
        auto expectedDuplicate = original.objects.back();
        expectedDuplicate.id = duplicate.id;
        expectedDuplicate.name = duplicate.name;
        SceneDocument expected, actual;
        expected.objects = {expectedDuplicate};
        actual.objects = {duplicate};
        Check(SceneDocumentsEqual(expected, actual), "Duplication changed authored data/transform");

        Editor editor;
        Check(editor.UpdateSceneHistorySession(owner, ProjectPaths(owner.m_projectRoot), "Physics data"),
            "Cannot establish diagnostic history session");
        // P.2F: drive the exact Inspector commit/history path without an ImGui or GPU context.
        float parsed = 42.0f;
        for(const char* text : {"", " ", "-", ".", "1e", "1oops", "nan", "inf", "1e999", "1 2"})
        {
            Check(!Editor::ParsePhysicsNumber(text, parsed) && parsed == 42.0f, "Malformed numeric text accepted");
        }
        Check(Editor::ParsePhysicsNumber(" -0.125 ", parsed) && parsed == -0.125f, "Valid numeric text rejected");
        owner.m_initialized = true;
        const auto selection = owner.GetObjectMetadata(std::string_view{"plain-object"});
        Check(selection.has_value(), "Cannot inspect plain object");
        const auto baseline = runtime.m_fileScene.GetDocument();
        Check(editor.CommitObjectPhysics(owner, *selection, {}, PhysicsComponent{}), "Checkbox enable failed");
        Check(runtime.GetFileScene().IsDirty(), "Physics enable did not dirty scene");
        Check(runtime.GetFileScene().GetDocument().objects.front().physics == PhysicsComponent{}, "Incorrect checkbox defaults");
        PhysicsComponent previous;
        for(auto type : {BodyType::Static, BodyType::Dynamic, BodyType::Kinematic})
        {
            auto edited = previous;
            edited.bodyType = type;
            edited.boxDimensions = {2.5f, 3.25f, 4.75f};
            edited.colliderLocalPositionOffset = {-0.125f, 0.25f, 0.5f};
            edited.density = 997.25f;
            edited.friction = 0.625f;
            edited.restitution = 0.375f;
            Check(editor.CommitObjectPhysics(owner, *selection, previous, edited), "Property commit failed");
            previous = edited;
        }
        const auto cursor = editor.m_historyCursor;
        auto invalid = previous;
        invalid.boxDimensions.width = 0;
        Check(!editor.CommitObjectPhysics(owner, *selection, previous, invalid), "Invalid dimensions accepted");
        invalid = previous;
        invalid.density = -1;
        Check(!editor.CommitObjectPhysics(owner, *selection, previous, invalid), "Invalid density accepted");
        invalid = previous;
        invalid.friction = -1;
        Check(!editor.CommitObjectPhysics(owner, *selection, previous, invalid), "Invalid friction accepted");
        invalid = previous;
        invalid.restitution = 1.5f;
        Check(!editor.CommitObjectPhysics(owner, *selection, previous, invalid), "Invalid restitution accepted");
        invalid = previous;
        invalid.colliderLocalPositionOffset.x = std::numeric_limits<float>::infinity();
        Check(!editor.CommitObjectPhysics(owner, *selection, previous, invalid), "Invalid offset accepted");
        Check(editor.m_historyCursor == cursor, "Invalid edits entered history");
        for(auto state : {Editor::PhysicsState::Playing, Editor::PhysicsState::Stopping})
        {
            editor.SetPhysicsStatus(state, false, {});
            Check(!editor.CommitObjectPhysics(owner, *selection, previous, {}), "Simulation accepted physics mutation");
            editor.ReplaySceneHistory(owner, false);
            Check(editor.m_historyCursor == cursor, "Simulation replay changed history");
        }
        editor.SetPhysicsStatus(Editor::PhysicsState::Stopped, true, {});
        Check(editor.CommitObjectPhysics(owner, *selection, previous, {}), "Checkbox removal failed");
        editor.ReplaySceneHistory(owner, false);
        Check(runtime.GetFileScene().GetDocument().objects.front().physics == previous, "Removal undo lost component");
        editor.ReplaySceneHistory(owner, true);
        Check(!runtime.GetFileScene().GetDocument().objects.front().physics, "Removal redo failed");
        editor.ReplaySceneHistory(owner, false);
        const auto editedDocument = runtime.GetFileScene().GetDocument();
        auto unchanged = editedDocument;
        unchanged.objects.front().physics.reset();
        Check(SceneDocumentsEqual(unchanged, baseline), "Physics edit changed unrelated authored fields");
        Check(!owner.GetScene().HasPhysicsPresentation(), "Physics edit changed presentation");
        SceneDocument presentedCapture;
        Check(CaptureSceneDocument(editedDocument, owner.GetScene(), runtime.GetObjectMapping(), presentedCapture, &error), error);
        Check(SceneDocumentsEqual(presentedCapture, editedDocument), "Physics edit changed renderer authored transforms");
        SceneObjectDocument inspected;
        Check(owner.CapturePhysicsInspection("physics-object", inspected) && inspected.physics == baseline.objects.back().physics,
            "Physics selection inspection failed");
        Check(owner.CapturePhysicsInspection("plain-object", inspected) && inspected.physics == previous,
            "Selection change retained previous physics state");
        auto stale = *selection;
        ++stale.runtimeIdentity;
        Check(!editor.CommitObjectPhysics(owner, stale, previous, {}), "Stale Inspector selection accepted");

        const auto roundTripPath = std::filesystem::current_path() / "build" / "physics-inspector-roundtrip.scene";
        Check(SaveSceneDocument(editedDocument, roundTripPath, &error), error);
        SceneDocument loaded;
        Check(LoadSceneDocument(roundTripPath, loaded, &error), error);
        Check(SceneDocumentsEqual(loaded, editedDocument), "Inspector physics round-trip failed");
        SceneObjectDocument editedDuplicate;
        size_t editedDuplicateIndex = 0;
        Check(PrepareSceneObjectDuplicate(runtime, "plain-object", editedDuplicate, editedDuplicateIndex, error), error);
        Check(editedDuplicate.id != "plain-object" && editedDuplicate.physics == previous, "Edited physics duplicate failed");
        while(editor.m_historyCursor) editor.ReplaySceneHistory(owner, false);
        Check(SceneDocumentsEqual(runtime.GetFileScene().GetDocument(), baseline), "Addition/edit undo failed");
        while(editor.m_historyCursor < editor.m_sceneHistory.size()) editor.ReplaySceneHistory(owner, true);
        Check(!runtime.GetFileScene().GetDocument().objects.front().physics, "Full redo removal failed");
        editor.ClearSceneHistory();
        Check(editor.UpdateSceneHistorySession(owner, ProjectPaths(owner.m_projectRoot), "Physics data"), "History reset failed");
        const auto insertionOperation = editor.AllocateSceneHistoryOperationID();
        Check(editor.StageObjectDeletionHistory(duplicate, insertionIndex, {}, {}, insertionOperation,
            editor.m_historyEpoch, false, error, true), error);
        owner.m_runtime = runtime.PrepareObjectRestoration(resolver, duplicate, insertionIndex, &error);
        Check(owner.m_runtime != nullptr, error);
        Check(editor.RebindSceneHistoryAfterStructureChange(owner), "Cannot rebind inserted object history");
        editor.CompleteObjectDeletionHistory(insertionOperation, editor.m_historyEpoch, true, {});

        Replay(editor, owner, resolver, false);
        Check(owner.GetScene().GetRenderObjects().size() == 2, "Duplication undo did not remove object");
        Replay(editor, owner, resolver, true);
        Check(owner.m_runtime->GetFileScene().GetDocument().objects.back().physics == duplicate.physics,
            "Duplication redo lost physics");

        SceneObjectDocument snapshot;
        std::size_t documentIndex = 0;
        Check(owner.m_runtime->CaptureObjectDeletionSnapshot(duplicate.id, snapshot, documentIndex, &error), error);
        const auto deletionOperation = editor.AllocateSceneHistoryOperationID();
        Check(editor.StageObjectDeletionHistory(snapshot, documentIndex, {}, {}, deletionOperation,
            editor.m_historyEpoch, false, error), error);
        owner.m_runtime = owner.m_runtime->PrepareObjectDeletion(duplicate.id, &error);
        Check(owner.m_runtime != nullptr, error);
        Check(editor.RebindSceneHistoryAfterStructureChange(owner), "Cannot rebind deleted object history");
        editor.CompleteObjectDeletionHistory(deletionOperation, editor.m_historyEpoch, true, {});
        Replay(editor, owner, resolver, false);
        Replay(editor, owner, resolver, true);
        Replay(editor, owner, resolver, false);

        SceneDocument captured;
        Check(CaptureSceneDocument(owner.m_runtime->GetFileScene().GetDocument(), owner.GetScene(),
            owner.m_runtime->GetObjectMapping(), captured, &error), error);
        expected = original;
        expected.objects.push_back(duplicate);
        Check(SceneDocumentsEqual(expected, captured), "History changed IDs, physics, transforms, or nonphysics object");
        Check(!captured.objects.front().physics, "History enabled physics on an unrelated object");
    }

private:
    static void Replay(Editor& editor, FileBackedDemoScene& owner, DiagnosticResolver& resolver, bool redo)
    {
        editor.ReplaySceneHistory(owner, redo);
        auto request = editor.TakeSceneHistoryReplayRequest();
        Check(request.has_value(), "History did not produce membership replay");
        std::string error;

        if (redo && request->remove)
        {
            auto changedSnapshot = request->object;
            changedSnapshot.physics->density += 1.0f;
            Check(!editor.StageObjectDeletionHistory(changedSnapshot, request->insertionIndex, {}, {},
                request->identity.operationID, request->identity.epoch, true, error),
                "Redo accepted a changed physics snapshot");
            Check(editor.StageObjectDeletionHistory(request->object, request->insertionIndex, {}, {},
                request->identity.operationID, request->identity.epoch, true, error), error);
        }

        auto candidate = request->remove ?
            owner.m_runtime->PrepareObjectDeletion(request->object.id, &error) :
            owner.m_runtime->PrepareObjectRestoration(resolver, request->object, request->insertionIndex, &error);
        Check(candidate != nullptr, error);
        owner.m_runtime = std::move(candidate);
        Check(editor.RebindSceneHistoryAfterStructureChange(owner), "Cannot rebind replayed object history");
        editor.CompleteSceneHistoryReplay(request->identity, true, {});
    }
};

int VerifyScenePhysicsData()
{
    try
    {
        VerifyDefaultsAndValidation();
        VerifyPersistenceBoundary();
        ScenePhysicsDataVerification::Run();
        std::cout << "PASS: scene physics defaults, validation, duplication, snapshot/history restoration, persistence boundary\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAIL: scene physics data: " << exception.what() << '\n';
        return 1;
    }
}

