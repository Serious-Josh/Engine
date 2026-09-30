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

class Shader;

class DemoScene
{
    public:
        bool Initialize(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& descriptorHeap, const Shader* vertexShader, const Shader* pixelShader);
        void Shutdown();

        void SetAspectRatio(float aspectRatio);

        Scene& GetScene();
        const Scene& GetScene() const;

    private:
        VertexLayout m_vertexLayout;
        Mesh m_mesh;
        Texture m_texture;
        Pipeline m_pipeline;

        Material m_material;

        Scene m_scene;
};