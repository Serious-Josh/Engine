#pragma once

#include "Math/Math.h"
#include "Renderer/Mesh.h"
#include "Renderer/Texture.h"
#include "Renderer/VertexLayout.h"
#include "Renderer/Material.h"
#include "Renderer/Pipeline.h"
#include "Scene/Scene.h"
#include "DemoContent/IDemoScene.h"

#include <memory>
#include <vector>

class Shader;
class ResourceUploader;
class DescriptorHeap;

class CornellBoxScene : public IDemoScene
{
    public:
    bool Initialize(
      ID3D12Device* device,
      ResourceUploader& uploader,
      DescriptorHeap& descriptorHeap,
      const Shader* vertexShader,
      const Shader* pixelShader) override;

    void Shutdown() override;
    void Update(float deltaTime) override;
    void SetAspectRatio(float aspectRatio) override;

    Scene& GetScene() override;
    const Scene& GetScene() const override;

    std::vector<Material*> GetMaterials() override;

    void ResetCamera() override;
    void ResetDirectionalLight() override;

    private:
    VertexLayout m_vertexLayout;

    std::vector<std::unique_ptr<Mesh>> m_cornellMeshes;
    std::vector<std::unique_ptr<Mesh>> m_tankMeshes;

    Mesh m_brickSphereMesh;

    Texture m_whiteTexture;
    Texture m_tankAlbedoTexture;
    Texture m_tankNormalTexture;
    Texture m_brickDiffuseTexture;

    Texture m_brickNormalTexture;
    Texture m_brickRoughnessTexture;
    Texture m_brickARMTexture;

    Pipeline m_pipeline;

    Material m_defaultMaterial;
    Material m_redWallMaterial;
    Material m_greenWallMaterial;
    Material m_tankMaterial;

    Material m_brickMaterial;

    Scene m_scene;
};
