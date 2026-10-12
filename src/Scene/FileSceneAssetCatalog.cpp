#include "Scene/FileSceneAssetCatalog.h"

#include "Renderer/ResourceUploader.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/Vertex.h"
#include "Renderer/TangentGenerator.h"

#include "Renderer/Geometry/SphereGenerator.h"

Mesh* FileSceneAssetCatalog::ResolveMesh(std::string_view reference)
{
    if (!m_initialized)
    {
        return nullptr;
    }

    if (reference == "builtin:sphere")
    {
        return &m_sphereMesh;
    }

    return nullptr;
}

Material* FileSceneAssetCatalog::ResolveMaterial(std::string_view reference)
{
    if (!m_initialized)
    {
        return nullptr;
    }

    if (reference == "builtin:default")
    {
        return &m_defaultMaterial;
    }

    return nullptr;
}

std::vector<Material*> FileSceneAssetCatalog::GetMaterials()
{
    if (!m_initialized)
    {
        return {};
    }

    return {&m_defaultMaterial};
}


bool FileSceneAssetCatalog::Initialize(
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    const Shader* vertexShader,
    const Shader* pixelShader)
{
    if (m_initialized)
    {
        return false;
    }

    if (!device || !vertexShader || !pixelShader)
    {
        return false;
    }

    // Clear any resources left by an earlier failed initialization.
    // The caller must ensure no GPU work references those resources.
    Shutdown();

    m_vertexLayout = CreateVertexLayout();

    PipelineDescription description{};

    description.vertexShader = vertexShader;
    description.pixelShader = pixelShader;
    description.vertexLayout = &m_vertexLayout;

    description.renderTargets.count = 4;
    description.renderTargets.formats[0] =
        DXGI_FORMAT_R8G8B8A8_UNORM;
    description.renderTargets.formats[1] =
        DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.renderTargets.formats[2] =
        DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.renderTargets.formats[3] =
        DXGI_FORMAT_R16G16B16A16_FLOAT;

    description.rootParameters.push_back({
        RootParameterType::Constants,
        0,
        9,
        D3D12_SHADER_VISIBILITY_PIXEL
    });

    description.rootParameters.push_back({
        RootParameterType::ConstantBuffer,
        1,
        0,
        D3D12_SHADER_VISIBILITY_ALL
    });

    description.rootParameters.push_back({
        RootParameterType::ConstantBuffer,
        2,
        0,
        D3D12_SHADER_VISIBILITY_VERTEX
    });

    description.depthEnabled = true;
    description.depthFormat = DXGI_FORMAT_D32_FLOAT;

    if (!m_pipeline.Initialize(device, description))
    {
        Shutdown();
        return false;
    }

    SphereGeometry sphere =
        SphereGenerator::Generate(1.0f, 32, 64);

    TangentGenerator::GenerateTangents(
        sphere.vertices,
        sphere.indices);

    if (!m_sphereMesh.Initialize(
            device,
            uploader,
            descriptorHeap,
            sphere.vertices.data(),
            sphere.vertices.size(),
            m_vertexLayout,
            sphere.indices.data(),
            sphere.indices.size()))
    {
        Shutdown();
        return false;
    }

    // Opaque white base texture. The material's baseColor
    // controls the final tint.
    constexpr unsigned char whitePixel[4] = {
        255, 255, 255, 255
    };

    if (!m_whiteTexture.Initialize(
            device,
            whitePixel,
            1,
            1,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            4))
    {
        Shutdown();
        return false;
    }

    if (!uploader.UploadTexture(
        m_whiteTexture.GetResource(),
        m_whiteTexture.GetInitialData(),
        m_whiteTexture.GetInitialDataSize()))
    {
        Shutdown();
        return false;
    }

    if (!m_whiteTexture.CreateSRV(
            device,
            descriptorHeap))
    {
        Shutdown();
        return false;
    }

    m_defaultMaterial = Material{};
    m_defaultMaterial.pipeline = &m_pipeline;
    m_defaultMaterial.texture = &m_whiteTexture;
    m_defaultMaterial.model = MaterialModel::Dielectric;

    m_defaultMaterial.baseColor[0] = 0.8f;
    m_defaultMaterial.baseColor[1] = 0.8f;
    m_defaultMaterial.baseColor[2] = 0.8f;

    m_defaultMaterial.roughness = 0.5f;
    m_defaultMaterial.ior = 1.5f;

    // Publish assets only after all initialization succeeds.
    m_initialized = true;
    return true;
}

void FileSceneAssetCatalog::Shutdown()
{
    m_initialized = false;

    m_defaultMaterial = Material{};

    m_whiteTexture.Shutdown();
    m_sphereMesh.Shutdown();
    m_pipeline.Shutdown();

    m_vertexLayout = VertexLayout{};
}