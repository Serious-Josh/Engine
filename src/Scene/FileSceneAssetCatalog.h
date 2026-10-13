#pragma once

#include "Scene/SceneAssetResolver.h"
#include "assets/AssetID.h"
#include "assets/MaterialAssetData.h"

#include "Renderer/Material.h"
#include "Renderer/Mesh.h"
#include "Renderer/Pipeline.h"
#include "Renderer/VertexLayout.h"
#include "Renderer/Texture.h"
#include "Renderer/ProjectTextureLoader.h"

#include <filesystem>
#include <functional>
#include <cstdint>
#include <optional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <string_view>
#include <vector>

class ResourceUploader;
class DescriptorHeap;
class Shader;
class Renderer;
class ProjectPaths;
class ProjectAssetRegistry;

enum class SavedMaterialRevisionResult { Pending, Applied, Rejected };

// No borrowed resource pointers: a snapshot remains safe to retain after teardown.
// GPU indices are observations only; requery before using them for future updates.
struct RuntimeMaterialVersions
{
    struct Version
    {
        std::uint64_t version = 0; // 0: ordinary cache; 1+: fresh assignment versions
        unsigned int gpuIndex = 0;
        bool operator==(const Version&) const = default;
    };
    std::filesystem::path projectIdentity;

    AssetID assetID;

    std::uint64_t catalogIdentity = 0;
    std::uint64_t runtimeIdentity = 0; // supplied by the owning FileBackedDemoScene
    std::uint64_t structureRevision = 0;

    std::vector<Version> versions;
    bool Matches(const RuntimeMaterialVersions& current) const
    {
        return projectIdentity == current.projectIdentity && assetID == current.assetID &&
            catalogIdentity == current.catalogIdentity && runtimeIdentity == current.runtimeIdentity &&
            structureRevision == current.structureRevision && versions == current.versions;
    }
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
    // Call outside active recording. Saved content and fresh dependency resources;
    // never changes existing Material objects. Uploads use the checked synchronous uploader.
    // Completed versions remain owned until checked catalog Shutdown (maximum 64 per scene).
    Material* ResolveFreshMaterial(std::string_view reference);

    // Returns a catalog borrow for material attachment. Live revision publication may
    // retire unused wrappers; never retain this raw pointer across that boundary.
    // Texture::Dependency retains GPU allocation/descriptors, not the wrapper itself.
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
    bool OwnsMaterial(const Material& material) const noexcept;
    std::optional<RuntimeMaterialVersions> GetMaterialVersions(
        const std::filesystem::path& canonicalProject, const AssetID& id,
        std::string* errorMessage = nullptr) const;

    // Main-thread pre-recording transaction. Publisher must reject without mutation or
    // accept the complete table; no fallible work follows acceptance. No GPU assets change.
    bool PublishSavedMaterialParameters(const RuntimeMaterialVersions& expected, const MaterialAssetData& data,
        std::vector<GPUMaterial> table, const std::function<bool(const std::vector<GPUMaterial>&)>& publisher,
        std::string* errorMessage);

    SavedMaterialRevisionResult AdvanceSavedMaterialRevision(const RuntimeMaterialVersions& expected,
        const MaterialAssetData& data, const std::string& sourceIdentity, Renderer& renderer,
        const std::function<bool()>& sourceStillCurrent, std::string& error);
    void CancelSavedMaterialRevision();
    // null parameters restores this catalog's last successfully published saved values.
    bool PublishWorkingMaterialParameters(const RuntimeMaterialVersions& expected,
        const std::optional<MaterialAssetData>& parameters, Renderer& renderer, std::string& error);

    bool IsInitialized() const noexcept
    {
        return m_initialized;
    }

private:
    std::optional<RuntimeMaterialVersions> GetOwnedMaterialVersions(const std::filesystem::path& canonicalProject,
        const AssetID& id, bool requireRegistration, std::string* errorMessage) const;
    struct TextureSource
    {
        std::filesystem::path path;

        std::uintmax_t size = 0;

        std::filesystem::file_time_type modified;

        std::string digest;
        bool operator==(const TextureSource&) const = default;
    };
    static TextureSource InspectTextureSource(const std::filesystem::path& path);
    struct MaterialVersion
    {
        Material material;

        MaterialAssetData source;

        std::map<std::pair<std::string, ProjectTextureInterpretation>, TextureSource> textureSources;

        std::filesystem::path projectIdentity;

        std::optional<AssetID> assetID;

        std::map<std::pair<std::string, ProjectTextureInterpretation>, Texture> textures;

        bool published = false;
        ~MaterialVersion();
        void ReleaseTextures();
    };
    struct LiveTexture
    {
        std::string assetID;

        ProjectTextureInterpretation interpretation = ProjectTextureInterpretation::LinearData;

        TextureSource source;

        Texture texture;
        ~LiveTexture()
        {
            texture.Shutdown();
        }
    };
    struct RevisionDependency
    {
        std::string assetID;

        ProjectTextureInterpretation interpretation;

        TextureSource source;

        Texture* reused = nullptr; // Internal catalog borrow, validated before each publication.

        std::optional<PreparedProjectTexture> prepared;

        std::uint64_t ticket = 0;

        std::unique_ptr<LiveTexture> uploaded;
    };
    struct SavedRevisionJob
    {
        RuntimeMaterialVersions expected;

        MaterialAssetData data;

        std::string sourceIdentity;

        Renderer* renderer = nullptr; // Engine-owned; outlives catalog teardown.

        std::vector<RevisionDependency> dependencies;

        std::array<int, 7> slots{}; // -1 denotes the existing empty-slot semantics.
    };
    std::optional<SavedRevisionJob> m_savedRevisionJob;

    std::vector<std::unique_ptr<LiveTexture>> m_liveTextures;
    Texture* FindReusableTexture(const std::string& id, ProjectTextureInterpretation interpretation,
        const TextureSource& source);
    void PruneUnusedTextures(const std::vector<Texture*>& used);
    std::uint64_t m_catalogIdentity = 0;
    static constexpr size_t kMaterialVersionCapacity = 64;
    std::vector<std::unique_ptr<MaterialVersion>> m_materialVersions;
    Material* ResolveMaterialVersion(std::string_view reference, bool fresh);
    std::filesystem::path m_projectRoot;
    const ProjectAssetRegistry* m_registry = nullptr;
    std::string m_materialError;

    using MaterialCacheKey = std::pair<std::filesystem::path, std::string>;

    std::map<MaterialCacheKey, Material> m_projectMaterials;

    std::map<MaterialCacheKey, MaterialAssetData> m_materialSources;

    using TextureCacheKey = std::tuple<std::filesystem::path, std::string, ProjectTextureInterpretation>;

    std::map<TextureCacheKey, Texture> m_projectTextures;

    std::map<TextureCacheKey, TextureSource> m_projectTextureSources;

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
