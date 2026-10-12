#include "DemoContent/StressTestScene.h"
#include "DemoContent/SceneRegistry.h"

#include "Renderer/ResourceUploader.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/RenderObject.h"
#include "Renderer/Vertex.h"
#include "Renderer/TangentGenerator.h"
#include "Renderer/Geometry/SphereGenerator.h"
#include "Renderer/DDSLoader.h"

#include "Scene/Camera.h"
#include "Scene/DirectionalLight.h"

#include "Core/Log.h"

REGISTER_DEMO_SCENE("Stress Test", StressTestScene);

bool StressTestScene::Initialize(
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    const Shader* vertexShader,
    const Shader* pixelShader)
{
    m_vertexLayout = CreateVertexLayout();

    if (!vertexShader || !pixelShader)
    {
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

    pipelineDescription.rootParameters.push_back(
        {RootParameterType::ConstantBuffer, 1, 0, D3D12_SHADER_VISIBILITY_VERTEX});

    pipelineDescription.rootParameters.push_back(
        {RootParameterType::ConstantBuffer, 2, 0, D3D12_SHADER_VISIBILITY_VERTEX});

    pipelineDescription.depthEnabled = true;
    pipelineDescription.depthFormat = DXGI_FORMAT_D32_FLOAT;

    if (!m_pipeline.Initialize(device, pipelineDescription))
    {
        return false;
    }

    SphereGeometry sphere = SphereGenerator::Generate(1.0f, 32, 64);

    TangentGenerator::GenerateTangents(sphere.vertices, sphere.indices);

    if (!m_sphereMesh.Initialize(device, uploader, descriptorHeap, sphere.vertices.data(), sphere.vertices.size(),
                                 m_vertexLayout, sphere.indices.data(), sphere.indices.size()))
    {
        return false;
    }

    std::vector<Vertex> planeVertices = {{{-1.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
                                         {{-1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},
                                         {{1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
                                         {{1.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}}};

    std::vector<std::uint32_t> planeIndices = {0, 1, 2, 0, 2, 3};

    TangentGenerator::GenerateTangents(planeVertices, planeIndices);

    if (!m_planeMesh.Initialize(device, uploader, descriptorHeap, planeVertices.data(), planeVertices.size(),
                                m_vertexLayout, planeIndices.data(), planeIndices.size()))
    {
        return false;
    }

    // Brick diffuse texture: BC1 with sRGB sampling.

    if (!DDSLoader::LoadTexture(device, uploader, descriptorHeap,
                                "assets/Textures/Materials/stacked_brick_wall_diff_4k.dds", m_whiteTexture, true))
    {
        return false;
    }

    // Brick normal map: BC5 with linear sampling and the existing DirectX convention.

    if (!DDSLoader::LoadTexture(device, uploader, descriptorHeap,
                                "assets/Textures/Materials/stacked_brick_wall_nor_dx_4k.dds", m_normalTexture, false))
    {
        return false;
    }

    // Brick roughness map: BC4 with linear sampling, shared by sphere materials.

    if (!DDSLoader::LoadTexture(device, uploader, descriptorHeap,
                                "assets/Textures/Materials/stacked_brick_wall_rough_4k.dds", m_roughnessTexture, false))
    {
        return false;
    }

    m_roughMaterial.pipeline = &m_pipeline;
    m_roughMaterial.texture = &m_whiteTexture;
    m_roughMaterial.normalTexture = &m_normalTexture;

    m_roughMaterial.model = MaterialModel::Dielectric;
    m_roughMaterial.roughnessTexture = &m_roughnessTexture;
    m_roughMaterial.roughness = 1.0f;
    m_roughMaterial.ior = 1.5f;

    m_roughMaterial.baseColor[0] = 0.55f;
    m_roughMaterial.baseColor[1] = 0.55f;
    m_roughMaterial.baseColor[2] = 0.55f;

    m_glossyMaterial.pipeline = &m_pipeline;
    m_glossyMaterial.texture = &m_whiteTexture;
    m_glossyMaterial.normalTexture = &m_normalTexture;
    m_glossyMaterial.model = MaterialModel::Dielectric;

    m_glossyMaterial.roughnessTexture = &m_roughnessTexture;
    m_glossyMaterial.roughness = 1.0f;
    m_glossyMaterial.ior = 1.5f;
    m_glossyMaterial.baseColor[0] = 0.25f;

    m_glossyMaterial.baseColor[1] = 0.45f;
    m_glossyMaterial.baseColor[2] = 0.8f;

    m_metalMaterial.pipeline = &m_pipeline;
    m_metalMaterial.texture = &m_whiteTexture;
    m_metalMaterial.normalTexture = &m_normalTexture;
    m_metalMaterial.model = MaterialModel::MetallicRoughness;

    m_metalMaterial.roughnessTexture = &m_roughnessTexture;
    m_metalMaterial.roughness = 1.0f;
    m_metalMaterial.metallic = 1.0f;
    m_metalMaterial.baseColor[0] = 0.8f;

    m_metalMaterial.baseColor[1] = 0.25f;
    m_metalMaterial.baseColor[2] = 0.08f;

    m_floorMaterial.pipeline = &m_pipeline;
    m_floorMaterial.texture = &m_whiteTexture;
    m_floorMaterial.normalTexture = &m_normalTexture;
    m_floorMaterial.model = MaterialModel::Dielectric;

    m_floorMaterial.roughness = 0.65f;
    m_floorMaterial.ior = 1.5f;
    m_floorMaterial.baseColor[0] = 0.3f;
    m_floorMaterial.baseColor[1] = 0.3f;

    m_floorMaterial.baseColor[2] = 0.3f;

    RebuildGrid();

    ResetCamera();
    ResetDirectionalLight();

    return true;
}

void StressTestScene::SetGridSize(unsigned int gridSize)
{
    if (gridSize < 10 || gridSize > 50 || gridSize == m_gridSize)
    {
        return;
    }

    m_gridSize = gridSize;
    RebuildGrid();
}

unsigned int StressTestScene::GetGridSize() const
{
    return m_gridSize;
}

void StressTestScene::RebuildGrid()
{
    m_scene.ClearRenderObjects();

    const unsigned int gridWidth = m_gridSize;
    const unsigned int gridDepth = m_gridSize;

    constexpr float spacing = 2.2f;
    constexpr float sphereRadius = 0.75f;

    const float startX = -float(gridWidth - 1) * spacing * 0.5f;
    const float startZ = -float(gridDepth - 1) * spacing * 0.5f;

    for (unsigned int z = 0; z < gridDepth; ++z)
    {
        for (unsigned int x = 0; x < gridWidth; ++x)
        {
            RenderObject object{};

            object.mesh = &m_sphereMesh;

            const unsigned int materialIndex = (x + z) % 3;

            if (materialIndex == 0)
            {
                object.material = &m_roughMaterial;
            }
            else if (materialIndex == 1)
            {
                object.material = &m_glossyMaterial;
            }
            else
            {
                object.material = &m_metalMaterial;
            }

            object.transform.position = Vector3{startX + float(x) * spacing, sphereRadius, startZ + float(z) * spacing};

            const unsigned int scalePattern = (x * 3 + z * 5) % 4;

            if (scalePattern == 0)
            {
                object.transform.scale = Vector3{sphereRadius * 1.25f, sphereRadius * 0.70f, sphereRadius};
            }
            else if (scalePattern == 1)
            {
                object.transform.scale = Vector3{sphereRadius, sphereRadius * 1.35f, sphereRadius * 0.75f};
            }
            else if (scalePattern == 2)
            {
                object.transform.scale = Vector3{sphereRadius * 0.80f, sphereRadius, sphereRadius * 1.30f};
            }
            else
            {
                object.transform.scale = Vector3{sphereRadius, sphereRadius, sphereRadius};
            }

            m_scene.AddRenderObject(object);
        }
    }

    RenderObject floor{};

    floor.mesh = &m_planeMesh;
    floor.material = &m_floorMaterial;
    floor.transform.position = Vector3{0.0f, 0.0f, 0.0f};
    floor.transform.scale = Vector3{float(gridWidth) * spacing * 0.55f, 1.0f, float(gridDepth) * spacing * 0.55f};

    m_scene.AddRenderObject(floor);
}

void StressTestScene::Shutdown()
{
    m_scene.ClearRenderObjects();

    m_whiteTexture.Shutdown();
    m_normalTexture.Shutdown();
    m_roughnessTexture.Shutdown();

    m_sphereMesh.Shutdown();
    m_planeMesh.Shutdown();

    m_pipeline.Shutdown();
}

void StressTestScene::Update(float deltaTime)
{
}

void StressTestScene::SetAspectRatio(float aspectRatio)
{
    m_scene.GetCamera().SetAspectRatio(aspectRatio);
}

Scene& StressTestScene::GetScene()
{
    return m_scene;
}

const Scene& StressTestScene::GetScene() const
{
    return m_scene;
}

std::vector<Material*> StressTestScene::GetMaterials()
{
    return {&m_roughMaterial, &m_glossyMaterial, &m_metalMaterial, &m_floorMaterial};
}

void StressTestScene::ResetCamera()
{
    Camera& camera = m_scene.GetCamera();

    camera.SetPosition(Vector3{0.0f, 18.0f, -32.0f});
    camera.SetTarget(Vector3{0.0f, 0.0f, 0.0f});
}

void StressTestScene::ResetDirectionalLight()
{
    DirectionalLight& light = m_scene.GetDirectionalLight();

    light.direction = Vector3{-0.4f, -1.0f, 0.3f}.Normalized();
    light.intensity = 3.0f;
    light.ambientStrength = 0.0f;
    light.angularRadius = ToRadians(5.0f);
}
