#include "SphereGridScene.h"
#include "Renderer/ResourceUploader.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/RenderObject.h"
#include "Scene/Camera.h"
#include "Scene/DirectionalLight.h"
#include "Renderer/Vertex.h"
#include "Renderer/TangentGenerator.h"
#include "Renderer/Geometry/SphereGenerator.h"
#include "Core/Log.h"
#include "Renderer/DDSLoader.h"
#include "DemoContent/SceneRegistry.h"

#include <cmath>

REGISTER_DEMO_SCENE("Sphere Grid", SphereGridScene);

bool SphereGridScene::Initialize(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& descriptorHeap, const Shader* vertexShader, const Shader* pixelShader)
{
    m_vertexLayout = CreateVertexLayout();

    if(!vertexShader || !pixelShader){
        return false;
    }

    PipelineDescription pipelineDescription{};
    pipelineDescription.vertexShader = vertexShader;
    pipelineDescription.pixelShader = pixelShader;
    pipelineDescription.vertexLayout = &m_vertexLayout;
    pipelineDescription.renderTargets.count = 4;
    pipelineDescription.renderTargets.formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipelineDescription.renderTargets.formats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pipelineDescription.renderTargets.formats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pipelineDescription.renderTargets.formats[3] = DXGI_FORMAT_R16G16B16A16_FLOAT;

    pipelineDescription.rootParameters.push_back({RootParameterType::Constants, 0, 9, D3D12_SHADER_VISIBILITY_PIXEL});

    pipelineDescription.rootParameters.push_back({RootParameterType::ConstantBuffer, 1, 0, D3D12_SHADER_VISIBILITY_VERTEX});
    pipelineDescription.rootParameters.push_back({RootParameterType::ConstantBuffer, 2, 0, D3D12_SHADER_VISIBILITY_VERTEX});

    pipelineDescription.depthEnabled = true;
    pipelineDescription.depthFormat = DXGI_FORMAT_D32_FLOAT;

    if(!m_pipeline.Initialize(device, pipelineDescription))
    {
        return false;
    }

    SphereGeometry sphere = SphereGenerator::Generate(1.0f, 32, 64);
    TangentGenerator::GenerateTangents(sphere.vertices, sphere.indices);

    if(!m_mesh.Initialize(device, uploader, descriptorHeap, sphere.vertices.data(), sphere.vertices.size(), m_vertexLayout, sphere.indices.data(), sphere.indices.size())){
        return false;
    }

    std::vector<Vertex> planeVertices =
    {
        {
            {-1.0f, 0.0f, -1.0f},
            {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f}
        },
        {
            {-1.0f, 0.0f, 1.0f},
            {0.0f, 1.0f, 0.0f},
            {0.0f, 1.0f}
        },
        {
            {1.0f, 0.0f, 1.0f},
            {0.0f, 1.0f, 0.0f},
            {1.0f, 1.0f}
        },
        {
            {1.0f, 0.0f, -1.0f},
            {0.0f, 1.0f, 0.0f},
            {1.0f, 0.0f}
        }
    };

    std::vector<std::uint32_t> planeIndices =
    {
        0, 1, 2,
        0, 2, 3
    };

    TangentGenerator::GenerateTangents(planeVertices, planeIndices);

    if(!m_planeMesh.Initialize(
        device,
        uploader, descriptorHeap,
        planeVertices.data(),
        planeVertices.size(),
        m_vertexLayout,
        planeIndices.data(),
        planeIndices.size()))
    {
        return false;
    }

    // Brick diffuse texture: BC1 with sRGB sampling.
    if(!DDSLoader::LoadTexture(
        device,
        uploader,
        descriptorHeap,
        "assets/Textures/Materials/stacked_brick_wall_diff_4k.dds",
        m_texture,
        true))
    {
        return false;
    }

    // Brick normal map: BC5 with linear sampling and the existing DirectX convention.
    if(!DDSLoader::LoadTexture(
        device,
        uploader,
        descriptorHeap,
        "assets/Textures/Materials/stacked_brick_wall_nor_dx_4k.dds",
        m_normalTexture,
        false))
    {
        return false;
    }

    // Brick roughness map: BC4 with linear sampling.
    if(!DDSLoader::LoadTexture(
        device,
        uploader,
        descriptorHeap,
        "assets/Textures/Materials/stacked_brick_wall_rough_4k.dds",
        m_roughnessTexture,
        false))
    {
        return false;
    }

    constexpr float horizontalSpacing = 1.65f;
    constexpr float verticalSpacing = 1.65f;

    const float startX = -horizontalSpacing * float(MaterialSampleCount - 1) * 0.5f;

    for(unsigned int i = 0; i < MaterialSampleCount; ++i)
    {
        Material& material = m_dielectricMaterials[i];

        material.pipeline = &m_pipeline;
        material.texture = &m_texture;
        material.normalTexture = &m_normalTexture;
        material.model = MaterialModel::Dielectric;
        material.roughnessTexture = &m_roughnessTexture;
        material.roughness = 1.0f;
        material.ior = 1.536f;

        material.baseColor[0] = 0.5f;
        material.baseColor[1] = 0.5f;
        material.baseColor[2] = 0.5f;

        RenderObject object{};

        object.mesh = &m_mesh;
        object.material = &material;
        object.transform.position = Vector3{startX + horizontalSpacing * float(i), verticalSpacing * 0.5f, 0.0f};
        object.transform.scale = Vector3{0.65f, 0.65f, 0.65f};

        m_scene.AddRenderObject(object);
    }

    for(unsigned int i = 0; i < MaterialSampleCount; ++i)
    {
        Material& material = m_conductorMaterials[i];

        material.pipeline = &m_pipeline;
        material.texture = &m_texture;
        material.normalTexture = &m_normalTexture;
        material.model = MaterialModel::Conductor;
        material.roughnessTexture = &m_roughnessTexture;
        material.roughness = 1.0f;
        if(i == 0)
        {
            // Temporary A/B diagnostic: restore this sphere's pre-texture roughness; keep its normal map.
            material.roughnessTexture = nullptr;
            material.roughness = 0.05f;
        }
        else if(i == 1)
        {
            material.roughness = 0.15f;
        }
        else if(i == 2)
        {
            material.roughness = 0.35f;
        }
        else if(i == 3)
        {
            material.roughness = 0.65f;
        }

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

        object.mesh = &m_mesh;
        object.material = &material;
        object.transform.position = Vector3{startX + horizontalSpacing * float(i), -verticalSpacing * 0.5f, 0.0f};
        object.transform.scale = Vector3{0.65f, 0.65f, 0.65f};

        m_scene.AddRenderObject(object);
    }

    const float metallicValues[MaterialSampleCount] =
    {
        0.0f,
        0.0f,
        0.5f,
        1.0f,
        1.0f
    };

    for(unsigned int i = 0; i < MaterialSampleCount; ++i)
    {
        Material& material = m_metallicRoughnessMaterials[i];

        material.pipeline = &m_pipeline;
        material.texture = &m_texture;
        material.normalTexture = &m_normalTexture;
        material.model = MaterialModel::MetallicRoughness;
        material.roughnessTexture = &m_roughnessTexture;
        material.roughness = 1.0f;
        material.metallic = metallicValues[i];

        material.baseColor[0] = 0.80f;
        material.baseColor[1] = 0.20f;
        material.baseColor[2] = 0.05f;

        RenderObject object{};

        object.mesh = &m_mesh;
        object.material = &material;
        object.transform.position = Vector3{startX + horizontalSpacing * float(i), verticalSpacing * 1.5f, 0.0f};
        object.transform.scale = Vector3{0.65f, 0.65f, 0.65f};

        m_scene.AddRenderObject(object);
    }

    m_floorMaterial.pipeline = &m_pipeline;
    m_floorMaterial.texture = &m_texture;
    m_floorMaterial.normalTexture = &m_normalTexture;
    m_floorMaterial.model = MaterialModel::Dielectric;
    m_floorMaterial.roughness = 0.7f;
    m_floorMaterial.ior = 1.5f;
    m_floorMaterial.baseColor[0] = 0.35f;
    m_floorMaterial.baseColor[1] = 0.35f;
    m_floorMaterial.baseColor[2] = 0.35f;

    RenderObject floor{};

    floor.mesh = &m_planeMesh;
    floor.material = &m_floorMaterial;
    floor.transform.position = Vector3{0.0f, -1.5f, 0.0f};
    floor.transform.scale = Vector3{5.0f, 1.0f, 5.0f};

    m_scene.AddRenderObject(floor);

    ResetCamera();
    ResetDirectionalLight();

    return true;
}

void SphereGridScene::Update(float deltaTime)
{
    switch(m_updateMode){
        case DemoUpdateMode::None:
            break;
        case DemoUpdateMode::RotateLight:
            UpdateRotatingLight(deltaTime);
            break;
        case DemoUpdateMode::MoveSphere:
            UpdateMovingSphere(deltaTime);
            break;
        case DemoUpdateMode::OrbitCamera:
            UpdateOrbitingCamera(deltaTime);
            break;
    }
}

void SphereGridScene::UpdateRotatingLight(float deltaTime)
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

void SphereGridScene::UpdateMovingSphere(float deltaTime)
{
    m_lightAngle += deltaTime * m_lightRotationSpeed;

    const std::vector<RenderObject>& renderObjects = m_scene.GetRenderObjects();

    if(renderObjects.empty()){
        return;
    }

    Transform transform = renderObjects[0].transform;

    transform.position.y = 0.825f + std::sin(m_lightAngle) * 0.75f;

    m_scene.SetRenderObjectTransform(0, transform);
}

void SphereGridScene::UpdateOrbitingCamera(float deltaTime)
{
    m_cameraOrbitAngle += deltaTime * m_cameraOrbitSpeed;
    const float angle = m_cameraOrbitAngle;

    Camera& camera = m_scene.GetCamera();

    constexpr float orbitRadius = 7.0f;
    constexpr float cameraHeight = 0.825f;

    const float x = std::sin(angle) * orbitRadius;
    const float z = -std::cos(angle) * orbitRadius;

    camera.SetPosition(Vector3{x, cameraHeight, z});

    camera.SetTarget(Vector3{0.0f, cameraHeight, 0.0f});
}

void SphereGridScene::ResetCamera()
{
    Camera& camera = m_scene.GetCamera();

    camera.SetPosition(Vector3{0.0f, 0.825f, -7.0f});
    camera.SetTarget(Vector3{0.0f, 0.825f, 0.0f});
}

void SphereGridScene::ResetDirectionalLight()
{
    DirectionalLight& light = m_scene.GetDirectionalLight();

    light.direction = Vector3{0.0f, -1.0f, 0.0f};

    light.intensity = 3.0f;
    light.ambientStrength = 0.0f;
    light.angularRadius = ToRadians(5.0f);
}

void SphereGridScene::Shutdown()
{
    m_scene.ClearRenderObjects();

    Log::Info("DemoScene shutdown: texture...");
    m_texture.Shutdown();
    m_normalTexture.Shutdown();
    m_roughnessTexture.Shutdown();
    Log::Info("DemoScene shutdown: texture done.");

    Log::Info("DemoScene shutdown: mesh...");
    m_mesh.Shutdown();
    m_planeMesh.Shutdown();
    Log::Info("DemoScene shutdown: mesh done.");

    Log::Info("DemoScene shutdown: pipeline...");
    m_pipeline.Shutdown();
    Log::Info("DemoScene shutdown: pipeline done.");

    Log::Info("DemoScene shutdown: complete.");
}

void SphereGridScene::SetAspectRatio(float aspectRatio)
{
    m_scene.GetCamera().SetAspectRatio(aspectRatio);
}

Scene& SphereGridScene::GetScene()
{
    return m_scene;
}

const Scene& SphereGridScene::GetScene() const
{
    return m_scene;
}

std::vector<Material*> SphereGridScene::GetMaterials()
{
    std::vector<Material*> materials;

    materials.reserve(MaterialSampleCount * 3 + 1);

    for(Material& material : m_dielectricMaterials){
        materials.push_back(&material);
    }

    for(Material& material : m_conductorMaterials){
        materials.push_back(&material);
    }

    for(Material& material : m_metallicRoughnessMaterials){
        materials.push_back(&material);
    }

    materials.push_back(&m_floorMaterial);

    return materials;
}

void SphereGridScene::SetUpdateMode(DemoUpdateMode mode)
{
    m_updateMode = mode;
}

DemoUpdateMode SphereGridScene::GetUpdateMode() const
{
    return m_updateMode;
}

void SphereGridScene::SetLightRotationSpeed(float speed)
{
    m_lightRotationSpeed = speed;
}

float SphereGridScene::GetLightRotationSpeed() const
{
    return m_lightRotationSpeed;
}

void SphereGridScene::SetCameraOrbitSpeed(float speed)
{
    m_cameraOrbitSpeed = speed;
}

float SphereGridScene::GetCameraOrbitSpeed() const
{
    return m_cameraOrbitSpeed;
}

void SphereGridScene::SetLightVerticalAngle(float angle)
{
    m_lightVerticalAngle = angle;
}

float SphereGridScene::GetLightVerticalAngle() const
{
    return m_lightVerticalAngle;
}
