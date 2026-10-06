#pragma once

#include "DemoContent/IDemoScene.h"

#include <memory>

class SphereGridScene;

enum class DemoSceneId
{
    SphereGrid,
    MirrorTest
};

class DemoSceneManager
{
public:
    bool Initialize(
        DemoSceneId sceneId,
        ID3D12Device* device,
        ResourceUploader& uploader,
        DescriptorHeap& descriptorHeap,
        const Shader* vertexShader,
        const Shader* pixelShader,
        float aspectRatio);

    void Shutdown();

    void RequestScene(DemoSceneId sceneId);
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

    DemoSceneId GetActiveSceneId() const;

private:
    std::unique_ptr<IDemoScene> CreateScene(DemoSceneId sceneId) const;

    std::unique_ptr<IDemoScene> m_activeScene;

    DemoSceneId m_activeSceneId = DemoSceneId::SphereGrid;
    DemoSceneId m_requestedSceneId = DemoSceneId::SphereGrid;

    float m_aspectRatio = 1.0f;
    bool m_hasPendingSceneChange = false;
};