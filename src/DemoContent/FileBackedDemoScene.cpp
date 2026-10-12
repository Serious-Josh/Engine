
#include "DemoContent/FileBackedDemoScene.h"
#include "DemoContent/SceneRegistry.h"

#include "Scene/Camera.h"
#include "Scene/DirectionalLight.h"

#include "Core/Log.h"

#include "Project/ProjectLocator.h"
#include "Project/ProjectPaths.h"

#include <memory>
#include <string>
#include <utility>

namespace
{
    std::unique_ptr<IDemoScene> CreateFileBackedTestScene()
    {
        const ProjectPaths paths(
            ProjectLocator::GetDefaultProjectRoot());

        const std::filesystem::path scenePath =
            paths.GetScenesDirectory() / "FirstFileBackedScene.scene";

        return std::make_unique<FileBackedDemoScene>(scenePath);
    }

    const bool g_fileBackedTestRegistered =
        SceneRegistry::Instance().Register(
            "File-Backed Test",
            &CreateFileBackedTestScene);
}

FileBackedDemoScene::FileBackedDemoScene(
    std::filesystem::path scenePath)
    : m_scenePath(std::move(scenePath))
{
}

void FileBackedDemoScene::SetProjectAssets(
    const std::filesystem::path& root,
    const ProjectAssetRegistry& registry)
{
    m_projectRoot = root;
    m_registry = &registry;
}

bool FileBackedDemoScene::Initialize(
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    const Shader* vertexShader,
    const Shader* pixelShader)
{
    if (m_initialized)
    {
        return false;
    }

    // Initialization is performed while this scene is inactive.
    // Engine remains responsible for GPU synchronization when
    // replacing a previously active scene.
    Shutdown();

    std::string error;

    // Validate the document before allocating GPU resources.
    if (!m_runtime.Open(m_scenePath, &error))
    {
        Log::Error("Failed to open file-backed scene: " + error);
        return false;
    }

    if (!m_assets.Initialize(
            device,
            uploader,
            descriptorHeap,
            vertexShader,
            pixelShader))
    {
        Log::Error("Failed to initialize file-backed scene assets.");
        Shutdown();
        return false;
    }

    if(m_registry)
    {
        m_assets.SetProjectAssets(ProjectPaths(m_projectRoot), *m_registry);
    }

    if (!m_runtime.Prepare(m_assets, &error))
    {
        Log::Error("Failed to prepare file-backed scene: " + error + " " + m_assets.GetLastMaterialError());
        Shutdown();
        return false;
    }

    if (!m_runtime.Commit(&error))
    {
        Log::Error("Failed to commit file-backed scene: " + error);
        Shutdown();
        return false;
    }

    m_initialized = true;
    return true;
}

void FileBackedDemoScene::Shutdown()
{
    // This must only run after Engine has safely released any
    // renderer resources referencing this scene.
    m_initialized = false;

    m_runtime.DiscardPreparedScene();
    m_runtime.GetScene().ClearRenderObjects();

    // The catalog owns the objects referenced by RenderObject.
    m_assets.Shutdown();
}

void FileBackedDemoScene::Update(float deltaTime)
{
    (void)deltaTime;
}

Scene& FileBackedDemoScene::GetScene()
{
    return m_runtime.GetScene();
}

const Scene& FileBackedDemoScene::GetScene() const
{
    return m_runtime.GetScene();
}

std::vector<Material*> FileBackedDemoScene::GetMaterials()
{
    return m_assets.GetMaterials();
}

void FileBackedDemoScene::ResetCamera()
{
    if (!m_runtime.GetFileScene().HasDocument())
    {
        return;
    }

    const SceneCameraDocument& source =
        m_runtime.GetFileScene().GetDocument().camera;

    Camera& camera = m_runtime.GetScene().GetCamera();

    camera.SetPosition(source.position);
    camera.SetTarget(source.target);
    camera.SetVerticalFov(source.verticalFov);
    camera.SetAspectRatio(source.aspectRatio);
    camera.SetNearPlane(source.nearPlane);
    camera.SetFarPlane(source.farPlane);
}

void FileBackedDemoScene::ResetDirectionalLight()
{
    if (!m_runtime.GetFileScene().HasDocument())
    {
        return;
    }

    m_runtime.GetScene().GetDirectionalLight() =
        m_runtime.GetFileScene().GetDocument().directionalLight;
}

void FileBackedDemoScene::SetAspectRatio(float aspectRatio)
{
    m_runtime.GetScene().GetCamera().SetAspectRatio(aspectRatio);
}

const std::string& FileBackedDemoScene::GetDisplayName() const noexcept
{
    return m_runtime.GetFileScene().GetDocument().name;
}

bool FileBackedDemoScene::Save(std::string* errorMessage)
{
    if (!m_initialized)
    {
        if (errorMessage)
        {
            *errorMessage = "Cannot save an uninitialized file-backed scene.";
        }

        return false;
    }

    return m_runtime.Save(errorMessage);
}

bool FileBackedDemoScene::CheckUnsavedChanges(
    bool& dirty,
    std::string* errorMessage) const
{
    return m_runtime.CheckUnsavedChanges(dirty, errorMessage);
}
