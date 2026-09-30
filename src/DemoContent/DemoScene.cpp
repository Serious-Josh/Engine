#include "DemoScene.h"
#include "Renderer/Vertex.h"
#include "Renderer/Geometry/SphereGenerator.h"

bool DemoScene::Initialize(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& descriptorHeap, const Shader* vertexShader, const Shader* pixelShader)
{
    m_vertexLayout = CreateVertexLayout();

    if(!vertexShader || !pixelShader){
        return false;
    }

    PipelineDescription pipelineDescription{};
    pipelineDescription.vertexShader = vertexShader;
    pipelineDescription.pixelShader = pixelShader;
    pipelineDescription.vertexLayout = &m_vertexLayout;
    pipelineDescription.renderTargets.count = 2;
    pipelineDescription.renderTargets.formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipelineDescription.renderTargets.formats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;

    pipelineDescription.rootParameters.push_back({RootParameterType::Constants, 0, 1, D3D12_SHADER_VISIBILITY_PIXEL});

    pipelineDescription.rootParameters.push_back({RootParameterType::ConstantBuffer, 1, 0, D3D12_SHADER_VISIBILITY_VERTEX});
    pipelineDescription.rootParameters.push_back({RootParameterType::ConstantBuffer, 2, 0, D3D12_SHADER_VISIBILITY_VERTEX});

    pipelineDescription.depthEnabled = true;
    pipelineDescription.depthFormat = DXGI_FORMAT_D32_FLOAT;

    if(!m_pipeline.Initialize(device, pipelineDescription))
    {
        return false;
    }

    SphereGeometry sphere = SphereGenerator::Generate(1.0f, 32, 64);

    if(!m_mesh.Initialize(device, sphere.vertices.data(), sphere.vertices.size(), m_vertexLayout, sphere.indices.data(), sphere.indices.size())){
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

    RenderObject firstObject{};
    firstObject.mesh = &m_mesh;
    firstObject.material = &m_material;
    firstObject.transform.position = Vector3{-1.25f, 0.0f, 0.0f};

    m_scene.AddRenderObject(firstObject);

    RenderObject secondObject{};
    secondObject.mesh = &m_mesh;
    secondObject.material = &m_material;
    secondObject.transform.position = Vector3{1.25f, 0.0f, 0.0f};

    m_scene.AddRenderObject(secondObject);

    //m_scene.GetDirectionalLight().color = {1.0f, 0.0f, 0.0f};

    return true;
}

void DemoScene::Shutdown()
{
    m_scene.GetRenderObjects().clear();

    m_mesh.Shutdown();
    m_texture.Shutdown();
    m_pipeline.Shutdown();

    m_material.pipeline = nullptr;
    m_material.texture = nullptr;
}

void DemoScene::SetAspectRatio(float aspectRatio)
{
    m_scene.GetCamera().SetAspectRatio(aspectRatio);
}

Scene& DemoScene::GetScene()
{
    return m_scene;
}

const Scene& DemoScene::GetScene() const
{
    return m_scene;
}