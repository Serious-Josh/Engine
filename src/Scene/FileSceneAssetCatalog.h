#pragma once

#include "assets/ProjectMeshPreparationService.h"

#include "Scene/SceneAssetResolver.h"
#include "Scene/SceneObjectResolver.h"
#include "assets/AssetID.h"
#include "assets/MaterialAssetData.h"
#include "assets/ImportedMesh.h"
#include "assets/ModelImporter.h"
#include "assets/ProjectMeshCPUCache.h"
#include "Scene/MeshResidency.h"

#include "Renderer/Material.h"
#include "Renderer/Mesh.h"
#include "Renderer/Caustics/CausticGeometrySourceManifest.h"
#include "Renderer/Pipeline.h"
#include "Renderer/VertexLayout.h"
#include "Renderer/Texture.h"
#include "Renderer/ProjectTextureLoader.h"
#include "assets/ProjectTexturePreparationService.h"

#include <array>
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
#include <chrono>
#include <thread>
#include <unordered_map>

class Scene;
struct PreparedScene;
class FileSceneRuntime;

class ResourceUploader;
class DescriptorHeap;
class Shader;
class Renderer;
class ProjectPaths;
class ProjectAssetRegistry;
namespace CookedScene { struct ArchiveScene; }

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

    // Caller must release Renderer references and prove GPU/recording completion first.
    void Shutdown();

    // Configure after Initialize; Engine owns the registry and outlives this catalog.
    void SetArchiveScene(std::shared_ptr<const CookedScene::ArchiveScene> source);
    bool ValidateArchiveMesh(std::string_view reference, const CookedMeshSource::AssetEvidence* evidence,
        const CookedMesh::MetadataSnapshot* metadata, std::string* error) const;
    bool IsArchiveActivation() const noexcept { return bool(m_archiveScene); }
    const std::shared_ptr<const CookedScene::ArchiveScene>& ArchiveSource() const noexcept { return m_archiveScene; }
    void SetProjectAssets(const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    void SetMeshCPUCache(ProjectMeshCPUCache* cache) noexcept { m_meshCPUCache = cache; }
    void SetTextureStreamingEnabled(bool enabled) noexcept { m_textureStreamingEnabled = enabled; }
    void BeginTextureStreaming();
    // Active scene only, on the Engine thread before command recording.
    void AdvanceTextureStreaming(ProjectTexturePreparationService& preparation, Renderer& renderer,
        std::uint64_t runtimeIdentity, const Scene& scene,
        const std::function<bool(const AssetID&, const MaterialAssetData&)>& mayPublish);
    void CancelTextureStreaming();
    const std::string& GetLastMaterialError() const noexcept
    {
        return m_materialError;
    }

    enum class MeshReferenceKind { RegisteredAsset, Builtin, UnknownBuiltin, Invalid };
    static MeshReferenceKind ClassifyMeshReference(std::string_view reference);

    // Synchronous loading outside active recording. Borrows remain stable until Shutdown.
    // Changed source content requires a scene reload; live geometry is never replaced.
    Mesh* ResolveMesh(std::string_view reference) override;
    // Editor mutation only: reuses upload-complete catalog geometry, never imports.
    bool PrepareEditorObject(const SceneObjectDocument& authored, ResolvedSceneObject& object,
        std::string* errorMessage);
    Mesh* FindRetainedMesh(std::string_view reference);
    const MeshMetadata* FindRetainedSlotMetadata(std::string_view reference) const;

    // The 1x1 opaque white fallback base texture: uploaded once by Initialize, never written
    // afterwards and destroyed only by Shutdown. Identity for caustic texture admission only.
    const Texture* GetImmutableFallbackTexture() const noexcept
    {
        return m_initialized ? &m_whiteTexture : nullptr;
    }
    Material* FindRetainedMaterial(std::string_view reference);
    // Existing renderer identity only; includes retained Inspector assignment versions.
    // Never resolves files, uploads resources, or admits a new table entry.
    Material* FindPublishedMaterial(std::string_view reference, const Renderer& renderer);
    bool ValidateEditorBindings(const SceneDocument& document, PreparedScene& prepared,
        std::string* errorMessage);
    // Main-thread only, outside renderer recording. Revalidates source then
    // synchronously uploads immutable worker buffers; never imports FBX.
    Mesh* ResolvePreparedMesh(std::string_view reference,
        const ProjectMeshPreparationService::Completion& completion, std::string* errorMessage);
    struct CookedPreparationResult
    {
        ProjectMeshPreparationService::Status status = ProjectMeshPreparationService::Status::Invalid;
        enum class Failure { None, Completion, Metadata, Binding, GPU };
        Failure failure = Failure::None;
        std::string error;
        Mesh* mesh = nullptr; // Catalog borrow, only on successful GPU preparation.
        std::optional<MeshResidencyCandidate> candidate;
        std::shared_ptr<const CookedMeshResidencyEvidence> evidence;
    };
    // Explicit development selection only; never called by ordinary mesh lookup.
    // Owner thread, outside recording. Uses the existing synchronous uploader.
    CookedPreparationResult ResolvePreparedCookedMesh(std::string_view reference,
        const ProjectMeshPreparationService::Completion& completion, const ProjectMeshPreparationService& preparation,
        const Renderer& renderer, const FileSceneRuntime& runtime);
    CookedPreparationResult RetainPreparedCookedMetadata(std::string_view reference,
        const ProjectMeshPreparationService::Completion& completion, const ProjectMeshPreparationService& preparation);
    // Existing upload-complete project mesh only; never imports or uploads.
    std::optional<MeshResidencyCandidate> DescribeMeshResidencyCandidate(std::string_view reference,
        const Mesh& mesh, std::string* errorMessage = nullptr) const;
    MeshResidencySourceLock ValidateMeshResidencyCandidate(const MeshResidencyCandidate& candidate,
        std::string_view reference, std::string* errorMessage = nullptr) const;
    // Validation only: never loads material files/textures or submits GPU work.
    bool ValidateRetainedResidencyMaterial(std::string_view reference, const Material* material,
        std::string* errorMessage = nullptr) const;
    Material* ResolveResidencyMaterial(std::string_view reference, Material* retained,
        std::string* errorMessage = nullptr);
    // Reference/type/canonical containment checks only. Does not hash, import,
    // consult the CPU mesh cache, or allocate a project GPU mesh.
    bool ValidateMetadataMeshReference(std::string_view reference, std::string* errorMessage) const;
    bool ValidateMetadataMaterialReference(std::string_view reference, std::string* errorMessage) const;

    // One synchronous validation pass on the constructing thread. While open, equal
    // material references are validated once and an FBX source digest is computed
    // once per resolved path; the pass holds a share-read pin on that path, taken
    // before the digest, so the file cannot change while its digest is reused.
    // Nothing survives the scope: the next pass validates against disk again.
    class ValidationPass
    {
    public:
        explicit ValidationPass(const FileSceneAssetCatalog& catalog);
        ~ValidationPass();
        ValidationPass(const ValidationPass&) = delete;
        ValidationPass& operator=(const ValidationPass&) = delete;

    private:
        const FileSceneAssetCatalog& m_catalog;
        bool m_outermost = false;
    };
    // Requires the existing fallback-texture policy, so metadata preparation cannot
    // accidentally enter synchronous texture decode/upload through a direct caller.
    Material* ResolveMetadataMaterial(std::string_view reference);
    const std::string& GetLastMeshError() const noexcept
    {
        return m_meshError;
    }

    // Registered meshes and builtins. CPU geometry shares the catalog borrow lifetime.
    const ImportedMesh* GetMeshGeometry(const Mesh& mesh) const noexcept;
    enum class StaticSourceAdmissionStatus
    {
        Disabled, Configured, Ready, NotSelected, InvalidSource, LateConfiguration,
        NondefaultMetadata, InvalidMetadata, CapacityExceeded, PreparationFailed
    };
    struct StaticSourceAdmissionDiagnostic
    {
        StaticSourceAdmissionStatus status = StaticSourceAdmissionStatus::Disabled;
        CausticContracts::Geometry::AdmissionResult preparation{CausticContracts::Geometry::AdmissionStatus::Ready};
    };
    // Owner thread, before first GPU creation. Only full, validated cooked
    // completions are supported; fixtureID is provenance, never asset identity.
    StaticSourceAdmissionStatus ConfigureStaticSourceAdmission(
        const ProjectMeshPreparationService::Completion& completion,
        const ProjectMeshPreparationService& preparation,
        std::uint32_t firstPrimitive, std::uint32_t primitiveCount, std::uint64_t fixtureID);
    StaticSourceAdmissionDiagnostic GetLastStaticSourceAdmission() const noexcept
    {
        return m_staticSourceDiagnostic;
    }
    // Upper bound on live static-source generations charged to one catalog. Each costs at most
    // 638,976 B of source plus 2 MiB of resources, so the aggregate is bounded near 21 MiB.
    static constexpr std::size_t MaxStaticSources = 8;
    enum class StaticSourceRequestStatus { Ready, Cleared, EmptyAssetID, Duplicate, Overflow };
    // Scene caustic registration opt-in, before residency: the first FBX-imported GPU creation of each
    // requested asset ID is admitted as an immutable static source when every triangle certifies.
    // Admission is independent per asset; a refused asset keeps ordinary geometry and never affects the
    // others. Empty clears. Invalid sets are refused whole and clear the request. Never changes
    // geometry already created.
    StaticSourceRequestStatus RequestStaticSourceAssets(std::vector<std::string> assetIDs);
    void RequestStaticSourceAsset(std::string assetID)
    {
        std::vector<std::string> assetIDs;
        if(!assetID.empty())
            assetIDs.push_back(std::move(assetID));
        RequestStaticSourceAssets(std::move(assetIDs));
    }
    const std::vector<std::string>& GetStaticSourceAssetRequests() const noexcept { return m_staticSourceAssetRequests; }
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
    friend class RayTracingRecordingDiagnostic;
    friend class SceneMaterialBindingPublication;
#if defined(ENGINE_HAS_VERIFICATION)
    friend class PrimitiveCreationVerification;
    friend class SceneHierarchyVerification;
#endif
    friend struct MaterialAdmissionState;
    friend class Renderer;
    std::map<std::string, std::string> m_admittedMaterialDigests;
    Mesh* CreateProjectMesh(const ProjectMeshCPUCache::Key& key,
        std::shared_ptr<const ImportedMesh> geometry,
        std::shared_ptr<const CookedMeshResidencyEvidence> cooked = {});
    struct MeshAsset
    {
        std::shared_ptr<const ImportedMesh> geometry;
        std::shared_ptr<const CookedMeshResidencyEvidence> cooked;

        Mesh mesh;

        bool published = false;
        ~MeshAsset();
    };

    // Path is intentionally absent: a rename with unchanged bytes preserves the borrow.
    using MeshCacheKey = ProjectMeshCPUCache::Key;
    static constexpr std::uint32_t kMeshImportRevision = ModelImporter::kImportRevision;
    std::map<MeshCacheKey, std::unique_ptr<MeshAsset>> m_projectMeshes;
    std::map<MeshCacheKey, std::shared_ptr<const CookedMeshResidencyEvidence>> m_cookedMetadata;

    ProjectMeshCPUCache* m_meshCPUCache = nullptr; // Engine-owned; CPU-only, outlives catalogs.

    std::string m_meshError;

    struct StaticSourceSelection
    {
        MeshCacheKey key;
        std::weak_ptr<const ImportedMesh> geometry;
        std::shared_ptr<const CookedMeshResidencyEvidence> cooked;
        std::uint64_t catalogIdentity = 0;
        std::uint32_t firstPrimitive = 0;
        std::uint32_t primitiveCount = 0;
        std::uint64_t fixtureID = 0;
    };
    bool ValidateStaticSourceSelection(const StaticSourceSelection& selection) const;
    std::optional<StaticSourceSelection> m_staticSourceSelection;
    StaticSourceAdmissionDiagnostic m_staticSourceDiagnostic;
    // Sorted and unique, so admission order and fixture provenance never depend on request order.
    std::vector<std::string> m_staticSourceAssetRequests;
    // Deliberately survive Shutdown/Initialize and selection changes. Historical external
    // owners still consume their generation slot on this catalog.
    struct AdmittedStaticSource
    {
        std::string assetID;
        std::weak_ptr<const CausticContracts::Geometry::MeshSourceGeneration> generation;
    };
    std::array<AdmittedStaticSource, MaxStaticSources> m_admittedSources;
    bool HasLiveStaticSource() const noexcept;
    bool HasLiveStaticSource(const std::string& assetID) const noexcept;
    AdmittedStaticSource* FindFreeStaticSourceSlot() noexcept;

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
    std::shared_ptr<const CookedScene::ArchiveScene> m_archiveScene;
    std::filesystem::path m_projectRoot;
    const ProjectAssetRegistry* m_registry = nullptr;
    std::string m_materialError;

    using MaterialCacheKey = std::pair<std::filesystem::path, std::string>;

    std::map<MaterialCacheKey, Material> m_projectMaterials;

    std::map<MaterialCacheKey, MaterialAssetData> m_materialSources;

    struct ValidationPassState
    {
        struct MaterialResult
        {
            bool valid = false;
            std::string error;
        };

        struct PinnedSource
        {
            MeshResidencySourceLock pin; // Acquired before the digest was computed.
            std::string digest;
        };

        std::thread::id owner;
        std::unordered_map<std::string, MaterialResult> materials; // Exact authored reference.
        std::map<std::filesystem::path, PinnedSource> sources; // Resolved FBX path.
    };

    mutable std::optional<ValidationPassState> m_validationPass;
    ValidationPassState* ActiveValidationPass() const noexcept;
    bool ValidateMetadataMaterialReferenceOnDisk(std::string_view reference, std::string* errorMessage) const;

    // Ordinary material binding revisions change only on saved texture publication.
    std::map<MaterialCacheKey, std::uint64_t> m_materialBindingRevisions;

    using TextureCacheKey = std::tuple<std::filesystem::path, std::string, ProjectTextureInterpretation>;

    struct StreamingBinding
    {
        MaterialCacheKey material;
        size_t slot = 0;
        std::uint64_t revision = 0;
    };
    struct StreamingTexture
    {
        enum class Phase { Requested, Preparing, CPUReady, Uploading, Ready, Resident, Failed, Superseded };
        Phase phase = Phase::Requested;

        std::vector<StreamingBinding> bindings;

        std::optional<ProjectTexturePreparationService::Completion> completion;

        std::unique_ptr<LiveTexture> uploaded;

        std::uint64_t cpuTicket = 0;
        std::uint64_t gpuTicket = 0;

        std::chrono::steady_clock::time_point retryAfter;
    };
    struct StreamingStatistics
    {
        size_t submitted = 0;
        size_t deduplicated = 0;
        size_t deferred = 0;
        size_t cpuCompleted = 0;
        size_t cpuFailed = 0;
        size_t gpuCompleted = 0;
        size_t failed = 0;
        size_t publications = 0;
        size_t diagnostics = 0;
    };
    static constexpr size_t kStreamingTextureCapacity = 256;
    std::map<TextureCacheKey, StreamingTexture> m_streamingTextures;
    StreamingStatistics m_streamingStatistics;

    std::chrono::steady_clock::time_point m_streamingActivated;

    ProjectTexturePreparationService* m_texturePreparation = nullptr;
    Renderer* m_textureRenderer = nullptr;

    std::uint64_t m_streamingRuntime = 0;
    std::uint64_t m_streamingTable = 0;
    std::uint64_t m_streamingGeneration = 0;

    size_t m_streamingCursor = 0;

    bool m_textureStreamingEnabled = false;
    bool m_streamingReported = false;

    void QueueMaterialTextures(const MaterialCacheKey& key, const MaterialAssetData& data);
    bool IsCurrentStreamingBinding(const StreamingBinding& binding, const TextureCacheKey& texture,
        const Scene& scene) const;
    void ResetStreamingTexture(StreamingTexture& texture);
    void FailStreamingTexture(StreamingTexture& texture, const std::string& error);
    TextureSource ValidateStreamingSource(const TextureCacheKey& key, const StreamingTexture& texture) const;
    void AdvanceStreamingTexture(const TextureCacheKey& key, StreamingTexture& texture, const Scene& scene,
        const std::function<bool(const AssetID&, const MaterialAssetData&)>& mayPublish);
    bool PublishStreamingTexture(const TextureCacheKey& key, StreamingTexture& texture, const Scene& scene,
        const std::function<bool(const AssetID&, const MaterialAssetData&)>& mayPublish);
    void ReportTextureStreaming();

    std::map<TextureCacheKey, Texture> m_projectTextures;

    std::map<TextureCacheKey, TextureSource> m_projectTextureSources;

    // Borrowed for the initialized catalog lifetime; Engine owns these GPU services.
    ID3D12Device* m_device = nullptr;

    ResourceUploader* m_uploader = nullptr;

    DescriptorHeap* m_descriptorHeap = nullptr;

    Mesh m_sphereMesh;
    ImportedMesh m_sphereGeometry;

    // builtin:triangle: one axis-aligned dyadic triangle, initialized as an immutable static
    // source so the accepted caustic domain can be authored with ordinary scene references.
    // Owns its own generation; it never consumes an m_admittedSources slot.
    Mesh m_triangleMesh;
    ImportedMesh m_triangleGeometry;
    const Mesh* FindBuiltinMesh(std::string_view reference) const noexcept;

    Texture m_whiteTexture;

    Material m_defaultMaterial;

    bool m_initialized = false;

    VertexLayout m_vertexLayout;

    Pipeline m_pipeline;
};
