#include "CornellBoxScene.h"

#include "Renderer/ResourceUploader.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/RenderObject.h"
#include "Renderer/Vertex.h"

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

    pipelineDescription.renderTargets.count = 3;
    pipelineDescription.renderTargets.formats[0] =
        DXGI_FORMAT_R8G8B8A8_UNORM;
    pipelineDescription.renderTargets.formats[1] =
        DXGI_FORMAT_R16G16B16A16_FLOAT;
    pipelineDescription.renderTargets.formats[2] =
        DXGI_FORMAT_R16G16B16A16_FLOAT;

    pipelineDescription.rootParameters.push_back(
        {
            RootParameterType::Constants,
            0,
            8,
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

    m_defaultMaterial.model =
        MaterialModel::Dielectric;

    m_defaultMaterial.roughness = 0.7f;
    m_defaultMaterial.ior = 1.5f;

    m_defaultMaterial.baseColor[0] = 0.7f;
    m_defaultMaterial.baseColor[1] = 0.7f;
    m_defaultMaterial.baseColor[2] = 0.7f;


    // Cornell box

    const auto cornellBox =
        ModelImporter::Load(
            "assets/Models/CornellBox/cornell_box.fbx");

    if (cornellBox.empty())
    {
        throw std::runtime_error(
            "Cornell box import produced no meshes.");
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
                "Failed to create imported tank mesh.");
        }

        m_tankMeshes.push_back(std::move(mesh));
    }


    // Cornell box render objects

    for (const auto& mesh : m_cornellMeshes)
    {
        RenderObject boxPart{};
        boxPart.mesh = mesh.get();
        boxPart.material = &m_defaultMaterial;

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
        tankPart.material = &m_defaultMaterial;

        tankPart.transform.position =
            Vector3{
                0.0f,
                0.0f,
                0.0f
            };

        tankPart.transform.scale =
            Vector3{
                1.0f,
                1.0f,
                1.0f
            };

        m_scene.AddRenderObject(tankPart);
    }

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
        &m_defaultMaterial
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