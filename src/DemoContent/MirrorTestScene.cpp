#include "MirrorTestScene.h"
#include "Renderer/ResourceUploader.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/RenderObject.h"
#include "Scene/Camera.h"
#include "Scene/DirectionalLight.h"

#include "Renderer/Vertex.h"
#include "Renderer/Geometry/SphereGenerator.h"
#include "Renderer/ImageLoader.h"
#include "Core/Log.h"

#include "DemoContent/SceneRegistry.h"

#include <cstdint>

REGISTER_DEMO_SCENE("Mirror Test", MirrorTestScene);

bool MirrorTestScene::Initialize(
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    const Shader* vertexShader,
    const Shader* pixelShader)
{
    m_vertexLayout = CreateVertexLayout();

    if(!vertexShader || !pixelShader)
    {
        return false;
    }

    PipelineDescription pipelineDescription{};
    pipelineDescription.vertexShader = vertexShader;
    pipelineDescription.pixelShader = pixelShader;
    pipelineDescription.vertexLayout = &m_vertexLayout;

    pipelineDescription.renderTargets.count = 3;
    pipelineDescription.renderTargets.formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipelineDescription.renderTargets.formats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pipelineDescription.renderTargets.formats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;

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

    if(!m_pipeline.Initialize(device, pipelineDescription))
    {
        return false;
    }

    SphereGeometry sphere = SphereGenerator::Generate(1.0f, 32, 64);

    if(!m_sphereMesh.Initialize(
        device,
        uploader,
        descriptorHeap,
        sphere.vertices.data(),
        sphere.vertices.size(),
        m_vertexLayout,
        sphere.indices.data(),
        sphere.indices.size()))
    {
        return false;
    }

    const Vertex planeVertices[] =
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

    const unsigned int planeIndices[] =
    {
        0, 1, 2,
        0, 2, 3
    };

    if(!m_planeMesh.Initialize(
        device,
        uploader,
        descriptorHeap,
        planeVertices,
        4,
        m_vertexLayout,
        planeIndices,
        6))
    {
        return false;
    }

    const std::filesystem::path sphereTexturePath = "assets/Textures/Materials/TestBaseColor.jpg";

    ImageData sphereImage;

    if(!ImageLoader::Load(
        sphereTexturePath,
        sphereImage))
    {
        Log::Error(
            "Failed to load mirror test sphere texture.");
        return false;
    }

    if(!m_sphereTexture.Initialize(
        device,
        sphereImage.pixels.data(),
        sphereImage.width,
        sphereImage.height,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        4))
    {
        return false;
    }

    if(!uploader.UploadTexture(
        m_sphereTexture.GetResource(),
        m_sphereTexture.GetInitialData(),
        m_sphereTexture.GetInitialDataSize()))
    {
        return false;
    }

    if(!m_sphereTexture.CreateSRV(
        device,
        descriptorHeap,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
    {
        return false;
    }


    // Neutral plane texture

    const std::uint8_t whitePixel[4] =
    {
        255,
        255,
        255,
        255
    };

    if(!m_whiteTexture.Initialize(
        device,
        whitePixel,
        1,
        1,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        4))
    {
        return false;
    }

    if(!uploader.UploadTexture(
        m_whiteTexture.GetResource(),
        m_whiteTexture.GetInitialData(),
        m_whiteTexture.GetInitialDataSize()))
    {
        return false;
    }

    if(!m_whiteTexture.CreateSRV(
        device,
        descriptorHeap,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
    {
        return false;
    }


    // Mirror A material

    m_mirrorAMaterial.pipeline = &m_pipeline;
    m_mirrorAMaterial.texture = &m_whiteTexture;

    m_mirrorAMaterial.model =
        MaterialModel::MetallicRoughness;

    m_mirrorAMaterial.roughness = 0.0f;
    m_mirrorAMaterial.metallic = 1.0f;

    m_mirrorAMaterial.baseColor[0] = 1.0f;
    m_mirrorAMaterial.baseColor[1] = 1.0f;
    m_mirrorAMaterial.baseColor[2] = 1.0f;


    // Mirror B material

    m_mirrorBMaterial.pipeline = &m_pipeline;
    m_mirrorBMaterial.texture = &m_whiteTexture;

    m_mirrorBMaterial.model =
        MaterialModel::MetallicRoughness;

    m_mirrorBMaterial.roughness = 0.10f;
    m_mirrorBMaterial.metallic = 1.0f;

    m_mirrorBMaterial.baseColor[0] = 1.0f;
    m_mirrorBMaterial.baseColor[1] = 1.0f;
    m_mirrorBMaterial.baseColor[2] = 1.0f;



    // Red sphere material

    m_coloredMaterial.pipeline = &m_pipeline;
    m_coloredMaterial.texture = &m_sphereTexture;

    m_coloredMaterial.model =
        MaterialModel::MetallicRoughness;

    m_coloredMaterial.roughness = 0.5f;
    m_coloredMaterial.metallic = 0.0f;

    m_coloredMaterial.baseColor[0] = 1.0f;
    m_coloredMaterial.baseColor[1] = 0.02f;
    m_coloredMaterial.baseColor[2] = 0.02f;


    // Floor material

    m_floorMaterial.pipeline = &m_pipeline;
    m_floorMaterial.texture = &m_whiteTexture;

    m_floorMaterial.model =
        MaterialModel::Dielectric;

    m_floorMaterial.roughness = 0.7f;
    m_floorMaterial.ior = 1.5f;

    m_floorMaterial.baseColor[0] = 0.25f;
    m_floorMaterial.baseColor[1] = 0.25f;
    m_floorMaterial.baseColor[2] = 0.25f;


    // Mirror A

    RenderObject mirrorA{};
    mirrorA.mesh = &m_planeMesh;
    mirrorA.material = &m_mirrorAMaterial;

    mirrorA.transform.position =
        Vector3{
            -1.5f,
            1.0f,
            2.0f
        };

    mirrorA.transform.rotation =
        Vector3{
            ToRadians(-90.0f),
            ToRadians(-42.7f),
            0.0f
        };

    mirrorA.transform.scale =
        Vector3{
            2.0f,
            1.0f,
            2.5f
        };

    m_scene.AddRenderObject(mirrorA);


    // Mirror B

    RenderObject mirrorB{};
    mirrorB.mesh = &m_planeMesh;
    mirrorB.material = &m_mirrorBMaterial;

    mirrorB.transform.position =
        Vector3{
            2.5f,
            1.0f,
            3.0f
        };

    mirrorB.transform.rotation =
        Vector3{
            ToRadians(-90.0f),
            ToRadians(115.0f),
            0.0f
        };

    mirrorB.transform.scale =
        Vector3{
            1.5f,
            1.0f,
            2.0f
        };

    m_scene.AddRenderObject(mirrorB);


    // Red terminal object

    RenderObject coloredSphere{};
    coloredSphere.mesh = &m_sphereMesh;
    coloredSphere.material = &m_coloredMaterial;

    coloredSphere.transform.position =
        Vector3{
            1.2f,
            1.0f,
            2.8f
        };

    coloredSphere.transform.scale =
        Vector3{
            0.75f,
            0.75f,
            0.75f
        };

    m_scene.AddRenderObject(coloredSphere);


    // Floor

    RenderObject floor{};
    floor.mesh = &m_planeMesh;
    floor.material = &m_floorMaterial;

    floor.transform.position =
        Vector3{
            0.0f,
            -1.0f,
            2.0f
        };

    floor.transform.scale =
        Vector3{
            7.0f,
            1.0f,
            7.0f
        };

    m_scene.AddRenderObject(floor);

    ResetCamera();
    ResetDirectionalLight();

    return true;
}

void MirrorTestScene::Shutdown()
{
    m_scene.ClearRenderObjects();

    m_sphereTexture.Shutdown();
    m_whiteTexture.Shutdown();

    m_sphereMesh.Shutdown();
    m_planeMesh.Shutdown();

    m_pipeline.Shutdown();
}

void MirrorTestScene::Update(float deltaTime)
{
    (void)deltaTime;
}

void MirrorTestScene::SetAspectRatio(float aspectRatio)
{
    m_scene.GetCamera().SetAspectRatio(aspectRatio);
}

Scene& MirrorTestScene::GetScene()
{
    return m_scene;
}

const Scene& MirrorTestScene::GetScene() const
{
    return m_scene;
}

std::vector<Material*> MirrorTestScene::GetMaterials()
{
    return {
        &m_mirrorAMaterial,
        &m_mirrorBMaterial,
        &m_coloredMaterial,
        &m_floorMaterial
    };
}

void MirrorTestScene::ResetCamera()
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

void MirrorTestScene::ResetDirectionalLight()
{
    DirectionalLight& light =
        m_scene.GetDirectionalLight();

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