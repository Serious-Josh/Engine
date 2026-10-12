#pragma once

#include "Scene/SceneAssetResolver.h"

#include "Renderer/Material.h"
#include "Renderer/Mesh.h"
#include "Renderer/Pipeline.h"
#include "Renderer/VertexLayout.h"
#include "Renderer/Texture.h"

#include <filesystem>
#include <map>
#include <string>
#include <tuple>
#include <string_view>
#include <vector>

class ResourceUploader;
class DescriptorHeap;
class Shader;
class ProjectPaths;
class ProjectAssetRegistry;

enum class ProjectTextureInterpretation
{
    SRGBColor,
    LinearData,
    NormalMap
};

class FileSceneAssetCatalog final : public SceneAssetResolver
{
public:
    FileSceneAssetCatalog() = default;

    FileSceneAssetCatalog(const FileSceneAssetCatalog&) = delete;
    FileSceneAssetCatalog& operator=(const FileSceneAssetCatalog&) = delete;

    bool Initialize(
        ID3D12Device* device,
        ResourceUploader& uploader,
        DescriptorHeap& descriptorHeap,
        const Shader* vertexShader,
        const Shader* pixelShader);

    void Shutdown();

    // Configure after Initialize; Engine owns the registry and outlives this catalog.
    void SetProjectAssets(const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    const std::string& GetLastMaterialError() const noexcept
    {
        return m_materialError;
    }

    Mesh* ResolveMesh(std::string_view reference) override;
    Material* ResolveMaterial(std::string_view reference) override;

    // Returns a borrowed, scene-owned texture, cached until Shutdown() without hot reload.
    // Callers may record transitions but must not replace or free its resource/descriptors.
    // Loading is synchronous and must occur outside active command recording.
    // The caller must transition COPY_DEST to the required shader-read state before use.
    Texture* ResolveTexture(
        std::string_view reference,
        ProjectTextureInterpretation interpretation,
        const ProjectPaths& paths,
        const ProjectAssetRegistry& registry,
        std::string* errorMessage = nullptr);

    std::vector<Material*> GetMaterials();

    bool IsInitialized() const noexcept
    {
        return m_initialized;
    }

private:
    std::filesystem::path m_projectRoot;
    const ProjectAssetRegistry* m_registry = nullptr;
    std::string m_materialError;

    using MaterialCacheKey = std::pair<std::filesystem::path, std::string>;
    std::map<MaterialCacheKey, Material> m_projectMaterials;

    using TextureCacheKey = std::tuple<std::filesystem::path, std::string, ProjectTextureInterpretation>;
    std::map<TextureCacheKey, Texture> m_projectTextures;

    // Borrowed for the initialized catalog lifetime; Engine owns these GPU services.
    ID3D12Device* m_device = nullptr;
    ResourceUploader* m_uploader = nullptr;
    DescriptorHeap* m_descriptorHeap = nullptr;

    Mesh m_sphereMesh;
    Texture m_whiteTexture;
    Material m_defaultMaterial;

    bool m_initialized = false;

    VertexLayout m_vertexLayout;
    Pipeline m_pipeline;
};
