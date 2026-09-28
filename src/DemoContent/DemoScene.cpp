#include "DemoScene.h"
#include "Renderer/Triangle.h"

bool DemoScene::Initialize(
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap)
{
    m_vertexLayout = CreateTriangleVertexLayout();

    PipelineDescription pipelineDescription{};
    pipelineDescription.vertexShaderPath = ENGINE_SHADER_DIR "/TriangleVS.cso";
    pipelineDescription.pixelShaderPath = ENGINE_SHADER_DIR "/TrianglePS.cso";
    pipelineDescription.vertexLayout = &m_vertexLayout;
    pipelineDescription.renderTargets.count = 2;
    pipelineDescription.renderTargets.formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipelineDescription.renderTargets.formats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pipelineDescription.rootConstantCount = 1;

    if(!m_pipeline.Initialize(device, pipelineDescription))
    {
        return false;
    }

    if(!m_mesh.Initialize(device, TriangleVertices, 3, m_vertexLayout)){
        return false;
    }

    const unsigned int texturePixels[] =
    {
        0xFFFF0000,
        0xFF00FF00,
        0xFF0000FF,
        0xFFFFFFFF
    };

    if(!m_texture.Initialize(device, texturePixels, 2, 2)){
        return false;
    }

    if(!uploader.UploadTexture(
        m_texture.GetResource(),
        m_texture.GetInitialData(),
        m_texture.GetInitialDataSize(),
        2,
        2))
    {
        return false;
    }

    if(!m_texture.CreateSRV(device, descriptorHeap)){
        return false;
    }

    m_material.pipeline = &m_pipeline;
    m_material.texture = &m_texture;

    m_renderObject.mesh = &m_mesh;
    m_renderObject.material = &m_material;


    return true;
}

void DemoScene::Shutdown()
{
    m_mesh.Shutdown();
    m_texture.Shutdown();
    m_pipeline.Shutdown();

    m_material.pipeline = nullptr;
    m_material.texture = nullptr;

    m_renderObject.mesh = nullptr;
    m_renderObject.material = nullptr;
}

const RenderObject& DemoScene::GetRenderObject() const
{
    return m_renderObject;
}