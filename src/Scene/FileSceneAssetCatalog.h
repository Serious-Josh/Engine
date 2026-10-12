#pragma once

#include "Scene/SceneAssetResolver.h"

#include "Renderer/Material.h"
#include "Renderer/Mesh.h"
#include "Renderer/Pipeline.h"
#include "Renderer/VertexLayout.h"
#include "Renderer/Texture.h"

#include <string_view>
#include <vector>

class ResourceUploader;
class DescriptorHeap;
class Shader;

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

    Mesh* ResolveMesh(std::string_view reference) override;
    Material* ResolveMaterial(std::string_view reference) override;

    std::vector<Material*> GetMaterials();

    bool IsInitialized() const noexcept
    {
        return m_initialized;
    }

private:
    Mesh m_sphereMesh;
    Texture m_whiteTexture;
    Material m_defaultMaterial;

    bool m_initialized = false;

    VertexLayout m_vertexLayout;
    Pipeline m_pipeline;
};