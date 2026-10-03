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

    const float roughnessValues[MaterialSampleCount] =
    {
        0.05f,
        0.25f,
        0.50f,
        0.75f,
        1.00f
    };

    constexpr float horizontalSpacing = 1.65f;
    constexpr float verticalSpacing = 1.65f;

    const float startX =
        -horizontalSpacing *
        float(MaterialSampleCount - 1) *
        0.5f;

    for(unsigned int i = 0; i < MaterialSampleCount; ++i)
    {
        Material& material =
            m_dielectricMaterials[i];

        material.pipeline = &m_pipeline;
        material.texture = &m_texture;

        material.model =
            MaterialModel::Dielectric;

        material.roughness =
            roughnessValues[i];

        material.ior =
            1.536f;

        material.baseColor[0] = 0.5f;
        material.baseColor[1] = 0.5f;
        material.baseColor[2] = 0.5f;

        RenderObject object{};

        object.mesh =
            &m_mesh;

        object.material =
            &material;

        object.transform.position =
            Vector3{
                startX +
                horizontalSpacing * float(i),
                verticalSpacing * 0.5f,
                0.0f
            };

        object.transform.scale =
            Vector3{
                0.65f,
                0.65f,
                0.65f
            };

        m_scene.AddRenderObject(
            object
        );
    }

    for(unsigned int i = 0; i < MaterialSampleCount; ++i)
    {
        Material& material =
            m_conductorMaterials[i];

        material.pipeline = &m_pipeline;
        material.texture = &m_texture;

        material.model =
            MaterialModel::Conductor;

        material.roughness =
            roughnessValues[i];

        material.conductorN[0] = 0.2f;
        material.conductorN[1] = 0.9f;
        material.conductorN[2] = 1.1f;

        material.conductorK[0] = 3.9f;
        material.conductorK[1] = 2.5f;
        material.conductorK[2] = 2.1f;

        material.baseColor[0] = 0.5f;
        material.baseColor[1] = 0.5f;
        material.baseColor[2] = 0.5f;

        RenderObject object{};

        object.mesh =
            &m_mesh;

        object.material =
            &material;

        object.transform.position =
            Vector3{
                startX +
                horizontalSpacing * float(i),
                -verticalSpacing * 0.5f,
                0.0f
            };

        object.transform.scale =
            Vector3{
                0.65f,
                0.65f,
                0.65f
            };

        m_scene.AddRenderObject(
            object
        );
    }

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
    std::vector<Material*> materials;

    materials.reserve(
        MaterialSampleCount * 2
    );

    for(Material& material : m_dielectricMaterials){
        materials.push_back(&material);
    }

    for(Material& material : m_conductorMaterials){
        materials.push_back(&material);
    }

    return materials;
}