#include "Scene/FileSceneAssetCatalog.h"

#include "Renderer/ResourceUploader.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/Vertex.h"
#include "Renderer/TangentGenerator.h"

#include "Renderer/Geometry/SphereGenerator.h"
#include "Renderer/ImageLoader.h"
#include "Renderer/DDSLoader.h"
#include "assets/ProjectAssetResolver.h"
#include "assets/MaterialAssetSerializer.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <limits>
#include <utility>

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

void FileSceneAssetCatalog::SetProjectAssets(
    const ProjectPaths& paths,
    const ProjectAssetRegistry& registry)
{
    m_projectRoot = paths.GetRoot();
    m_registry = &registry;
}

Material* FileSceneAssetCatalog::ResolveMaterial(std::string_view reference)
{
    m_materialError.clear();

    const auto fail = [&](const std::string& message) -> Material*
    {
        m_materialError = message;
        return nullptr;
    };

    if(!m_initialized)
    {
        return fail("Material resolution requires an initialized catalog.");
    }

    if(reference == "builtin:default")
    {
        return &m_defaultMaterial;
    }

    if(!m_registry)
    {
        return fail("Project material resolution requires the live project registry.");
    }

    try
    {
        const auto id = AssetID::ParseReference(reference);

        if(!id)
        {
            return fail("Material reference must be a canonical asset:<uuid> reference.");
        }

        const auto* record = m_registry->Find(*id);

        if(!record || record->type != AssetType::Material)
        {
            return fail("Material reference is not registered as a Material asset.");
        }

        const ProjectPaths paths(m_projectRoot);
        const auto resolved = ProjectAssetResolver::Resolve(paths, *m_registry, *id);

        if(resolved.status != AssetAvailability::Available)
        {
            switch(resolved.status)
            {
            case AssetAvailability::Missing:
                return fail("Registered material file is missing.");
            case AssetAvailability::Unreadable:
                return fail("Registered material path is inaccessible.");
            case AssetAvailability::InvalidPath:
                return fail("Registered material path is invalid or escapes the project's Materials directory.");
            default:
                return fail("Material asset is no longer registered.");
            }
        }

        const MaterialCacheKey key{std::filesystem::canonical(paths.GetRoot()), id->ToString()};
        const auto cached = m_projectMaterials.find(key);

        if(cached != m_projectMaterials.end())
        {
            return &cached->second;
        }

        MaterialAssetData data;
        std::string error;

        if(!MaterialAssetSerializer::LoadFromFile(resolved.resolvedPath, data, &error))
        {
            return fail("Failed to load material " + std::string(reference) + ": " + error);
        }

        Material material{};
        material.pipeline = &m_pipeline;
        material.texture = &m_whiteTexture;
        material.model = data.model;
        std::copy(data.baseColor.begin(), data.baseColor.end(), material.baseColor);
        std::copy(data.conductorN.begin(), data.conductorN.end(), material.conductorN);
        std::copy(data.conductorK.begin(), data.conductorK.end(), material.conductorK);

        material.roughness = data.roughness;
        material.metallic = data.metallic;
        material.ior = data.ior;

        material.surfaceDetailMode = data.surfaceDetailMode;
        material.heightScale = data.heightScale;
        material.heightBias = data.heightBias;
        material.preferARM = data.preferARM;

        const auto attach = [&](
            const std::optional<std::string>& textureReference,
            ProjectTextureInterpretation interpretation,
            Texture*& destination)
        {
            if(!textureReference)
            {
                return true;
            }

            Texture* texture = ResolveTexture(*textureReference, interpretation, paths, *m_registry, &error);

            if(!texture)
            {
                m_materialError = "Material " + std::string(reference) + " texture " + *textureReference + ": " + error;
                return false;
            }

            destination = texture;
            return true;
        };

        if(!attach(data.baseColorTexture, ProjectTextureInterpretation::SRGBColor, material.texture) ||
           !attach(data.normalTexture, ProjectTextureInterpretation::NormalMap, material.normalTexture) ||
           !attach(data.roughnessTexture, ProjectTextureInterpretation::LinearData, material.roughnessTexture) ||
           !attach(data.metallicTexture, ProjectTextureInterpretation::LinearData, material.metallicTexture))
        {
            return nullptr;
        }

        if(!attach(data.armTexture, ProjectTextureInterpretation::LinearData, material.armTexture) ||
           !attach(data.aoTexture, ProjectTextureInterpretation::LinearData, material.aoTexture) ||
           !attach(data.heightTexture, ProjectTextureInterpretation::LinearData, material.heightTexture))
        {
            return nullptr;
        }

        // Publish only after every dependency has a completed upload and valid SRV.
        return &m_projectMaterials.emplace(key, material).first->second;
    }
    catch(const std::exception& exception)
    {
        return fail("Project material resolution failed: " + std::string(exception.what()));
    }
}

std::vector<Material*> FileSceneAssetCatalog::GetMaterials()
{
    if (!m_initialized)
    {
        return {};
    }

    std::vector<Material*> materials{&m_defaultMaterial};
    materials.reserve(1 + m_projectMaterials.size());

    for(auto& [key, material] : m_projectMaterials)
    {
        materials.push_back(&material);
    }

    return materials;
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
    m_device = device;
    m_uploader = &uploader;
    m_descriptorHeap = &descriptorHeap;
    m_initialized = true;
    return true;
}

void FileSceneAssetCatalog::Shutdown()
{
    m_initialized = false;

    m_defaultMaterial = Material{};
    m_projectMaterials.clear();
    m_registry = nullptr;
    m_projectRoot.clear();
    m_materialError.clear();

    // Same precondition as built-in assets: Renderer has released the scene after
    // checked GPU completion. Texture::Shutdown immediately returns descriptors.
    for(auto& [key, texture] : m_projectTextures)
    {
        texture.Shutdown();
    }

    m_projectTextures.clear();
    m_device = nullptr;
    m_uploader = nullptr;
    m_descriptorHeap = nullptr;

    m_whiteTexture.Shutdown();
    m_sphereMesh.Shutdown();
    m_pipeline.Shutdown();

    m_vertexLayout = VertexLayout{};
}

Texture* FileSceneAssetCatalog::ResolveTexture(
    std::string_view reference,
    ProjectTextureInterpretation interpretation,
    const ProjectPaths& paths,
    const ProjectAssetRegistry& registry,
    std::string* errorMessage)
{
    const auto fail = [&](const std::string& message) -> Texture*
    {
        if(errorMessage)
        {
            *errorMessage = message;
        }

        return nullptr;
    };

    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(!m_initialized || !m_device || !m_uploader || !m_descriptorHeap)
    {
        return fail("Texture loading requires an initialized file-scene asset catalog.");
    }

    MipFilterMode filter;

    switch(interpretation)
    {
    case ProjectTextureInterpretation::SRGBColor:
        filter = MipFilterMode::SRGBColor;
        break;
    case ProjectTextureInterpretation::LinearData:
        filter = MipFilterMode::LinearData;
        break;
    case ProjectTextureInterpretation::NormalMap:
        filter = MipFilterMode::NormalMap;
        break;
    default:
        return fail("Unsupported project texture sampling interpretation.");
    }

    const auto id = AssetID::ParseReference(reference);

    if(!id)
    {
        return fail("Texture reference must be a canonical asset:<uuid> reference.");
    }

    const auto* record = registry.Find(*id);

    if(!record || record->type != AssetType::Texture)
    {
        return fail("Texture reference is not registered as a Texture asset.");
    }

    const auto resolved = ProjectAssetResolver::Resolve(paths, registry, *id);

    if(resolved.status != AssetAvailability::Available)
    {
        switch(resolved.status)
        {
        case AssetAvailability::Missing:
            return fail("Registered texture file is missing.");
        case AssetAvailability::Unreadable:
            return fail("Registered texture path is inaccessible.");
        case AssetAvailability::InvalidPath:
            return fail("Registered texture path is invalid or escapes the project's Textures directory.");
        default:
            return fail("Texture asset is no longer registered.");
        }
    }

    Texture texture;

    try
    {
        std::error_code pathError;
        const auto root = std::filesystem::canonical(paths.GetRoot(), pathError);

        if(pathError)
        {
            return fail("Cannot resolve the texture's project root: " + pathError.message());
        }

        // Revalidate registry type and containment even on cache hits. Renames retain
        // the cached resource; other projects cannot alias it through a reused UUID.
        const TextureCacheKey key{root, id->ToString(), interpretation};
        const auto cached = m_projectTextures.find(key);

        if(cached != m_projectTextures.end())
        {
            return &cached->second;
        }

        std::string extension = resolved.resolvedPath.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });

        if(extension == ".dds")
        {
            // Preserve DDSLoader's format restrictions and authored mip chain.
            if(!DDSLoader::LoadTexture(m_device, *m_uploader, *m_descriptorHeap,
                                       resolved.resolvedPath, texture,
                                       interpretation == ProjectTextureInterpretation::SRGBColor))
            {
                texture.Shutdown();
                return fail("DDS texture load/upload failed: " + resolved.resolvedPath.string() +
                            ". Supported formats are BC1, BC4, and BC5; only BC1 supports sRGB.");
            }
        }
        else if(extension == ".hdr")
        {
            if(interpretation != ProjectTextureInterpretation::LinearData)
            {
                return fail("HDR textures require linear interpretation; sRGB and normal-map HDR loading are unsupported.");
            }

            HDRImageData image;

            if(!ImageLoader::LoadHDR(resolved.resolvedPath, image))
            {
                return fail("Cannot decode HDR texture: " + resolved.resolvedPath.string());
            }

            // Existing HDR decoding is float RGBA and has no CPU float mip generator.
            if(!texture.Initialize(m_device, image.pixels.data(), image.width, image.height,
                                   DXGI_FORMAT_R32G32B32A32_FLOAT, sizeof(float) * 4) ||
               !m_uploader->UploadTexture(texture.GetResource(), texture.GetInitialData(), texture.GetInitialDataSize()) ||
               !texture.CreateSRV(m_device, *m_descriptorHeap))
            {
                texture.Shutdown();
                return fail("Failed to create/upload HDR texture or allocate its SRV.");
            }
        }
        else
        {
            const bool commonImage = extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga";
            const bool additionalImage = extension == ".bmp" || extension == ".psd" || extension == ".gif" || extension == ".pic";
            const bool portableImage = extension == ".pnm" || extension == ".ppm" || extension == ".pgm";

            if(!commonImage && !additionalImage && !portableImage)
            {
                return fail("Unsupported project texture extension: " + extension);
            }

            ImageData image;
            ImageMipChain mipChain;

            if(!ImageLoader::Load(resolved.resolvedPath, image) ||
               !ImageLoader::GenerateMipChain(image, filter, mipChain))
            {
                return fail("Cannot decode texture or generate its mip chain: " + resolved.resolvedPath.string());
            }

            if(mipChain.levels.size() > (std::numeric_limits<unsigned int>::max)())
            {
                return fail("Texture mip count exceeds the supported limit.");
            }

            std::vector<TextureSubresourceData> subresources;
            subresources.reserve(mipChain.levels.size());

            for(const auto& level : mipChain.levels)
            {
                subresources.push_back({level.pixels.data(), static_cast<size_t>(level.width) * 4, level.pixels.size()});
            }

            const auto mipCount = static_cast<unsigned int>(subresources.size());
            const DXGI_FORMAT srvFormat = interpretation == ProjectTextureInterpretation::SRGBColor
                ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;

            if(!texture.InitializeMipmapped(m_device, image.width, image.height, mipCount, DXGI_FORMAT_R8G8B8A8_UNORM) ||
               !m_uploader->UploadTexture(texture.GetResource(), subresources.data(), mipCount) ||
               !texture.CreateSRV(m_device, *m_descriptorHeap, srvFormat))
            {
                texture.Shutdown();
                return fail("Failed to create/upload image texture or allocate its SRV.");
            }
        }

        // Upload success proves completion on the uploader queue. Publication does
        // not imply shader-read state or authorize destruction after future draws.
        const auto inserted = m_projectTextures.emplace(key, std::move(texture));
        return &inserted.first->second;
    }
    catch(const std::exception& error)
    {
        // Uploader uncertainty terminates rather than returning/throwing here.
        // Only this unpublished, upload-complete texture may be released.
        texture.Shutdown();
        return fail(std::string("Project texture loading failed: ") + error.what());
    }
}
