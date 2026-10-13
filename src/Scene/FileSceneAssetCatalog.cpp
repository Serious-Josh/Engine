#include "Scene/FileSceneAssetCatalog.h"
#include "Scene/MaterialParameterPropagation.h"
#include "Renderer/Renderer.h"
#include "Renderer/Preview/MaterialThumbnailValidation.h"

#include "Renderer/ResourceUploader.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/Vertex.h"
#include "Renderer/TangentGenerator.h"

#include "Renderer/Geometry/SphereGenerator.h"
#include "Renderer/ImageLoader.h"
#include "Renderer/DDSLoader.h"
#include "Renderer/ProjectTextureLoader.h"
#include "assets/ProjectAssetResolver.h"
#include "assets/MaterialAssetSerializer.h"

#include <algorithm>
#include <atomic>
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

FileSceneAssetCatalog::MaterialVersion::~MaterialVersion()
{
    // Only unpublished uploads may be cleaned by unwinding. Published resources require
    // checked scene teardown; unexpected owner destruction must not free in-flight assets.
    if(!published)
    {
        ReleaseTextures();
    }
}

void FileSceneAssetCatalog::MaterialVersion::ReleaseTextures()
{
    for(auto& [key, texture] : textures)
    {
        texture.Shutdown();
    }
}

Material* FileSceneAssetCatalog::ResolveMaterial(std::string_view reference)
{
    return ResolveMaterialVersion(reference, false);
}

Material* FileSceneAssetCatalog::ResolveFreshMaterial(std::string_view reference)
{
    return ResolveMaterialVersion(reference, true);
}

Material* FileSceneAssetCatalog::ResolveMaterialVersion(std::string_view reference, bool fresh)
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

        if(!fresh && cached != m_projectMaterials.end())
        {
            return &cached->second;
        }

        if(fresh && m_materialVersions.size() >= kMaterialVersionCapacity)
        {
            return fail("Fresh material-version capacity reached; save/reload the scene before assigning more versions.");
        }

        auto version = fresh ? std::make_unique<MaterialVersion>() : nullptr;
        MaterialAssetData data;
        std::string error;
        const auto modified = std::filesystem::last_write_time(resolved.resolvedPath);
        const auto size = std::filesystem::file_size(resolved.resolvedPath);

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

            Texture* texture = nullptr;

            if(version)
            {
                const auto textureID = AssetID::ParseReference(*textureReference);
                const auto* textureRecord = textureID ? m_registry->Find(*textureID) : nullptr;
                const auto resolution = textureID ? ProjectAssetResolver::Resolve(paths, *m_registry, *textureID) : AssetResolution{};

                if(!textureRecord || textureRecord->type != AssetType::Texture || resolution.status != AssetAvailability::Available)
                {
                    error = "Fresh material texture is invalid, missing, inaccessible, or unsafe.";
                }
                else
                {
                    const auto textureKey = std::pair{textureID->ToString(), interpretation};
                    auto [entry, inserted] = version->textures.try_emplace(textureKey);

                    if(!inserted)
                    {
                        texture = &entry->second;
                    }
                    else
                    {
                        const auto evidence = InspectTextureSource(resolution.resolvedPath);
                        const auto textureSize = std::filesystem::file_size(resolution.resolvedPath);
                        const auto textureTime = std::filesystem::last_write_time(resolution.resolvedPath);

                        const bool loaded = LoadProjectTexture(m_device, *m_uploader, *m_descriptorHeap,
                            resolution.resolvedPath, interpretation, entry->second, &error);

                        if(loaded && textureSize == std::filesystem::file_size(resolution.resolvedPath) &&
                           textureTime == std::filesystem::last_write_time(resolution.resolvedPath) &&
                           evidence == InspectTextureSource(resolution.resolvedPath))
                        {
                            version->textureSources.emplace(textureKey, evidence);
                            texture = &entry->second;
                        }
                        else if(loaded)
                        {
                            error = "Texture changed during fresh material loading; retry.";
                        }
                    }
                }
            }
            else
            {
                texture = ResolveTexture(*textureReference, interpretation, paths, *m_registry, &error);
            }

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

        if(fresh)
        {
            if(modified != std::filesystem::last_write_time(resolved.resolvedPath) ||
               size != std::filesystem::file_size(resolved.resolvedPath))
            {
                return fail("Material changed during fresh loading; retry.");
            }

            version->source = data;
            version->projectIdentity = key.first;
            version->assetID = *id;
            version->material = material;

            auto* result = &version->material;
            m_materialVersions.push_back(std::move(version));
            m_materialVersions.back()->published = true;
            return result;
        }

        // Publish only after every dependency has a completed upload and valid SRV.
        m_materialSources.insert_or_assign(key, data);
        return &m_projectMaterials.emplace(key, material).first->second;
    }
    catch(const std::exception& exception)
    {
        return fail("Project material resolution failed: " + std::string(exception.what()));
    }
}

bool FileSceneAssetCatalog::OwnsMaterial(const Material& material) const noexcept
{
    if(!m_initialized)
    {
        return false;
    }

    if(&material == &m_defaultMaterial)
    {
        return true;
    }

    for(const auto& [key, owned] : m_projectMaterials)
    {
        if(&owned == &material)
        {
            return true;
        }
    }

    for(const auto& version : m_materialVersions)
    {
        if(&version->material == &material)
        {
            return true;
        }
    }

    return false;
}

std::vector<Material*> FileSceneAssetCatalog::GetMaterials()
{
    if (!m_initialized)
    {
        return {};
    }

    std::vector<Material*> materials{&m_defaultMaterial};
    materials.reserve(1 + m_projectMaterials.size() + m_materialVersions.size());

    for(auto& [key, material] : m_projectMaterials)
    {
        materials.push_back(&material);
    }

    for(const auto& version : m_materialVersions)
    {
        materials.push_back(&version->material);
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
    static std::atomic<std::uint64_t> nextIdentity{1};
    m_catalogIdentity = nextIdentity.fetch_add(1);
    m_initialized = true;
    return true;
}

void FileSceneAssetCatalog::Shutdown()
{
    CancelSavedMaterialRevision();
    m_initialized = false;
    m_catalogIdentity = 0;

    m_defaultMaterial = Material{};
    m_projectMaterials.clear();
    m_materialSources.clear();

    // Same checked GPU completion precondition as every other scene-owned descriptor.
    for(auto& version : m_materialVersions)
    {
        version->ReleaseTextures();
    }

    m_materialVersions.clear();
    m_liveTextures.clear();
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
    m_projectTextureSources.clear();
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

    // Preserve interpretation rejection before reference/registry validation.
    switch(interpretation)
    {
    case ProjectTextureInterpretation::SRGBColor:
    case ProjectTextureInterpretation::LinearData:
    case ProjectTextureInterpretation::NormalMap:
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

        const auto evidence = InspectTextureSource(resolved.resolvedPath);

        if(!LoadProjectTexture(m_device, *m_uploader, *m_descriptorHeap,
                resolved.resolvedPath, interpretation, texture, errorMessage))
        {
            texture.Shutdown();
            return nullptr;
        }

        if(evidence != InspectTextureSource(resolved.resolvedPath))
        {
            texture.Shutdown();
            return fail("Texture changed during upload; retry.");
        }

        m_projectTextureSources.insert_or_assign(key, evidence);

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

std::optional<RuntimeMaterialVersions> FileSceneAssetCatalog::GetMaterialVersions(
    const std::filesystem::path& canonicalProject, const AssetID& id, std::string* errorMessage) const
{
    return GetOwnedMaterialVersions(canonicalProject, id, true, errorMessage);
}

std::optional<RuntimeMaterialVersions> FileSceneAssetCatalog::GetOwnedMaterialVersions(
    const std::filesystem::path& canonicalProject, const AssetID& id, bool requireRegistration, std::string* errorMessage) const
{
    if(errorMessage)
    {
        errorMessage->clear();
    }

    std::error_code error;
    const auto root = std::filesystem::canonical(m_projectRoot, error);
    const auto* record = m_registry ? m_registry->Find(id) : nullptr;

    if(!m_initialized || error || root != canonicalProject || (requireRegistration &&
       (!record || record->type != AssetType::Material || !ValidateAssetRecord(*record))))
    {
        if(errorMessage)
        {
            *errorMessage = "Runtime material identity is unavailable for this project or registration.";
        }

        return std::nullopt;
    }

    RuntimeMaterialVersions result{root, id, m_catalogIdentity};
    const auto cached = m_projectMaterials.find({root, id.ToString()});

    if(cached != m_projectMaterials.end())
    {
        result.versions.push_back({0, cached->second.gpuIndex});
    }

    for(size_t index = 0; index < m_materialVersions.size(); ++index)
    {
        const auto& version = *m_materialVersions[index];

        if(version.projectIdentity == root && version.assetID && *version.assetID == id)
        {
            result.versions.push_back({index + 1, version.material.gpuIndex});
        }
    }

    return result;
}

FileSceneAssetCatalog::TextureSource FileSceneAssetCatalog::InspectTextureSource(const std::filesystem::path& path)
{
    // Reuse the existing checked, streaming SHA-256 utility with a fresh cache so
    // equal timestamps cannot hide a content change. No image decoding or GPU work.
    MaterialThumbnailValidation validation;
    TextureSource source{path, std::filesystem::file_size(path), std::filesystem::last_write_time(path), validation.Digest(path)};

    if(source.size != std::filesystem::file_size(path) || source.modified != std::filesystem::last_write_time(path))
    {
        throw std::runtime_error("Texture changed during source validation.");
    }

    return source;
}

bool FileSceneAssetCatalog::PublishSavedMaterialParameters(const RuntimeMaterialVersions& expected,
    const MaterialAssetData& data, std::vector<GPUMaterial> table,
    const std::function<bool(const std::vector<GPUMaterial>&)>& publisher, std::string* errorMessage)
{
    const auto fail = [&](const std::string& message)
    {
        if(errorMessage)
        {
            *errorMessage = message;
        }

        return false;
    };
    const auto current = GetMaterialVersions(expected.projectIdentity, expected.assetID);

    if(!current || current->catalogIdentity != expected.catalogIdentity || current->versions != expected.versions)
    {
        return fail("Runtime material versions changed before parameter publication.");
    }

    try
    {
        MaterialAssetSerializer::Serialize(data); // Existing validation; never writes a file.
        const MaterialCacheKey materialKey{expected.projectIdentity, expected.assetID.ToString()};
        std::vector<Material*> targets;
        std::map<std::filesystem::path, TextureSource> checkedSources;
        const ProjectPaths paths(expected.projectIdentity);
        const auto verify = [&](const MaterialAssetData& source, const auto& findEvidence)
        {
            if(!MaterialParameterPropagation::SameTextures(source, data))
            {
                return false;
            }

            const std::optional<std::string> references[] = {source.baseColorTexture, source.normalTexture,
                source.roughnessTexture, source.metallicTexture, source.armTexture, source.aoTexture, source.heightTexture};

            for(size_t slot = 0; slot < 7; ++slot)
            {
                if(!references[slot])
                {
                    continue;
                }

                const auto id = AssetID::ParseReference(*references[slot]);
                const auto* record = id ? m_registry->Find(*id) : nullptr;

                if(!record || record->type != AssetType::Texture)
                {
                    return false;
                }

                const auto resolved = ProjectAssetResolver::Resolve(paths, *m_registry, *id);

                if(resolved.status != AssetAvailability::Available)
                {
                    return false;
                }

                const auto interpretation = slot == 0 ? ProjectTextureInterpretation::SRGBColor :
                    slot == 1 ? ProjectTextureInterpretation::NormalMap : ProjectTextureInterpretation::LinearData;
                const auto* evidence = findEvidence(id->ToString(), interpretation);
                auto found = checkedSources.find(resolved.resolvedPath);

                if(found == checkedSources.end())
                {
                    found = checkedSources.emplace(resolved.resolvedPath, InspectTextureSource(resolved.resolvedPath)).first;
                }

                if(!evidence || *evidence != found->second)
                {
                    return false;
                }
            }

            return true;
        };

        for(const auto& version : current->versions)
        {
            Material* material = nullptr;

            if(version.version == 0)
            {
                const auto source = m_materialSources.find(materialKey);

                if(source == m_materialSources.end() || !verify(source->second, [&](const std::string& id, auto interpretation)
                    -> const TextureSource*
                    {
                        const auto evidence = m_projectTextureSources.find({expected.projectIdentity, id, interpretation});
                        return evidence == m_projectTextureSources.end() ? nullptr : &evidence->second;
                    }))
                {
                    return fail("Saved revision changes texture bindings, interpretation, or source contents; reload the scene to apply it.");
                }

                material = &m_projectMaterials.at(materialKey);
            }
            else
            {
                auto& owned = *m_materialVersions.at(static_cast<size_t>(version.version - 1));

                if(!verify(owned.source, [&](const std::string& id, auto interpretation) -> const TextureSource*
                    {
                        const auto evidence = owned.textureSources.find({id, interpretation});
                        return evidence == owned.textureSources.end() ? nullptr : &evidence->second;
                    }))
                {
                    return fail("Saved revision changes a fresh version's texture dependencies; reload the scene to apply it.");
                }

                material = &owned.material;
            }

            if(material->gpuIndex != version.gpuIndex)
            {
                return fail("Runtime material GPU index changed.");
            }

            targets.push_back(material);
        }

        return MaterialParameterPropagation::Publish(targets, data, std::move(table), publisher, errorMessage);
    }
    catch(const std::exception& error)
    {
        return fail(std::string("Saved material parameter preparation failed: ") + error.what());
    }
}

Texture* FileSceneAssetCatalog::FindReusableTexture(const std::string& id, ProjectTextureInterpretation interpretation,
    const TextureSource& source)
{
    const TextureCacheKey key{std::filesystem::canonical(m_projectRoot), id, interpretation};
    const auto evidence = m_projectTextureSources.find(key);
    const auto cached = m_projectTextures.find(key);

    if(evidence != m_projectTextureSources.end() && cached != m_projectTextures.end() && evidence->second == source)
    {
        return &cached->second;
    }

    for(auto& version : m_materialVersions)
    {
        const auto textureKey = std::pair{id, interpretation};
        const auto found = version->textureSources.find(textureKey);
        const auto texture = version->textures.find(textureKey);

        if(found != version->textureSources.end() && texture != version->textures.end() && found->second == source)
        {
            return &texture->second;
        }
    }

    for(auto& texture : m_liveTextures)
    {
        if(texture->assetID == id && texture->interpretation == interpretation && texture->source == source)
        {
            return &texture->texture;
        }
    }

    return nullptr;
}

void FileSceneAssetCatalog::CancelSavedMaterialRevision()
{
    if(m_savedRevisionJob)
    {
        for(auto& dependency : m_savedRevisionJob->dependencies)
        {
            if(dependency.ticket)
            {
                m_savedRevisionJob->renderer->CancelPreparedTextureUpload(dependency.ticket);
            }
        }

        m_savedRevisionJob.reset(); // Uploaded-but-unpublished textures have already completed.
    }
}

void FileSceneAssetCatalog::PruneUnusedTextures(const std::vector<Texture*>& used)
{
    const auto unused = [&](const Texture* texture)
    {
        return std::find(used.begin(), used.end(), texture) == used.end();
    };

    for(auto entry = m_projectTextures.begin(); entry != m_projectTextures.end();)
    {
        if(unused(&entry->second))
        {
            entry->second.Shutdown(); // Old GPU tables retain K.4A allocation leases.
            m_projectTextureSources.erase(entry->first);
            entry = m_projectTextures.erase(entry);
        }
        else
        {
            ++entry;
        }
    }

    for(auto& version : m_materialVersions)
    {
        for(auto entry = version->textures.begin(); entry != version->textures.end();)
        {
            if(unused(&entry->second))
            {
                entry->second.Shutdown();
                version->textureSources.erase(entry->first);
                entry = version->textures.erase(entry);
            }
            else
            {
                ++entry;
            }
        }
    }

    std::erase_if(m_liveTextures, [&](const auto& texture)
    {
        return unused(&texture->texture);
    });
}

SavedMaterialRevisionResult FileSceneAssetCatalog::AdvanceSavedMaterialRevision(const RuntimeMaterialVersions& expected,
    const MaterialAssetData& data, const std::string& sourceIdentity, Renderer& renderer,
    const std::function<bool()>& sourceStillCurrent, std::string& error)
{
    error.clear();
    const auto reject = [&](const std::string& message)
    {
        error = message;
        CancelSavedMaterialRevision();
        return SavedMaterialRevisionResult::Rejected;
    };

    try
    {
        const auto current = GetMaterialVersions(expected.projectIdentity, expected.assetID);

        if(!current || current->catalogIdentity != expected.catalogIdentity || current->versions != expected.versions)
        {
            return reject("Runtime material versions changed.");
        }

        if(m_savedRevisionJob && (m_savedRevisionJob->sourceIdentity != sourceIdentity ||
           !m_savedRevisionJob->expected.Matches(expected)))
        {
            CancelSavedMaterialRevision();
        }

        const ProjectPaths paths(expected.projectIdentity);

        if(!m_savedRevisionJob)
        {
            MaterialAssetSerializer::Serialize(data);
            SavedRevisionJob job{expected, data, sourceIdentity, &renderer};
            job.slots.fill(-1);
            const std::optional<std::string> references[] = {data.baseColorTexture, data.normalTexture, data.roughnessTexture,
                data.metallicTexture, data.armTexture, data.aoTexture, data.heightTexture};

            for(size_t slot = 0; slot < 7; ++slot)
            {
                if(!references[slot])
                {
                    continue;
                }

                const auto id = AssetID::ParseReference(*references[slot]);
                const auto* record = id ? m_registry->Find(*id) : nullptr;
                const auto resolved = id ? ProjectAssetResolver::Resolve(paths, *m_registry, *id) : AssetResolution{};

                if(!record || record->type != AssetType::Texture || resolved.status != AssetAvailability::Available)
                {
                    return reject("Saved revision contains a missing, invalid, or unsafe texture reference.");
                }

                const auto interpretation = slot == 0 ? ProjectTextureInterpretation::SRGBColor :
                    slot == 1 ? ProjectTextureInterpretation::NormalMap : ProjectTextureInterpretation::LinearData;
                auto found = std::find_if(job.dependencies.begin(), job.dependencies.end(), [&](const RevisionDependency& entry)
                {
                    return entry.assetID == id->ToString() && entry.interpretation == interpretation;
                });

                if(found == job.dependencies.end())
                {
                    auto source = InspectTextureSource(resolved.resolvedPath);
                    auto* reused = FindReusableTexture(id->ToString(), interpretation, source);
                    job.dependencies.push_back({id->ToString(), interpretation, std::move(source), reused});
                    job.slots[slot] = static_cast<int>(job.dependencies.size() - 1);
                }
                else
                {
                    job.slots[slot] = static_cast<int>(found - job.dependencies.begin());
                }
            }

            m_savedRevisionJob.emplace(std::move(job));
        }

        auto& job = *m_savedRevisionJob;

        for(auto& dependency : job.dependencies)
        {
            if(dependency.reused || dependency.uploaded)
            {
                continue;
            }

            if(dependency.ticket)
            {
                const auto state = renderer.GetPreparedTextureUploadState(dependency.ticket, &error);

                if(!state || *state == PreparedTextureUpload::State::Rejected || *state == PreparedTextureUpload::State::Cancelled ||
                   *state == PreparedTextureUpload::State::Uncertain)
                {
                    return reject("Prepared texture upload failed or became unavailable: " + error);
                }

                if(*state != PreparedTextureUpload::State::Ready)
                {
                    return SavedMaterialRevisionResult::Pending;
                }

                auto uploaded = std::make_unique<LiveTexture>();
                uploaded->assetID = dependency.assetID;
                uploaded->interpretation = dependency.interpretation;
                uploaded->source = dependency.source;

                if(!renderer.TakeCompletedPreparedTextureUpload(dependency.ticket, uploaded->texture))
                {
                    return SavedMaterialRevisionResult::Pending;
                }

                dependency.ticket = 0;
                dependency.uploaded = std::move(uploaded);
                continue;
            }

            if(!dependency.prepared)
            {
                // Multi-frame jobs must resolve containment again before opening a stored path.
                const auto id = AssetID::Parse(dependency.assetID);
                const auto* record = id ? m_registry->Find(*id) : nullptr;
                const auto resolved = id ? ProjectAssetResolver::Resolve(paths, *m_registry, *id) : AssetResolution{};

                if(!record || record->type != AssetType::Texture || resolved.status != AssetAvailability::Available ||
                   resolved.resolvedPath != dependency.source.path)
                {
                    return reject("Texture registration or safe project path changed before preparation.");
                }

                PreparedProjectTexture prepared;

                if(!PrepareProjectTexture(dependency.source.path, dependency.interpretation, prepared, &error) ||
                   dependency.source != InspectTextureSource(dependency.source.path))
                {
                    return reject("Texture preparation failed or source changed: " + error);
                }

                dependency.prepared = std::move(prepared);
            }

            if(!renderer.QueuePreparedTextureUpload(*dependency.prepared, dependency.ticket, error))
            {
                if(error.starts_with("Deferred:"))
                {
                    return SavedMaterialRevisionResult::Pending;
                }

                return reject(error);
            }

            dependency.prepared.reset();
            return SavedMaterialRevisionResult::Pending; // At most one new preparation/queue operation per advance.
        }

        // All uploads completed. Revalidate every source, including reused dependencies.
        for(const auto& dependency : job.dependencies)
        {
            const auto id = AssetID::Parse(dependency.assetID);
            const auto* record = id ? m_registry->Find(*id) : nullptr;
            const auto resolved = id ? ProjectAssetResolver::Resolve(paths, *m_registry, *id) : AssetResolution{};

            if(!record || record->type != AssetType::Texture || resolved.status != AssetAvailability::Available ||
               dependency.source != InspectTextureSource(resolved.resolvedPath))
            {
                return reject("Texture source changed while preparing the saved revision.");
            }
        }

        const auto tableIdentity = renderer.GetMaterialTableIdentity();
        std::vector<GPUMaterial> table;

        if(!renderer.CopyMaterialTableSnapshot(tableIdentity, table, &error))
        {
            return reject(error);
        }

        Texture* slots[7] = {&m_whiteTexture, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

        for(size_t slot = 0; slot < 7; ++slot)
        {
            if(job.slots[slot] >= 0)
            {
                auto& dependency = job.dependencies[static_cast<size_t>(job.slots[slot])];
                slots[slot] = dependency.reused ? dependency.reused : &dependency.uploaded->texture;
            }
        }

        struct Candidate
        {
            Material* target;

            Material value;

            MaterialAssetData* baseline;

            MaterialAssetData source;
        };

        std::vector<Candidate> candidates;
        const MaterialCacheKey key{expected.projectIdentity, expected.assetID.ToString()};

        for(const auto& version : expected.versions)
        {
            auto* target = version.version == 0 ? &m_projectMaterials.at(key) :
                &m_materialVersions.at(static_cast<size_t>(version.version - 1))->material;
            auto* baseline = version.version == 0 ? &m_materialSources.at(key) :
                &m_materialVersions.at(static_cast<size_t>(version.version - 1))->source;

            if(target->gpuIndex != version.gpuIndex || target->gpuIndex >= table.size())
            {
                return reject("Material index changed before saved revision publication.");
            }

            Material candidate = *target;
            MaterialParameterPropagation::Apply(candidate, job.data);
            candidate.texture = slots[0];
            candidate.normalTexture = slots[1];
            candidate.roughnessTexture = slots[2];
            candidate.metallicTexture = slots[3];

            candidate.armTexture = slots[4];
            candidate.aoTexture = slots[5];
            candidate.heightTexture = slots[6];
            table[target->gpuIndex] = candidate.GetGPUData();
            candidates.push_back({target, candidate, baseline, job.data});
        }

        std::vector<Texture*> used;
        std::vector<Texture::Dependency> leases;

        for(auto* material : GetMaterials())
        {
            const auto found = std::find_if(candidates.begin(), candidates.end(),
                [material](const Candidate& entry) { return entry.target == material; });
            const auto& value = found == candidates.end() ? *material : found->value;
            Texture* textures[] = {value.texture, value.normalTexture, value.roughnessTexture, value.metallicTexture,
                value.armTexture, value.aoTexture, value.heightTexture};

            for(auto* texture : textures)
            {
                if(texture)
                {
                    used.push_back(texture);
                    leases.push_back(texture->Retain());
                }
            }
        }

        m_liveTextures.reserve(m_liveTextures.size() + job.dependencies.size());

        if(!sourceStillCurrent())
        {
            return reject("Saved material source changed before publication.");
        }

        if(!renderer.SubmitMaterialTableRevision(tableIdentity, table, std::move(leases), &error))
        {
            if(error.starts_with("Deferred:"))
            {
                return SavedMaterialRevisionResult::Pending;
            }

            return reject(error);
        }

        static_assert(std::is_nothrow_copy_assignable_v<Material>);
        static_assert(std::is_nothrow_move_assignable_v<MaterialAssetData>);

        for(auto& candidate : candidates)
        {
            *candidate.target = candidate.value;
            *candidate.baseline = std::move(candidate.source);
        }

        for(auto& dependency : job.dependencies)
        {
            if(dependency.uploaded)
            {
                m_liveTextures.push_back(std::move(dependency.uploaded));
            }
        }

        m_savedRevisionJob.reset();
        PruneUnusedTextures(used);
        return SavedMaterialRevisionResult::Applied;
    }
    catch(const std::exception& exception)
    {
        return reject(std::string("Saved material revision rejected: ") + exception.what());
    }
}

bool FileSceneAssetCatalog::PublishWorkingMaterialParameters(const RuntimeMaterialVersions& expected,
    const std::optional<MaterialAssetData>& parameters, Renderer& renderer, std::string& error)
{
    error.clear();
    const auto current = GetOwnedMaterialVersions(expected.projectIdentity, expected.assetID, parameters.has_value(), &error);

    if(!current || current->catalogIdentity != expected.catalogIdentity || current->versions != expected.versions)
    {
        error = "Working material identity changed before parameter publication.";
        return false;
    }

    try
    {
        if(parameters)
        {
            MaterialAssetSerializer::Serialize(*parameters);
        }

        const auto identity = renderer.GetMaterialTableIdentity();
        std::vector<GPUMaterial> table;

        if(!renderer.CopyMaterialTableSnapshot(identity, table, &error))
        {
            return false;
        }

        struct Candidate
        {
            Material* target;

            Material value;
        };

        std::vector<Candidate> candidates;
        const MaterialCacheKey key{expected.projectIdentity, expected.assetID.ToString()};

        for(const auto& version : current->versions)
        {
            auto* target = version.version == 0 ? &m_projectMaterials.at(key) :
                &m_materialVersions.at(static_cast<size_t>(version.version - 1))->material;
            const auto& baseline = version.version == 0 ? m_materialSources.at(key) :
                m_materialVersions.at(static_cast<size_t>(version.version - 1))->source;

            if(target->gpuIndex != version.gpuIndex || target->gpuIndex >= table.size())
            {
                error = "Working material GPU index changed.";
                return false;
            }

            Material candidate = *target;
            MaterialParameterPropagation::Apply(candidate, parameters ? *parameters : baseline);
            // Texture pointers, pipeline and GPU index remain exactly those of the live material.
            table[target->gpuIndex] = candidate.GetGPUData();
            candidates.push_back({target, candidate});
        }

        if(!renderer.SubmitMaterialTableSnapshot(identity, table, &error))
        {
            return false;
        }

        for(const auto& candidate : candidates)
        {
            *candidate.target = candidate.value;
        }

        return true;
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}
