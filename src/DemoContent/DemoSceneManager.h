#pragma once

#include "DemoContent/IDemoScene.h"
#include "DemoContent/SceneRegistry.h"

#include <memory>
#include <span>
#include <string_view>

class SphereGridScene;

class DemoSceneManager
{
public:
    bool Initialize(
        std::string_view sceneName,
        ID3D12Device* device,
        ResourceUploader& uploader,
        DescriptorHeap& descriptorHeap,
        const Shader* vertexShader,
        const Shader* pixelShader,
        float aspectRatio);

    void Shutdown();

    bool RequestScene(std::string_view sceneName);

    bool HasPendingSceneChange() const;

    bool ApplyPendingSceneChange(
        ID3D12Device* device,
        ResourceUploader& uploader,
        DescriptorHeap& descriptorHeap,
        const Shader* vertexShader,
        const Shader* pixelShader);

    void SetAspectRatio(float aspectRatio);

    IDemoScene& GetActiveScene();
    const IDemoScene& GetActiveScene() const;

    SphereGridScene* GetActiveSphereGridScene();

    std::span<const DemoSceneEntry> GetAvailableScenes() const;
    std::string_view GetActiveSceneName() const;

private:
    std::unique_ptr<IDemoScene> m_activeScene;

    const DemoSceneEntry* m_activeSceneEntry = nullptr;
    const DemoSceneEntry* m_requestedSceneEntry = nullptr;

    float m_aspectRatio = 1.0f;
    bool m_hasPendingSceneChange = false;
};