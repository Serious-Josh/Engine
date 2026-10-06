#include "DemoSceneManager.h"

#include "DemoContent/SphereGridScene.h"
#include "DemoContent/MirrorTestScene.h"
#include "Core/Log.h"

#include <utility>

bool DemoSceneManager::Initialize(
    DemoSceneId sceneId,
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    const Shader* vertexShader,
    const Shader* pixelShader,
    float aspectRatio)
{
    m_aspectRatio = aspectRatio;
    m_activeSceneId = sceneId;
    m_requestedSceneId = sceneId;
    m_hasPendingSceneChange = false;

    m_activeScene = CreateScene(sceneId);

    if(!m_activeScene)
    {
        Log::Error("Failed to create demo scene.");
        return false;
    }

    if(!m_activeScene->Initialize(device, uploader, descriptorHeap, vertexShader, pixelShader))
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

    m_hasPendingSceneChange = false;
}

void DemoSceneManager::RequestScene(DemoSceneId sceneId)
{
    if(sceneId == m_activeSceneId)
    {
        m_hasPendingSceneChange = false;
        m_requestedSceneId = m_activeSceneId;
        return;
    }

    m_requestedSceneId = sceneId;
    m_hasPendingSceneChange = true;
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

    if(m_activeScene)
    {
        m_activeScene->Shutdown();
        m_activeScene.reset();
    }

    std::unique_ptr<IDemoScene> newScene = CreateScene(m_requestedSceneId);

    if(!newScene)
    {
        Log::Error("Failed to create requested demo scene.");
        return false;
    }

    if(!newScene->Initialize(device, uploader, descriptorHeap, vertexShader, pixelShader))
    {
        Log::Error("Failed to initialize requested demo scene.");
        newScene->Shutdown();
        return false;
    }

    newScene->SetAspectRatio(m_aspectRatio);

    m_activeScene = std::move(newScene);
    m_activeSceneId = m_requestedSceneId;
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
    return dynamic_cast<SphereGridScene*>(m_activeScene.get());
}

DemoSceneId DemoSceneManager::GetActiveSceneId() const
{
    return m_activeSceneId;
}

std::unique_ptr<IDemoScene> DemoSceneManager::CreateScene(DemoSceneId sceneId) const
{
    switch(sceneId)
    {
        case DemoSceneId::SphereGrid:
            return std::make_unique<SphereGridScene>();

        case DemoSceneId::MirrorTest:
            return std::make_unique<MirrorTestScene>();
    }

    return nullptr;
}