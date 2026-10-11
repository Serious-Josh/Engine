#include "CornellBoxScene.h"

#include "Renderer/ResourceUploader.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/RenderObject.h"
#include "Renderer/Vertex.h"
#include "Renderer/ImageLoader.h"
#include "Renderer/TangentGenerator.h"
#include "Renderer/Geometry/SphereGenerator.h"
#include "Renderer/DDSLoader.h"

#include "Scene/Camera.h"
#include "Scene/DirectionalLight.h"

#include "assets/ModelImporter.h"

#include "DemoContent/SceneRegistry.h"

#include <cstdint>
#include <stdexcept>

REGISTER_DEMO_SCENE("Cornell Box", CornellBoxScene);

bool CornellBoxScene::Initialize(
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

    //
    // Geometry pipeline
    //

    PipelineDescription pipelineDescription{};
    pipelineDescription.vertexShader = vertexShader;
    pipelineDescription.pixelShader = pixelShader;
    pipelineDescription.vertexLayout = &m_vertexLayout;

    pipelineDescription.renderTargets.count = 4;
    pipelineDescription.renderTargets.formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipelineDescription.renderTargets.formats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pipelineDescription.renderTargets.formats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pipelineDescription.renderTargets.formats[3] = DXGI_FORMAT_R16G16B16A16_FLOAT;

    pipelineDescription.rootParameters.push_back(
        {
            RootParameterType::Constants,
            0,
            9,
            D3D12_SHADER_VISIBILITY_PIXEL
        });

    pipelineDescription.rootParameters.push_back(
        {
            RootParameterType::ConstantBuffer,
            1,
            0,
            D3D12_SHADER_VISIBILITY_VERTEX
        });

    pipelineDescription.rootParameters.push_back(
        {
            RootParameterType::ConstantBuffer,
            2,
            0,
            D3D12_SHADER_VISIBILITY_VERTEX
        });

    pipelineDescription.depthEnabled = true;
    pipelineDescription.depthFormat = DXGI_FORMAT_D32_FLOAT;

    if (!m_pipeline.Initialize(
        device,
        pipelineDescription))
    {
        return false;
    }


    // White fallback texture

    const std::uint8_t whitePixel[4] =
    {
        255,
        255,
        255,
        255
    };

    if (!m_whiteTexture.Initialize(
        device,
        whitePixel,
        1,
        1,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        4))
    {
        return false;
    }

    if (!uploader.UploadTexture(
        m_whiteTexture.GetResource(),
        m_whiteTexture.GetInitialData(),
        m_whiteTexture.GetInitialDataSize()))
    {
        return false;
    }

    if (!m_whiteTexture.CreateSRV(
        device,
        descriptorHeap,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
    {
        return false;
    }


    // Temporary default material

    m_defaultMaterial.pipeline = &m_pipeline;
    m_defaultMaterial.texture = &m_whiteTexture;

    m_defaultMaterial.model = MaterialModel::Dielectric;

    m_defaultMaterial.roughness = 0.7f;
    m_defaultMaterial.ior = 1.5f;

    m_defaultMaterial.baseColor[0] = 0.7f;
    m_defaultMaterial.baseColor[1] = 0.7f;
    m_defaultMaterial.baseColor[2] = 0.7f;

    m_redWallMaterial = m_defaultMaterial;
    m_redWallMaterial.baseColor[0] = 0.75f;
    m_redWallMaterial.baseColor[1] = 0.08f;
    m_redWallMaterial.baseColor[2] = 0.06f;

    m_greenWallMaterial = m_defaultMaterial;
    m_greenWallMaterial.baseColor[0] = 0.08f;
    m_greenWallMaterial.baseColor[1] = 0.65f;
    m_greenWallMaterial.baseColor[2] = 0.10f;


    // Cornell box

    const auto cornellBox = ModelImporter::Load("assets/Models/CornellBox/cornell_box.fbx");

    if (cornellBox.empty())
    {
        throw std::runtime_error("Cornell box import produced no meshes.");
    }

    m_cornellMeshes.reserve(cornellBox.size());

    for (const ImportedMesh& importedMesh : cornellBox)
    {
        auto mesh = std::make_unique<Mesh>();

        if (!mesh->Initialize(
            device,
            uploader,
            descriptorHeap,
            importedMesh.vertices.data(),
            importedMesh.vertices.size(),
            m_vertexLayout,
            importedMesh.indices.data(),
            importedMesh.indices.size()))
        {
            throw std::runtime_error(
                "Failed to create imported Cornell box mesh.");
        }

        m_cornellMeshes.push_back(std::move(mesh));
    }


    // Tank

    ImageData tankAlbedo;
    if (!ImageLoader::Load("assets/Textures/Materials/tank_sample_alb.png", tankAlbedo))
    {
        throw std::runtime_error("Failed to load tank albedo atlas.");
    }

    if (!m_tankAlbedoTexture.Initialize(
        device, tankAlbedo.pixels.data(), tankAlbedo.width, tankAlbedo.height,
        DXGI_FORMAT_R8G8B8A8_UNORM, 4))
    {
        return false;
    }

    if (!uploader.UploadTexture(
        m_tankAlbedoTexture.GetResource(), m_tankAlbedoTexture.GetInitialData(),
        m_tankAlbedoTexture.GetInitialDataSize()))
    {
        return false;
    }

    if (!m_tankAlbedoTexture.CreateSRV(device, descriptorHeap, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
    {
        return false;
    }

    // Tank tangent-space normal map.
    // Store and sample as linear data, never sRGB.
    ImageData tankNormal;

    if (!ImageLoader::Load("assets/Textures/Materials/tank_sample_n.png", tankNormal))
    {
        throw std::runtime_error("Failed to load tank normal map.");
    }

    if (!m_tankNormalTexture.Initialize(
        device,
        tankNormal.pixels.data(),
        tankNormal.width,
        tankNormal.height,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        4))
    {
        return false;
    }

    if (!uploader.UploadTexture(
        m_tankNormalTexture.GetResource(),
        m_tankNormalTexture.GetInitialData(),
        m_tankNormalTexture.GetInitialDataSize()))
    {
        return false;
    }

    if (!m_tankNormalTexture.CreateSRV(
        device,
        descriptorHeap,
        DXGI_FORMAT_R8G8B8A8_UNORM))
    {
        return false;
    }

    m_tankMaterial = m_defaultMaterial;
    m_tankMaterial.texture = &m_tankAlbedoTexture;
    m_tankMaterial.normalTexture = &m_tankNormalTexture;
    m_tankMaterial.baseColor[0] = 1.0f;
    m_tankMaterial.baseColor[1] = 1.0f;
    m_tankMaterial.baseColor[2] = 1.0f;

    const auto tank =
        ModelImporter::Load(
            "assets/Models/CornellBox/tank_sample.fbx");

    if (tank.empty())
    {
        throw std::runtime_error(
            "Tank import produced no meshes.");
    }

    m_tankMeshes.reserve(tank.size());

    for (const ImportedMesh& importedMesh : tank)
    {
        auto mesh = std::make_unique<Mesh>();

        // The FBX atlas UVs use bottom-origin V; ImageLoader preserves PNG row order.
        // Adjust the shared vertex data so rasterization and DXR sample the same atlas.
        std::vector<Vertex> tankVertices = importedMesh.vertices;
        std::vector<std::uint32_t> tankIndices = importedMesh.indices;

        for (Vertex& vertex : tankVertices)
        {
            vertex.uv[1] = 1.0f - vertex.uv[1];
        }

        TangentGenerator::GenerateTangents(tankVertices, tankIndices);

        if (!mesh->Initialize(
            device,
            uploader,
            descriptorHeap,
            tankVertices.data(),
            tankVertices.size(),
            m_vertexLayout,
            tankIndices.data(),
            tankIndices.size()))
        {
            throw std::runtime_error("Failed to create imported tank mesh.");
        }

        m_tankMeshes.push_back(std::move(mesh));
    }


    // Brick sphere: match Mirror Test geometry and material textures.
    SphereGeometry sphere = SphereGenerator::Generate(1.0f, 32, 64);
    TangentGenerator::GenerateTangents(sphere.vertices, sphere.indices);

    if (!m_brickSphereMesh.Initialize(
        device, uploader, descriptorHeap,
        sphere.vertices.data(), sphere.vertices.size(), m_vertexLayout,
        sphere.indices.data(), sphere.indices.size()))
    {
        return false;
    }

    if (!DDSLoader::LoadTexture(
        device, uploader, descriptorHeap,
        "assets/Textures/Materials/stacked_brick_wall_diff_4k.dds",
        m_brickDiffuseTexture, true))
    {
        return false;
    }

    if (!DDSLoader::LoadTexture(
        device, uploader, descriptorHeap,
        "assets/Textures/Materials/stacked_brick_wall_nor_dx_4k.dds",
        m_brickNormalTexture, false))
    {
        return false;
    }

    if (!DDSLoader::LoadTexture(
        device, uploader, descriptorHeap,
        "assets/Textures/Materials/stacked_brick_wall_rough_4k.dds",
        m_brickRoughnessTexture, false))
    {
        return false;
    }

    if (!DDSLoader::LoadTexture(
        device, uploader, descriptorHeap,
        "assets/Textures/Materials/stacked_brick_wall_arm_4k.dds",
        m_brickARMTexture, false))
    {
        return false;
    }

    m_brickMaterial.pipeline = &m_pipeline;
    m_brickMaterial.texture = &m_brickDiffuseTexture;
    m_brickMaterial.normalTexture = &m_brickNormalTexture;
    m_brickMaterial.roughnessTexture = &m_brickRoughnessTexture;
    m_brickMaterial.armTexture = &m_brickARMTexture;
    m_brickMaterial.preferARM = true;
    m_brickMaterial.model = MaterialModel::MetallicRoughness;
    m_brickMaterial.roughness = 1.0f;
    m_brickMaterial.metallic = 0.0f;
    m_brickMaterial.baseColor[0] = 1.0f;
    m_brickMaterial.baseColor[1] = 0.02f;
    m_brickMaterial.baseColor[2] = 0.02f;

    // Cornell box render objects

    for (size_t i = 0; i < m_cornellMeshes.size(); ++i)
    {
        RenderObject boxPart{};
        boxPart.mesh = m_cornellMeshes[i].get();

        // Assign Cornell box wall materials.
        if (i == 1)
        {
            // Object 1: left wall
            boxPart.material = &m_redWallMaterial;
        }
        else if (i == 2)
        {
            // Object 2: right wall
            boxPart.material = &m_greenWallMaterial;
        }
        else
        {
            // Floor, ceiling, and remaining walls
            boxPart.material = &m_defaultMaterial;
        }

        boxPart.transform.position =
            Vector3{
                0.0f,
                0.0f,
                0.0f
            };

        boxPart.transform.scale =
            Vector3{
                1.0f,
                1.0f,
                1.0f
            };

        m_scene.AddRenderObject(boxPart);
    }


    // Tank render objects

    for (const auto& mesh : m_tankMeshes)
    {
        RenderObject tankPart{};
        tankPart.mesh = mesh.get();
        tankPart.material = &m_tankMaterial;

        tankPart.transform.position =
            Vector3{
                0.0f,
                0.0f,
                0.0f
            };

        tankPart.transform.rotation =
            Vector3{
                0.0f,
                ToRadians(45.0f),
                0.0f
            };

        tankPart.transform.scale =
            Vector3{
                0.5f,
                0.5f,
                0.5f
            };

        m_scene.AddRenderObject(tankPart);
    }

    RenderObject brickSphere{};
    brickSphere.mesh = &m_brickSphereMesh;
    brickSphere.material = &m_brickMaterial;
    // Raised above the tank and reduced to clear the imported ceiling geometry.
    brickSphere.transform.position = Vector3{0.0f, 2.75f, 0.0f};
    brickSphere.transform.scale = Vector3{0.5f, 0.5f, 0.5f};
    m_scene.AddRenderObject(brickSphere);

    ResetCamera();
    ResetDirectionalLight();

    return true;
}


void CornellBoxScene::Shutdown()
{
    m_scene.ClearRenderObjects();

    for (const auto& mesh : m_tankMeshes)
    {
        mesh->Shutdown();
    }

    m_tankMeshes.clear();

    for (const auto& mesh : m_cornellMeshes)
    {
        mesh->Shutdown();
    }

    m_cornellMeshes.clear();

    m_whiteTexture.Shutdown();
    m_tankAlbedoTexture.Shutdown();
    m_tankNormalTexture.Shutdown();
    m_brickDiffuseTexture.Shutdown();
    m_brickNormalTexture.Shutdown();
    m_brickRoughnessTexture.Shutdown();
    m_brickARMTexture.Shutdown();
    m_brickSphereMesh.Shutdown();
    m_pipeline.Shutdown();
}


void CornellBoxScene::Update(float deltaTime)
{
    (void)deltaTime;
}


void CornellBoxScene::SetAspectRatio(float aspectRatio)
{
    m_scene.GetCamera().SetAspectRatio(aspectRatio);
}


Scene& CornellBoxScene::GetScene()
{
    return m_scene;
}


const Scene& CornellBoxScene::GetScene() const
{
    return m_scene;
}


std::vector<Material*> CornellBoxScene::GetMaterials()
{
    return {
        &m_defaultMaterial,
        &m_redWallMaterial,
        &m_greenWallMaterial,
        &m_tankMaterial,
        &m_brickMaterial
    };
}


void CornellBoxScene::ResetCamera()
{
    Camera& camera = m_scene.GetCamera();

    camera.SetPosition(
        Vector3{
            0.0f,
            1.0f,
            -7.0f
        });

    camera.SetTarget(
        Vector3{
            0.0f,
            0.75f,
            1.5f
        });
}


void CornellBoxScene::ResetDirectionalLight()
{
    DirectionalLight& light = m_scene.GetDirectionalLight();

    light.direction =
        Vector3{
            -0.3f,
            -1.0f,
            0.2f
        }.Normalized();

    light.intensity = 3.0f;
    light.ambientStrength = 0.0f;
    light.angularRadius = ToRadians(5.0f);
}
