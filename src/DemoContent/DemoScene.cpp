#include "DemoScene.h"
#include "Renderer/Vertex.h"
#include "Renderer/Geometry/SphereGenerator.h"
#include "Core/Log.h"

#include <cmath>

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

    pipelineDescription.rootParameters.push_back({RootParameterType::Constants, 0, 3, D3D12_SHADER_VISIBILITY_PIXEL});

    pipelineDescription.rootParameters.push_back({RootParameterType::ConstantBuffer, 1, 0, D3D12_SHADER_VISIBILITY_VERTEX});
    pipelineDescription.rootParameters.push_back({RootParameterType::ConstantBuffer, 2, 0, D3D12_SHADER_VISIBILITY_VERTEX});

    pipelineDescription.depthEnabled = true;
    pipelineDescription.depthFormat = DXGI_FORMAT_D32_FLOAT;

    if(!m_pipeline.Initialize(device, pipelineDescription))
    {
        return false;
    }

    SphereGeometry sphere = SphereGenerator::Generate(1.0f, 32, 64);

    if(!m_mesh.Initialize(device, uploader, sphere.vertices.data(), sphere.vertices.size(), m_vertexLayout, sphere.indices.data(), sphere.indices.size())){
        return false;
    }

    const unsigned int texturePixels[] =
    {
        0xFFFF0000,
        0xFF00FF00,
        0xFF0000FF,
        0xFFFFFFFF
    };

    if(!m_texture.Initialize(device, texturePixels, 2, 2, DXGI_FORMAT_B8G8R8A8_UNORM, 4)){
        return false;
    }

    if(!uploader.UploadTexture(
        m_texture.GetResource(),
        m_texture.GetInitialData(),
        m_texture.GetInitialDataSize()))
    {
        return false;
    }

    if(!m_texture.CreateSRV(device, descriptorHeap)){
        return false;
    }

    m_leftMaterial.pipeline = &m_pipeline;
    m_leftMaterial.texture = &m_texture;
    m_leftMaterial.model = MaterialModel::Dielectric;
    m_leftMaterial.roughness = 0.1f;
    m_leftMaterial.ior = 1.536f;
    m_leftMaterial.baseColor[0] = 0.5f;
    m_leftMaterial.baseColor[1] = 0.5f;
    m_leftMaterial.baseColor[2] = 0.5f;

    m_rightMaterial.pipeline = &m_pipeline;
    m_rightMaterial.texture = &m_texture;
    m_rightMaterial.model = MaterialModel::Conductor;
    m_rightMaterial.roughness = 0.85f;
    
    m_rightMaterial.conductorN[0] = 0.2f;
    m_rightMaterial.conductorN[1] = 0.9f;
    m_rightMaterial.conductorN[2] = 1.1f;

    m_rightMaterial.conductorK[0] = 3.9f;
    m_rightMaterial.conductorK[1] = 2.5f;
    m_rightMaterial.conductorK[2] = 2.1f;

    m_rightMaterial.baseColor[0] = 0.5f;
    m_rightMaterial.baseColor[1] = 0.5f;
    m_rightMaterial.baseColor[2] = 0.5f;

    RenderObject firstObject{};
    firstObject.mesh = &m_mesh;
    firstObject.material = &m_leftMaterial;
    firstObject.transform.position = Vector3{-1.25f, 0.0f, 0.0f};

    m_scene.AddRenderObject(firstObject);

    RenderObject secondObject{};
    secondObject.mesh = &m_mesh;
    secondObject.material = &m_rightMaterial;
    secondObject.transform.position = Vector3{1.25f, 0.0f, 0.0f};

    m_scene.AddRenderObject(secondObject);

    //m_scene.GetDirectionalLight().color = {1.0f, 0.0f, 0.0f};
    DirectionalLight& light = m_scene.GetDirectionalLight();

    light.intensity = 3.0f;
    light.ambientStrength = 0.0f;

    return true;
}

void DemoScene::Update(float deltaTime)
{
    m_lightAngle += deltaTime * m_lightRotationSpeed;

    DirectionalLight& light = m_scene.GetDirectionalLight();

    const float horizontalScale = std::cos(m_lightVerticalAngle);

    const float x = std::sin(m_lightAngle) * horizontalScale;
    const float y = -std::sin(m_lightVerticalAngle);
    const float z = std::cos(m_lightAngle) * horizontalScale;

    const float length = std::sqrt(x * x + y * y + z * z);

    light.direction.x = x / length;
    light.direction.y = y / length;
    light.direction.z = z / length;
}

void DemoScene::Shutdown()
{
    m_scene.GetRenderObjects().clear();

    Log::Info("DemoScene shutdown: texture...");
    m_texture.Shutdown();
    Log::Info("DemoScene shutdown: texture done.");

    Log::Info("DemoScene shutdown: mesh...");
    m_mesh.Shutdown();
    Log::Info("DemoScene shutdown: mesh done.");

    Log::Info("DemoScene shutdown: pipeline...");
    m_pipeline.Shutdown();
    Log::Info("DemoScene shutdown: pipeline done.");

    Log::Info("DemoScene shutdown: complete.");
    m_leftMaterial.pipeline = nullptr;
    m_leftMaterial.texture = nullptr;

    m_rightMaterial.pipeline = nullptr;
    m_rightMaterial.texture = nullptr;
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

std::vector<Material*> DemoScene::GetMaterials()
{
    return {
        &m_leftMaterial,
        &m_rightMaterial
    };
}