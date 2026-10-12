
#include "Scene/SceneDocument.h"
#include "Scene/SceneSerializer.h"
#include "Scene/FileScene.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
    namespace fs = std::filesystem;
    using Json = nlohmann::json;

    void Require(bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    SceneDocument MakeScene()
    {
        SceneDocument scene;
        scene.id = "scene-test";
        scene.name = "Serializer Test";

        scene.camera.position = {2.0f, 3.0f, -8.0f};
        scene.camera.target = {0.0f, 1.0f, 0.0f};
        scene.camera.verticalFov = 0.9f;
        scene.camera.aspectRatio = 16.0f / 10.0f;
        scene.camera.nearPlane = 0.2f;
        scene.camera.farPlane = 500.0f;

        scene.directionalLight.direction = {0.2f, -0.8f, 0.4f};
        scene.directionalLight.color = {1.0f, 0.8f, 0.6f};
        scene.directionalLight.intensity = 3.0f;
        scene.directionalLight.ambientStrength = 0.12f;
        scene.directionalLight.angularRadius = 0.01f;

        SceneObjectDocument object;
        object.id = "object-001";
        object.name = "Test Cube";
        object.meshReference = "builtin:cube";
        object.materialReference = "Materials/Stone.material";

        object.transform.position = {1.0f, 2.0f, 3.0f};
        object.transform.rotation = {0.1f, 0.2f, 0.3f};
        object.transform.scale = {2.0f, 1.5f, 0.5f};

        scene.objects.push_back(object);
        return scene;
    }

    void TestRoundTrip(const fs::path& path)
    {
        const SceneDocument original = MakeScene();
        std::string error;

        Require(SaveSceneDocument(original, path, &error),
                "Save failed: " + error);

        SceneDocument loaded;
        Require(LoadSceneDocument(path, loaded, &error),
                "Load failed: " + error);

        Require(loaded.schemaVersion == original.schemaVersion,
                "Schema version mismatch.");
        Require(loaded.id == original.id, "Scene ID mismatch.");
        Require(loaded.name == original.name, "Scene name mismatch.");

        const auto& a = original.camera;
        const auto& b = loaded.camera;

        Require(b.position.x == a.position.x &&
                b.position.y == a.position.y &&
                b.position.z == a.position.z &&
                b.target.x == a.target.x &&
                b.target.y == a.target.y &&
                b.target.z == a.target.z &&
                b.verticalFov == a.verticalFov &&
                b.aspectRatio == a.aspectRatio &&
                b.nearPlane == a.nearPlane &&
                b.farPlane == a.farPlane,
                "Camera round-trip mismatch.");

        const auto& lightA = original.directionalLight;
        const auto& lightB = loaded.directionalLight;

        Require(lightB.direction.x == lightA.direction.x &&
                lightB.direction.y == lightA.direction.y &&
                lightB.direction.z == lightA.direction.z &&
                lightB.color.x == lightA.color.x &&
                lightB.color.y == lightA.color.y &&
                lightB.color.z == lightA.color.z &&
                lightB.intensity == lightA.intensity &&
                lightB.ambientStrength == lightA.ambientStrength &&
                lightB.angularRadius == lightA.angularRadius,
                "Directional light round-trip mismatch.");

        Require(loaded.objects.size() == 1, "Object count mismatch.");

        const auto& obj = loaded.objects.front();
        const auto& expected = original.objects.front();

        Require(obj.id == expected.id &&
                obj.name == expected.name &&
                obj.meshReference == expected.meshReference &&
                obj.materialReference == expected.materialReference,
                "Object metadata mismatch.");

        const auto& t = obj.transform;
        const auto& e = expected.transform;

        Require(t.position.x == e.position.x &&
                t.position.y == e.position.y &&
                t.position.z == e.position.z &&
                t.rotation.x == e.rotation.x &&
                t.rotation.y == e.rotation.y &&
                t.rotation.z == e.rotation.z &&
                t.scale.x == e.scale.x &&
                t.scale.y == e.scale.y &&
                t.scale.z == e.scale.z,
                "Transform round-trip mismatch.");
    }

    void TestInvalidDocuments(const fs::path& path)
    {
        std::string error;

        auto scene = MakeScene();
        scene.objects.push_back(scene.objects.front());

        Require(!SaveSceneDocument(scene, path, &error),
                "Duplicate IDs should be rejected.");

        scene = MakeScene();
        scene.id.clear();

        Require(!SaveSceneDocument(scene, path, &error),
                "Missing scene ID should be rejected.");

        scene = MakeScene();
        scene.objects.front().id.clear();

        Require(!SaveSceneDocument(scene, path, &error),
                "Missing object ID should be rejected.");

        scene = MakeScene();
        scene.schemaVersion = 999;

        Require(!SaveSceneDocument(scene, path, &error),
                "Unsupported schema should be rejected.");

        scene = MakeScene();
        scene.objects.front().transform.position.x =
            std::numeric_limits<float>::quiet_NaN();

        Require(!SaveSceneDocument(scene, path, &error),
                "Nonfinite transform should be rejected.");
    }

    void TestInvalidLoads(const fs::path& path)
    {
        SceneDocument destination = MakeScene();
        destination.id = "preserve-this";
        std::string error;

        {
            std::ofstream file(path, std::ios::trunc);
            file << "{ invalid json";
        }

        Require(!LoadSceneDocument(path, destination, &error),
                "Malformed JSON should be rejected.");
        Require(destination.id == "preserve-this",
                "Failed load changed destination.");

        {
            std::ofstream file(path, std::ios::trunc);
            file << R"({"schemaVersion":1,"id":"missing-fields"})";
        }

        Require(!LoadSceneDocument(path, destination, &error),
                "Missing required fields should be rejected.");
        Require(destination.id == "preserve-this",
                "Missing-field load changed destination.");

        // Begin with a valid saved scene, then modify its JSON.
        Require(SaveSceneDocument(MakeScene(), path, &error),
                "Failed to prepare valid scene.");

        Json json;
        {
            std::ifstream file(path);
            file >> json;
        }

        json["schemaVersion"] = 999;

        {
            std::ofstream file(path, std::ios::trunc);
            file << json.dump(4);
        }

        Require(!LoadSceneDocument(path, destination, &error),
                "Unsupported schema should be rejected.");
        Require(destination.id == "preserve-this",
                "Unsupported-schema load changed destination.");

        json["schemaVersion"] = 1;
        json["objects"].push_back(json["objects"].front());

        {
            std::ofstream file(path, std::ios::trunc);
            file << json.dump(4);
        }

        Require(!LoadSceneDocument(path, destination, &error),
                "Duplicate object IDs should be rejected.");
        Require(destination.id == "preserve-this",
                "Duplicate-ID load changed destination.");
    }

    void TestEmptyScene(const fs::path& path)
    {
        SceneDocument scene;
        scene.id = "empty-scene";

        std::string error;

        Require(SaveSceneDocument(scene, path, &error),
                "Empty scene save failed: " + error);

        SceneDocument loaded;
        Require(LoadSceneDocument(path, loaded, &error),
                "Empty scene load failed: " + error);

        Require(loaded.objects.empty(),
                "Empty scene unexpectedly contains objects.");
    }

    void TestReplacement(const fs::path& path)
    {
        SceneDocument first = MakeScene();
        SceneDocument second = MakeScene();
        second.name = "Replacement Scene";

        std::string error;

        Require(SaveSceneDocument(first, path, &error),
                "Initial save failed: " + error);

        Require(SaveSceneDocument(second, path, &error),
                "Replacement save failed: " + error);

        SceneDocument loaded;
        Require(LoadSceneDocument(path, loaded, &error),
                "Replacement load failed: " + error);

        Require(loaded.name == "Replacement Scene",
                "Existing scene was not replaced.");
    }

    void TestFailedSavePreservesFile(const fs::path& path)
    {
        const SceneDocument valid = MakeScene();
        std::string error;

        Require(SaveSceneDocument(valid, path, &error),
                "Initial save failed: " + error);

        SceneDocument invalid = valid;
        invalid.id.clear();

        Require(!SaveSceneDocument(invalid, path, &error),
                "Invalid save should fail.");

        SceneDocument loaded;
        Require(LoadSceneDocument(path, loaded, &error),
                "Original scene was damaged.");

        Require(loaded.id == valid.id &&
                loaded.name == valid.name,
                "Failed save changed existing scene.");
    }

    void TestFileSceneLifecycle(const fs::path& path)
    {
        FileScene scene;
        std::string error;

        Require(!scene.HasDocument(),
                "New FileScene should have no document.");

        Require(!scene.Save(&error),
                "Saving without a document should fail.");

        Require(scene.New("scene-001", "My Scene"),
                "Failed to create a new scene.");

        Require(scene.HasDocument(),
                "New scene should have a document.");

        Require(scene.IsDirty(),
                "New scene should be dirty.");

        Require(scene.GetPath().empty(),
                "New scene should have no file path.");

        Require(!scene.Save(&error),
                "Saving without a path should fail.");

        Require(scene.IsDirty(),
                "Failed save should preserve dirty state.");

        Require(scene.SaveAs(path, &error),
                "SaveAs failed: " + error);

        Require(!scene.IsDirty(),
                "Successful SaveAs should clear dirty state.");

        Require(scene.GetPath() == path,
                "SaveAs should update the scene path.");

        SceneDocument& document = scene.EditDocument();
        document.name = "Edited Scene";

        Require(scene.IsDirty(),
                "Editing should mark the scene dirty.");

        Require(scene.Save(&error),
                "Save failed: " + error);

        Require(!scene.IsDirty(),
                "Successful save should clear dirty state.");

        FileScene reopened;

        Require(reopened.Open(path, &error),
                "Open failed: " + error);

        Require(reopened.HasDocument(),
                "Opened scene should have a document.");

        Require(!reopened.IsDirty(),
                "Opened scene should not be dirty.");

        Require(reopened.GetDocument().name == "Edited Scene",
                "Edited scene name was not persisted.");

        // Failed opening must preserve the existing document.
        const fs::path missingPath =
            path.parent_path() / "nonexistent.scene";

        Require(!reopened.Open(missingPath, &error),
                "Opening a missing file should fail.");

        Require(reopened.GetDocument().name == "Edited Scene",
                "Failed open replaced the existing document.");

        Require(reopened.GetPath() == path,
                "Failed open changed the existing path.");

        Require(!reopened.IsDirty(),
                "Failed open changed dirty state.");

        // A failed save must not clear unsaved changes.
        reopened.EditDocument().id.clear();

        Require(reopened.IsDirty(),
                "Invalid edit should mark scene dirty.");

        Require(!reopened.Save(&error),
                "Saving an invalid scene should fail.");

        Require(reopened.IsDirty(),
                "Failed save cleared dirty state.");

        SceneDocument persisted;
        Require(LoadSceneDocument(path, persisted, &error),
                "Failed save damaged the existing file.");

        Require(persisted.id == "scene-001",
                "Failed save changed the persisted scene.");

        // New replaces the in-memory document, but not the file.
        Require(reopened.New("scene-002", "Another Scene"),
                "Failed to create replacement scene.");

        Require(reopened.GetPath().empty(),
                "New should clear the previous file path.");

        Require(reopened.GetDocument().id == "scene-002",
                "New did not replace the document.");

        Require(reopened.IsDirty(),
                "Replacement scene should be dirty.");
    }
}

int main()
{
    namespace fs = std::filesystem;

    try
    {
        const fs::path directory =
            fs::temp_directory_path() / "EngineSceneSerializerTests";

        fs::create_directories(directory);

        const fs::path path = directory / "test.scene";

        TestRoundTrip(path);
        TestInvalidDocuments(path);
        TestInvalidLoads(path);
        TestEmptyScene(path);
        TestReplacement(path);
        TestFailedSavePreservesFile(path);
        TestFileSceneLifecycle(path);

        fs::remove_all(directory);

        std::cout << "All scene serializer tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Scene serializer test failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
