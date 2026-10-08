#pragma once

#include "Math/Math.h"

#include "Renderer/Mesh.h"
#include "Renderer/Texture.h"
#include "Renderer/VertexLayout.h"
#include "Renderer/Material.h"
#include "Renderer/Pipeline.h"

#include "Scene/Scene.h"

#include "DemoContent/IDemoScene.h"

#include <vector>

class Shader;
class ResourceUploader;
class DescriptorHeap;

class MirrorTestScene : public IDemoScene
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

    Mesh m_sphereMesh;
    Mesh m_planeMesh;

    Texture m_sphereTexture;
    Texture m_whiteTexture;

    Pipeline m_pipeline;

    Material m_mirrorAMaterial;
    Material m_mirrorBMaterial;
    Material m_coloredMaterial;
    Material m_floorMaterial;

    Scene m_scene;
};