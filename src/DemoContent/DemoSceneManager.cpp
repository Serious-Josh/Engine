#include "DemoSceneManager.h"

#include "DemoContent/SphereGridScene.h"
#include "Core/Log.h"

#include <utility>


bool DemoSceneManager::Initialize(
    std::string_view sceneName,
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    const Shader* vertexShader,
    const Shader* pixelShader,
    float aspectRatio)
{
    m_aspectRatio = aspectRatio;
    m_hasPendingSceneChange = false;

    const DemoSceneEntry* sceneEntry = SceneRegistry::Instance().Find(sceneName);

    if(!sceneEntry)
    {
        Log::Error("Failed to find demo scene.");
        return false;
    }

    m_activeSceneEntry = sceneEntry;
    m_requestedSceneEntry = sceneEntry;

    m_activeScene = sceneEntry->create();

    if(!m_activeScene)
    {
        Log::Error("Failed to create demo scene.");
        return false;
    }

    if(!m_activeScene->Initialize(
        device,
        uploader,
        descriptorHeap,
        vertexShader,
        pixelShader))
    {
        Log::Error("Failed to initialize demo scene.");
        m_activeScene.reset();
        return false;
    }

    m_activeScene->SetAspectRatio(m_aspectRatio);

    return true;
}

void DemoSceneManager::Shutdown()
{
    if(m_activeScene)
    {
        m_activeScene->Shutdown();
        m_activeScene.reset();
    }

    m_activeSceneEntry = nullptr;
    m_requestedSceneEntry = nullptr;
    m_hasPendingSceneChange = false;
}

bool DemoSceneManager::RequestScene(
    std::string_view sceneName)
{
    const DemoSceneEntry* sceneEntry =
        SceneRegistry::Instance().Find(sceneName);

    if(!sceneEntry)
    {
        Log::Error("Requested demo scene was not found.");
        return false;
    }

    if(sceneEntry == m_activeSceneEntry)
    {
        m_hasPendingSceneChange = false;
        m_requestedSceneEntry = m_activeSceneEntry;
        return true;
    }

    m_requestedSceneEntry = sceneEntry;
    m_hasPendingSceneChange = true;

    return true;
}

bool DemoSceneManager::HasPendingSceneChange() const
{
    return m_hasPendingSceneChange;
}

bool DemoSceneManager::ApplyPendingSceneChange(
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    const Shader* vertexShader,
    const Shader* pixelShader)
{
    if(!m_hasPendingSceneChange)
    {
        return true;
    }

    if(!m_requestedSceneEntry)
    {
        Log::Error("Requested demo scene entry is invalid.");
        return false;
    }

    if(m_activeScene)
    {
        m_activeScene->Shutdown();
        m_activeScene.reset();
    }

    std::unique_ptr<IDemoScene> newScene = m_requestedSceneEntry->create();

    if(!newScene)
    {
        Log::Error("Failed to create requested demo scene.");
        return false;
    }

    if(!newScene->Initialize(
        device,
        uploader,
        descriptorHeap,
        vertexShader,
        pixelShader))
    {
        Log::Error("Failed to initialize requested demo scene.");
        newScene->Shutdown();
        return false;
    }

    newScene->SetAspectRatio(m_aspectRatio);

    m_activeScene = std::move(newScene);
    m_activeSceneEntry = m_requestedSceneEntry;
    m_hasPendingSceneChange = false;

    return true;
}

void DemoSceneManager::SetAspectRatio(float aspectRatio)
{
    m_aspectRatio = aspectRatio;

    if(m_activeScene)
    {
        m_activeScene->SetAspectRatio(aspectRatio);
    }
}

IDemoScene& DemoSceneManager::GetActiveScene()
{
    return *m_activeScene;
}

const IDemoScene& DemoSceneManager::GetActiveScene() const
{
    return *m_activeScene;
}

SphereGridScene* DemoSceneManager::GetActiveSphereGridScene()
{
    return dynamic_cast<SphereGridScene*>(
        m_activeScene.get());
}

std::span<const DemoSceneEntry>DemoSceneManager::GetAvailableScenes() const
{
    return SceneRegistry::Instance().GetScenes();
}

std::string_view DemoSceneManager::GetActiveSceneName() const
{
    if(!m_activeSceneEntry)
    {
        return {};
    }

    return m_activeSceneEntry->name;
}