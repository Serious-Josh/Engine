#include "DemoContent/StressTestScene.h"

#include "DemoContent/SceneRegistry.h"
#include "Renderer/ResourceUploader.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/RenderObject.h"
#include "Renderer/Vertex.h"
#include "Renderer/Geometry/SphereGenerator.h"
#include "Renderer/ImageLoader.h"
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

    if(!vertexShader || !pixelShader)
    {
        return false;
    }

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

    if(!m_pipeline.Initialize(
        device,
        pipelineDescription))
    {
        return false;
    }


    SphereGeometry sphere =
        SphereGenerator::Generate(
            1.0f,
            32,
            64);

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


    const std::filesystem::path texturePath =
        "assets/Textures/Materials/TestBaseColor.jpg";

    ImageData image;

    if(!ImageLoader::Load(
        texturePath,
        image))
    {
        Log::Error(
            "Failed to load stress test base color texture.");
        return false;
    }

    if(!m_whiteTexture.Initialize(
        device,
        image.pixels.data(),
        image.width,
        image.height,
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


    m_roughMaterial.pipeline =
        &m_pipeline;

    m_roughMaterial.texture =
        &m_whiteTexture;

    m_roughMaterial.model =
        MaterialModel::Dielectric;

    m_roughMaterial.roughness =
        0.8f;

    m_roughMaterial.ior =
        1.5f;

    m_roughMaterial.baseColor[0] =
        0.55f;

    m_roughMaterial.baseColor[1] =
        0.55f;

    m_roughMaterial.baseColor[2] =
        0.55f;


    m_glossyMaterial.pipeline =
        &m_pipeline;

    m_glossyMaterial.texture =
        &m_whiteTexture;

    m_glossyMaterial.model =
        MaterialModel::Dielectric;

    m_glossyMaterial.roughness =
        0.08f;

    m_glossyMaterial.ior =
        1.5f;

    m_glossyMaterial.baseColor[0] =
        0.25f;

    m_glossyMaterial.baseColor[1] =
        0.45f;

    m_glossyMaterial.baseColor[2] =
        0.8f;


    m_metalMaterial.pipeline =
        &m_pipeline;

    m_metalMaterial.texture =
        &m_whiteTexture;

    m_metalMaterial.model =
        MaterialModel::MetallicRoughness;

    m_metalMaterial.roughness =
        0.18f;

    m_metalMaterial.metallic =
        1.0f;

    m_metalMaterial.baseColor[0] =
        0.8f;

    m_metalMaterial.baseColor[1] =
        0.25f;

    m_metalMaterial.baseColor[2] =
        0.08f;


    m_floorMaterial.pipeline =
        &m_pipeline;

    m_floorMaterial.texture =
        &m_whiteTexture;

    m_floorMaterial.model =
        MaterialModel::Dielectric;

    m_floorMaterial.roughness =
        0.65f;

    m_floorMaterial.ior =
        1.5f;

    m_floorMaterial.baseColor[0] =
        0.3f;

    m_floorMaterial.baseColor[1] =
        0.3f;

    m_floorMaterial.baseColor[2] =
        0.3f;


    constexpr unsigned int gridWidth = 50;
    constexpr unsigned int gridDepth = 50;

    constexpr float spacing = 2.2f;
    constexpr float sphereRadius = 0.75f;

    const float startX =
        -float(gridWidth - 1) *
        spacing *
        0.5f;

    const float startZ =
        -float(gridDepth - 1) *
        spacing *
        0.5f;


    for(unsigned int z = 0;
        z < gridDepth;
        ++z)
    {
        for(unsigned int x = 0;
            x < gridWidth;
            ++x)
        {
            RenderObject object{};

            object.mesh =
                &m_sphereMesh;

            const unsigned int materialIndex =
                (x + z) % 3;

            if(materialIndex == 0)
            {
                object.material =
                    &m_roughMaterial;
            }
            else if(materialIndex == 1)
            {
                object.material =
                    &m_glossyMaterial;
            }
            else
            {
                object.material =
                    &m_metalMaterial;
            }

            object.transform.position =
                Vector3{
                    startX +
                        float(x) * spacing,

                    sphereRadius,

                    startZ +
                        float(z) * spacing
                };

            const unsigned int scalePattern =
                (x * 3 + z * 5) % 4;

            if(scalePattern == 0)
            {
                object.transform.scale =
                    Vector3{
                        sphereRadius * 1.25f,
                        sphereRadius * 0.70f,
                        sphereRadius
                    };
            }
            else if(scalePattern == 1)
            {
                object.transform.scale =
                    Vector3{
                        sphereRadius,
                        sphereRadius * 1.35f,
                        sphereRadius * 0.75f
                    };
            }
            else if(scalePattern == 2)
            {
                object.transform.scale =
                    Vector3{
                        sphereRadius * 0.80f,
                        sphereRadius,
                        sphereRadius * 1.30f
                    };
            }
            else
            {
                object.transform.scale =
                    Vector3{
                        sphereRadius,
                        sphereRadius,
                        sphereRadius
                    };
            }

            m_scene.AddRenderObject(
                object);
        }
    }


    RenderObject floor{};

    floor.mesh =
        &m_planeMesh;

    floor.material =
        &m_floorMaterial;

    floor.transform.position =
        Vector3{
            0.0f,
            0.0f,
            0.0f
        };

    floor.transform.scale =
        Vector3{
            float(gridWidth) *
                spacing *
                0.55f,

            1.0f,

            float(gridDepth) *
                spacing *
                0.55f
        };

    m_scene.AddRenderObject(
        floor);


    ResetCamera();
    ResetDirectionalLight();

    return true;
}


void StressTestScene::Shutdown()
{
    m_scene.GetRenderObjects().clear();

    m_whiteTexture.Shutdown();

    m_sphereMesh.Shutdown();
    m_planeMesh.Shutdown();

    m_pipeline.Shutdown();
}


void StressTestScene::Update(float deltaTime)
{
}


void StressTestScene::SetAspectRatio(
    float aspectRatio)
{
    m_scene.GetCamera().SetAspectRatio(
        aspectRatio);
}


Scene& StressTestScene::GetScene()
{
    return m_scene;
}


const Scene& StressTestScene::GetScene() const
{
    return m_scene;
}


std::vector<Material*>
StressTestScene::GetMaterials()
{
    return
    {
        &m_roughMaterial,
        &m_glossyMaterial,
        &m_metalMaterial,
        &m_floorMaterial
    };
}


void StressTestScene::ResetCamera()
{
    Camera& camera =
        m_scene.GetCamera();

    camera.SetPosition(
        Vector3{
            0.0f,
            18.0f,
            -32.0f
        });

    camera.SetTarget(
        Vector3{
            0.0f,
            0.0f,
            0.0f
        });
}


void StressTestScene::ResetDirectionalLight()
{
    DirectionalLight& light =
        m_scene.GetDirectionalLight();

    light.direction =
        Vector3{
            -0.4f,
            -1.0f,
            0.3f
        }.Normalized();

    light.intensity =
        3.0f;

    light.ambientStrength =
        0.0f;

    light.angularRadius =
        ToRadians(5.0f);
}