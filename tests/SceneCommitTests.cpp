
#include "Scene/SceneCommit.h"

#include <cstdlib>
#include <iostream>
#include <string>

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

    // Identity tokens only. No GPU resources are accessed.
    Mesh* TestMesh()
    {
        static unsigned char token;
        return reinterpret_cast<Mesh*>(&token);
    }

    Material* TestMaterial()
    {
        static unsigned char token;
        return reinterpret_cast<Material*>(&token);
    }

    ResolvedSceneObject MakeObject(
        const std::string& id,
        float positionX)
    {
        ResolvedSceneObject object;
        object.id = id;
        object.name = id;
        object.renderObject.mesh = TestMesh();
        object.renderObject.material = TestMaterial();
        object.renderObject.transform.position.x = positionX;
        object.renderObject.previousTransform =
            object.renderObject.transform;

        return object;
    }

    void TestSuccessfulCommit()
    {
        Scene scene;
        SceneObjectMapping mapping;

        PreparedScene prepared;
        prepared.camera.position = {2.0f, 3.0f, -8.0f};
        prepared.camera.verticalFov = 0.9f;
        prepared.camera.nearPlane = 0.25f;
        prepared.camera.farPlane = 500.0f;
        prepared.directionalLight.intensity = 2.5f;

        prepared.objects.push_back(MakeObject("object-a", 1.0f));
        prepared.objects.push_back(MakeObject("object-b", 4.0f));

        std::string error;

        Require(CommitPreparedScene(prepared, scene, mapping, &error),
                "Valid prepared scene should commit.");

        Require(scene.GetRenderObjects().size() == 2,
                "Expected two runtime objects.");

        Require(mapping.idToRuntimeIndex.at("object-a") == 0,
                "Incorrect mapping for object-a.");

        Require(mapping.idToRuntimeIndex.at("object-b") == 1,
                "Incorrect mapping for object-b.");

        Require(scene.GetRenderObjects()[1].transform.position.x == 4.0f,
                "Object transform was not preserved.");

        Require(scene.GetCamera().GetPosition().x == 2.0f,
                "Camera position was not transferred.");

        Require(scene.GetCamera().GetVerticalFov() == 0.9f,
                "Camera FOV was not transferred.");

        Require(scene.GetDirectionalLight().intensity == 2.5f,
                "Directional light was not transferred.");
    }

    void TestReplacement()
    {
        Scene scene;
        SceneObjectMapping mapping;
        std::string error;

        PreparedScene first;
        first.objects.push_back(MakeObject("old-object", 1.0f));

        Require(CommitPreparedScene(first, scene, mapping, &error),
                "Initial commit failed.");

        const auto previousRevision = scene.GetStructureRevision();

        PreparedScene replacement;
        replacement.objects.push_back(MakeObject("new-object", 9.0f));

        Require(CommitPreparedScene(replacement, scene, mapping, &error),
                "Replacement commit failed.");

        Require(scene.GetStructureRevision() > previousRevision,
                "Scene structure revision did not advance.");

        Require(scene.GetRenderObjects().size() == 1,
                "Replacement did not remove old objects.");

        Require(mapping.idToRuntimeIndex.size() == 1,
                "Replacement did not clear old mapping.");

        Require(mapping.idToRuntimeIndex.contains("new-object"),
                "Replacement mapping is missing new-object.");

        Require(!mapping.idToRuntimeIndex.contains("old-object"),
                "Old object mapping survived replacement.");
    }

    void TestInvalidSnapshotPreservesScene()
    {
        Scene scene;
        SceneObjectMapping mapping;
        std::string error;

        PreparedScene valid;
        valid.objects.push_back(MakeObject("original", 3.0f));

        Require(CommitPreparedScene(valid, scene, mapping, &error),
                "Initial commit failed.");

        const auto revision = scene.GetStructureRevision();
        const auto originalPosition =
            scene.GetRenderObjects()[0].transform.position.x;

        PreparedScene invalid;
        invalid.objects.push_back(MakeObject("invalid", 7.0f));
        invalid.objects[0].renderObject.mesh = nullptr;

        Require(!CommitPreparedScene(invalid, scene, mapping, &error),
                "Null mesh should reject commit.");

        Require(scene.GetStructureRevision() == revision,
                "Rejected commit changed structure revision.");

        Require(scene.GetRenderObjects().size() == 1,
                "Rejected commit changed object count.");

        Require(scene.GetRenderObjects()[0].transform.position.x ==
                    originalPosition,
                "Rejected commit changed the original object.");

        Require(mapping.idToRuntimeIndex.contains("original"),
                "Rejected commit changed the mapping.");

        // Duplicate persistent IDs must also fail.
        invalid.objects.clear();
        invalid.objects.push_back(MakeObject("duplicate", 1.0f));
        invalid.objects.push_back(MakeObject("duplicate", 2.0f));

        Require(!CommitPreparedScene(invalid, scene, mapping, &error),
                "Duplicate persistent IDs should reject commit.");

        Require(scene.GetStructureRevision() == revision,
                "Duplicate IDs changed the scene revision.");

        Require(mapping.idToRuntimeIndex.contains("original"),
                "Duplicate IDs changed the mapping.");
    }

    void TestEmptySceneCommit()
    {
        Scene scene;
        SceneObjectMapping mapping;
        std::string error;

        PreparedScene populated;
        populated.objects.push_back(MakeObject("existing", 1.0f));

        Require(CommitPreparedScene(populated, scene, mapping, &error),
                "Initial commit failed.");

        PreparedScene empty;

        Require(CommitPreparedScene(empty, scene, mapping, &error),
                "Empty scene commit failed.");

        Require(scene.GetRenderObjects().empty(),
                "Empty commit did not clear runtime objects.");

        Require(mapping.idToRuntimeIndex.empty(),
                "Empty commit did not clear persistent-ID mapping.");
    }
}

int main()
{
    TestSuccessfulCommit();
    TestReplacement();
    TestInvalidSnapshotPreservesScene();
    TestEmptySceneCommit();

    std::cout << "Scene commit tests passed.\n";
    return EXIT_SUCCESS;
}
