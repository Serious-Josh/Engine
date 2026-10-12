#include "Scene/SceneFileCreation.h"
#include "Scene/SceneSerializer.h"
#include "Project/ProjectPaths.h"

#include <Windows.h>
#include <objbase.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    struct TemporaryProject
    {
        std::filesystem::path root;

        TemporaryProject()
        {
            GUID id{};
            Require(SUCCEEDED(CoCreateGuid(&id)), "Temporary identity generation failed.");
            wchar_t text[40]{};
            Require(StringFromGUID2(id, text, 40) != 0, "Temporary identity formatting failed.");
            root = std::filesystem::temp_directory_path() / text;
            Require(std::filesystem::create_directory(root), "Temporary directory creation failed.");
        }

        ~TemporaryProject()
        {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    };

    SceneDocument Read(const std::filesystem::path& path)
    {
        SceneDocument document;
        std::string error;
        Require(LoadSceneDocument(path, document, &error), "Created document could not be loaded.");
        Require(ValidateSceneDocument(document, &error), "Created document is invalid.");
        return document;
    }

    void RequireStarterSphere(const SceneDocument& document)
    {
        Require(document.objects.size() == 1, "New scene must contain exactly one object.");
        const auto& sphere = document.objects.front();
        Require(sphere.name == "Sphere", "Incorrect starter object name.");
        Require(!sphere.id.empty() && sphere.id != document.id, "Invalid starter object identity.");
        Require(sphere.meshReference == "builtin:sphere" && sphere.materialReference == "builtin:default",
                "Incorrect starter asset references.");
        const auto& transform = sphere.transform;
        Require(transform.position.x == 0.0f && transform.position.y == 0.0f && transform.position.z == 0.0f,
                "Incorrect starter position.");
        Require(transform.rotation.x == 0.0f && transform.rotation.y == 0.0f && transform.rotation.z == 0.0f,
                "Incorrect starter rotation.");
        Require(transform.scale.x == 1.0f && transform.scale.y == 1.0f && transform.scale.z == 1.0f,
                "Incorrect starter scale.");
    }

    void TestCreation()
    {
        TemporaryProject temporary;
        ProjectPaths project(temporary.root);
        std::filesystem::path path;
        std::string error;
        Require(CreateNewSceneFile(project, path, &error), "First creation failed.");
        Require(path == project.GetScenesDirectory() / "Untitled Scene.scene", "Wrong first filename.");
        const SceneDocument first = Read(path);
        const SceneDocument defaults;
        Require(first.name == "Untitled Scene", "Wrong initial display name.");
        RequireStarterSphere(first);
        Require(first.schemaVersion == 1 && !first.id.empty(), "Invalid scene identity or schema.");
        Require(first.camera.position.z == defaults.camera.position.z &&
                first.camera.aspectRatio == defaults.camera.aspectRatio &&
                first.directionalLight.intensity == defaults.directionalLight.intensity,
                "Default scene settings changed.");

        SceneDocument marker;
        marker.id = "existing-marker";
        marker.name = "Do not overwrite";
        const auto secondPath = project.GetScenesDirectory() / "Untitled Scene (2).scene";
        Require(SaveSceneDocument(marker, secondPath, &error), "Marker creation failed.");
        Require(CreateNewSceneFile(project, path, &error), "Gap creation failed.");
        Require(path.filename() == "Untitled Scene (1).scene", "Lowest suffix was not reused.");
        const auto second = Read(path);
        RequireStarterSphere(second);
        Require(second.objects.front().id != first.objects.front().id, "Starter object ID was reused.");
        Require(second.name == "Untitled Scene (1)" && second.id != first.id, "Name or unique ID incorrect.");
        Require(CreateNewSceneFile(project, path, &error), "Numbered creation failed.");
        Require(path.filename() == "Untitled Scene (3).scene", "Occupied suffix was not skipped.");
        Require(Read(secondPath).id == marker.id, "Existing scene was overwritten.");
        Require(Read(project.GetScenesDirectory() / "Untitled Scene.scene").id == first.id,
                "Original scene was overwritten.");
        Require(CreateSceneDocument(first, secondPath, &error) == CreateSceneDocumentResult::Collision,
                "Collision was not classified.");
        Require(Read(secondPath).name == marker.name, "Collision changed the destination.");

        SceneDocument invalid;
        Require(CreateSceneDocument(invalid, project.GetScenesDirectory() / "invalid.scene", &error) ==
                CreateSceneDocumentResult::Error, "Invalid document accepted.");
        Require(!std::filesystem::exists(project.GetScenesDirectory() / "invalid.scene"),
                "Invalid document was published.");
        Require(CreateSceneDocument(first, temporary.root / "missing" / "bad.scene", &error) ==
                CreateSceneDocumentResult::Error, "Missing directory was not an error.");

        // Force a deterministic directory/write failure without changing ACLs.
        const auto blockedRoot = temporary.root / "blocked";
        std::ofstream(blockedRoot) << "regular file";
        const auto preservedPath = path;
        Require(!CreateNewSceneFile(ProjectPaths(blockedRoot), path, &error) && !error.empty(),
                "Blocked project directory should fail.");
        Require(path == preservedPath, "Failure changed output path.");
        Require(!CreateNewSceneFile(ProjectPaths("relative-project"), path, &error),
                "Relative project root should fail.");

        // Save retains its existing replacement behavior.
        marker.name = "Replacement save";
        Require(SaveSceneDocument(marker, secondPath, &error), "Replacement Save failed.");
        Require(Read(secondPath).name == marker.name, "Save no longer replaces its destination.");

        for (const auto& entry : std::filesystem::directory_iterator(project.GetScenesDirectory()))
        {
            Require(entry.path().extension() == ".scene", "Temporary serialization file leaked.");
        }
    }

    void TestConcurrentCreation()
    {
        TemporaryProject temporary;
        ProjectPaths project(temporary.root);
        std::array<std::filesystem::path, 4> paths;
        std::array<bool, 4> success{};
        std::array<std::thread, 4> workers;

        for (std::size_t index = 0; index < workers.size(); ++index)
        {
            workers[index] = std::thread([&, index]
            {
                success[index] = CreateNewSceneFile(project, paths[index]);
            });
        }

        for (auto& worker : workers)
        {
            worker.join();
        }

        std::set<std::filesystem::path> uniquePaths;
        std::set<std::string> uniqueIDs;
        std::set<std::string> uniqueObjectIDs;

        for (std::size_t index = 0; index < paths.size(); ++index)
        {
            Require(success[index], "Concurrent creation failed.");
            uniquePaths.insert(paths[index]);
            const auto document = Read(paths[index]);
            RequireStarterSphere(document);
            uniqueIDs.insert(document.id);
            uniqueObjectIDs.insert(document.objects.front().id);
            Require(Read(paths[index]).name == paths[index].stem().string(), "Concurrent display name mismatch.");
        }

        Require(uniquePaths.size() == paths.size() && uniqueIDs.size() == paths.size() &&
                uniqueObjectIDs.size() == paths.size(),
                "Concurrent creation reused a file, scene, or object identity.");
    }
}

int main()
{
    try
    {
        TestCreation();
        TestConcurrentCreation();
        std::cout << "Scene file creation tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
