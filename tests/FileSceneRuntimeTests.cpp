
#include "Scene/FileSceneRuntime.h"
#include "Scene/SceneAssetResolver.h"
#include "Scene/SceneDocumentAdapter.h"
#include "Scene/SceneSerializer.h"
#include <filesystem>
#include <chrono>
#include <limits>

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(EXIT_FAILURE);
        }
    }

    class MockResolver final : public SceneAssetResolver
    {
    public:
        bool allowMesh = true;
        bool allowMaterial = true;

        Mesh* ResolveMesh(std::string_view) override
        {
            return allowMesh ? reinterpret_cast<Mesh*>(&meshToken) : nullptr;
        }

        Material* ResolveMaterial(std::string_view) override
        {
            return allowMaterial
                ? reinterpret_cast<Material*>(&materialToken)
                : nullptr;
        }

    private:
        unsigned char meshToken = 0;
        unsigned char materialToken = 0;
    };

    void AddObject(FileSceneRuntime& runtime, const std::string& id)
    {
        SceneObjectDocument object;
        object.id = id;
        object.name = id;
        object.meshReference = "mesh:test";
        object.materialReference = "material:test";

        runtime.EditDocument().objects.push_back(object);
    }

    void TestPrepareAndCommit()
    {
        FileSceneRuntime runtime;
        MockResolver resolver;
        std::string error;

        Require(runtime.New("test-scene"), "New scene failed.");
        AddObject(runtime, "object-a");

        Require(runtime.Prepare(resolver, &error),
                "Preparation should succeed.");

        Require(runtime.HasPreparedScene(),
                "Prepared snapshot should exist.");

        Require(runtime.GetScene().GetRenderObjects().empty(),
                "Preparation must not populate the runtime scene.");

        Require(runtime.Commit(&error), "Commit should succeed.");

        Require(!runtime.HasPreparedScene(),
                "Successful commit must consume prepared snapshot.");

        Require(runtime.GetScene().GetRenderObjects().size() == 1,
                "Commit should populate one object.");

        Require(runtime.GetObjectMapping().idToRuntimeIndex.at("object-a") == 0,
                "Persistent-ID mapping is incorrect.");

        Require(!runtime.Commit(&error),
                "Commit without preparation must fail.");
    }

    void TestEditingInvalidatesPreparation()
    {
        FileSceneRuntime runtime;
        MockResolver resolver;
        std::string error;

        Require(runtime.New("edit-test"), "New scene failed.");
        AddObject(runtime, "original");

        Require(runtime.Prepare(resolver, &error),
                "Initial preparation failed.");

        runtime.EditDocument().name = "Modified";

        Require(!runtime.HasPreparedScene(),
                "Editing should invalidate preparation.");

        Require(!runtime.Commit(&error),
                "Commit should reject invalidated preparation.");

        Require(runtime.GetScene().GetRenderObjects().empty(),
                "Rejected commit must not change runtime objects.");
    }

    void TestNewInvalidatesPreparation()
    {
        FileSceneRuntime runtime;
        MockResolver resolver;
        std::string error;

        Require(runtime.New("first"), "First New failed.");
        Require(runtime.Prepare(resolver, &error),
                "Initial preparation failed.");

        Require(runtime.New("second"), "Second New failed.");

        Require(!runtime.HasPreparedScene(),
                "New should invalidate preparation.");

        Require(runtime.GetFileScene().GetDocument().id == "second",
                "New did not replace document.");
    }

    void TestFailedPreparePreservesCommittedScene()
    {
        FileSceneRuntime runtime;
        MockResolver resolver;
        std::string error;

        Require(runtime.New("failure-test"), "New scene failed.");
        AddObject(runtime, "existing");

        Require(runtime.Prepare(resolver, &error),
                "Initial preparation failed.");

        Require(runtime.Commit(&error),
                "Initial commit failed.");

        const auto revision = runtime.GetScene().GetStructureRevision();

        AddObject(runtime, "unresolved");

        resolver.allowMesh = false;

        Require(!runtime.Prepare(resolver, &error),
                "Preparation should fail when mesh resolution fails.");

        Require(runtime.GetScene().GetStructureRevision() == revision,
                "Failed preparation changed runtime structure.");

        Require(runtime.GetScene().GetRenderObjects().size() == 1,
                "Failed preparation changed committed objects.");

        Require(runtime.GetObjectMapping().idToRuntimeIndex.contains("existing"),
                "Failed preparation changed committed mapping.");

        Require(!runtime.HasPreparedScene(),
                "Failed preparation must not create a snapshot.");
    }

    void TestLiveSave()
    {
        FileSceneRuntime runtime;
        MockResolver resolver;
        std::string error;
        bool dirty = false;
        const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto path = std::filesystem::temp_directory_path() /
            ("EngineLiveSceneSaveTest-" + std::to_string(serial) + ".scene");

        Require(runtime.New("live-save", "Live Scene"), "New failed.");
        AddObject(runtime, "object-a");
        AddObject(runtime, "object-b");
        Require(runtime.Prepare(resolver, &error) && runtime.Commit(&error), "Commit failed.");
        Require(runtime.SaveAs(path, &error), "Initial save failed.");
        Require(runtime.CheckUnsavedChanges(dirty, &error) && !dirty, "Initial save should be clean.");

        runtime.GetScene().GetCamera().SetAspectRatio(2.5f);
        Require(runtime.CheckUnsavedChanges(dirty, &error) && !dirty, "Resize must not dirty the scene.");
        auto transform = runtime.GetScene().GetRenderObjects()[0].transform;
        transform.position.x = 7.0f;
        runtime.GetScene().SetRenderObjectTransform(0, transform);
        runtime.GetScene().GetCamera().SetPosition({1.0f, 2.0f, -4.0f});
        runtime.GetScene().GetCamera().SetTarget({2.0f, 1.0f, 0.0f});
        runtime.GetScene().GetCamera().SetVerticalFov(0.9f);
        runtime.GetScene().GetCamera().SetNearPlane(0.2f);
        runtime.GetScene().GetCamera().SetFarPlane(500.0f);
        runtime.GetScene().GetDirectionalLight().intensity = 3.0f;
        runtime.GetScene().GetDirectionalLight().color = {0.2f, 0.3f, 0.4f};
        Require(runtime.CheckUnsavedChanges(dirty, &error) && dirty, "Live edits should be dirty.");

        const SceneDocument before = runtime.GetFileScene().GetDocument();
        Require(!runtime.SaveAs(path / "invalid.scene", &error), "Invalid destination must fail.");
        Require(SceneDocumentsEqual(before, runtime.GetFileScene().GetDocument()), "Failed save changed document.");
        Require(runtime.CheckUnsavedChanges(dirty, &error) && dirty, "Failed save lost dirty state.");
        Require(runtime.Save(&error), "Live save failed.");
        Require(runtime.CheckUnsavedChanges(dirty, &error) && !dirty, "Successful save should be clean.");
        Require(!runtime.GetScene().GetChangedTransforms().empty(), "Save cleared renderer transform tracking.");

        SceneDocument loaded;
        Require(LoadSceneDocument(path, loaded, &error), "Reload failed.");
        Require(loaded.objects[0].transform.position.x == 7.0f, "Transform was not persisted.");
        Require(loaded.camera.position.x == 1.0f && loaded.camera.target.x == 2.0f, "Camera navigation lost.");
        Require(loaded.camera.verticalFov == 0.9f && loaded.camera.farPlane == 500.0f, "Camera properties lost.");
        Require(loaded.camera.aspectRatio == before.camera.aspectRatio, "Viewport aspect overwrote saved aspect.");
        Require(loaded.directionalLight.intensity == 3.0f && loaded.directionalLight.color.y == 0.3f, "Light lost.");
        Require(loaded.id == before.id && loaded.name == before.name, "Scene identity changed.");
        Require(loaded.objects[0].id == before.objects[0].id && loaded.objects[1].id == before.objects[1].id,
                "Object identity or ordering changed.");
        Require(loaded.objects[0].name == before.objects[0].name &&
                loaded.objects[0].meshReference == before.objects[0].meshReference &&
                loaded.objects[0].materialReference == before.objects[0].materialReference, "Asset metadata changed.");

        // Explicit document edits may change aspect; viewport changes may not.
        runtime.EditDocument().camera.aspectRatio = 1.25f;
        Require(runtime.CheckUnsavedChanges(dirty, &error) && dirty, "Explicit aspect edit should be dirty.");
        Require(runtime.Save(&error), "Explicit aspect save failed.");
        Require(runtime.GetFileScene().GetDocument().camera.aspectRatio == 1.25f, "Explicit aspect edit lost.");
        Require(runtime.CheckUnsavedChanges(dirty, &error) && !dirty, "Explicit aspect save should be clean.");

        SceneDocument captured = before;
        SceneObjectMapping bad = runtime.GetObjectMapping();
        bad.idToRuntimeIndex["object-a"] = 99;
        Require(!CaptureSceneDocument(loaded, runtime.GetScene(), bad, captured, &error), "Invalid index accepted.");
        Require(SceneDocumentsEqual(captured, before), "Failed capture changed output.");
        bad = runtime.GetObjectMapping();
        bad.idToRuntimeIndex["object-b"] = 0;
        Require(!CaptureSceneDocument(loaded, runtime.GetScene(), bad, captured, &error), "Duplicate mapping accepted.");
        auto& object = runtime.GetScene().GetRenderObjects()[0];
        Mesh* originalMesh = object.mesh;
        object.mesh = nullptr;
        Require(!runtime.Save(&error), "Runtime asset replacement accepted.");
        object.mesh = originalMesh;
        runtime.GetScene().GetCamera().SetNearPlane(std::numeric_limits<float>::quiet_NaN());
        Require(!runtime.Save(&error), "Invalid live values accepted.");
        runtime.GetScene().GetCamera().SetNearPlane(0.2f);
        runtime.GetScene().AddRenderObject(runtime.GetScene().GetRenderObjects()[0]);
        Require(!runtime.Save(&error), "Structural change accepted.");

        std::filesystem::remove(path);
    }

    void TestDiscardPreparation()
    {
        FileSceneRuntime runtime;
        MockResolver resolver;
        std::string error;

        Require(runtime.New("discard-test"), "New scene failed.");
        Require(runtime.Prepare(resolver, &error),
                "Preparation failed.");

        runtime.DiscardPreparedScene();

        Require(!runtime.HasPreparedScene(),
                "Discard did not clear preparation.");

        Require(!runtime.Commit(&error),
                "Discarded snapshot should not be committable.");
    }
}

int main()
{
    TestLiveSave();
    TestPrepareAndCommit();
    TestEditingInvalidatesPreparation();
    TestNewInvalidatesPreparation();
    TestFailedPreparePreservesCommittedScene();
    TestDiscardPreparation();

    std::cout << "File scene runtime tests passed.\n";
    return EXIT_SUCCESS;
}
