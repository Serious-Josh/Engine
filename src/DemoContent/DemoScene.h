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

class DemoScene
{
    public:
        bool Initialize(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& descriptorHeap);
        void Shutdown();

        void SetAspectRatio(float aspectRatio);

        const RenderObject& GetRenderObject() const;
        const Camera& GetCamera() const;

    private:
        VertexLayout m_vertexLayout;
        Mesh m_mesh;
        Texture m_texture;
        Pipeline m_pipeline;

        Material m_material;
        RenderObject m_renderObject;

        Camera m_camera;
};