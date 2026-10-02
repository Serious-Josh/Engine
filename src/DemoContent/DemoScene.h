#pragma once

#include "Renderer/Mesh.h"
#include "Renderer/Texture.h"
#include "Renderer/VertexLayout.h"
#include "Renderer/RenderObject.h"
#include "Renderer/ResourceUploader.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/Material.h"
#include "Renderer/Pipeline.h"
#include "Scene/Camera.h"
#include "Scene/DirectionalLight.h"
#include "Scene/Scene.h"

#include <vector>

class Shader;

class DemoScene
{
    public:
        bool Initialize(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& descriptorHeap, const Shader* vertexShader, const Shader* pixelShader);
        void Shutdown();

        void Update(float deltaTime);

        void SetAspectRatio(float aspectRatio);

        Scene& GetScene();
        const Scene& GetScene() const;
        std::vector<Material*> GetMaterials();

    private:
        VertexLayout m_vertexLayout;
        Mesh m_mesh;
        Texture m_texture;
        Pipeline m_pipeline;

        Material m_leftMaterial;
        Material m_rightMaterial;

        Scene m_scene;

        float m_lightAngle = 0.0f;
        float m_lightRotationSpeed = 0.5f;
        float m_lightVerticalAngle = 0.261799f; // 15 degrees
};