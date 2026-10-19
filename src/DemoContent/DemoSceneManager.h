#pragma once

#include "DemoContent/IDemoScene.h"
#include "DemoContent/SceneRegistry.h"
#include "Scene/SceneDocument.h"

#include <filesystem>
#include <string>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

class SphereGridScene;
class ProjectAssetRegistry;
class ProjectMeshCPUCache;
namespace CookedScene { struct ArchiveScene; }

class DemoSceneManager
{
    public:
    bool Initialize(
      std::string_view sceneName,
      ID3D12Device* device,
      ResourceUploader& uploader,
      DescriptorHeap& descriptorHeap,
      const Shader* vertexShader,
      const Shader* pixelShader,
      float aspectRatio);

    bool InitializeArchive(std::shared_ptr<const CookedScene::ArchiveScene>, ID3D12Device*, ResourceUploader&, DescriptorHeap&, const Shader*, const Shader*, float);

    // Initial scene for a project: a never-saved document with no file. Requires project assets.
    bool InitializeUnsaved(
        SceneDocument document,
        ID3D12Device* device,
        ResourceUploader& uploader,
        DescriptorHeap& descriptorHeap,
        const Shader* vertexShader,
        const Shader* pixelShader,
        float aspectRatio);

    void Shutdown();

    // Engine owns the live registry; scene instances only borrow it.
    void SetProjectAssets(const std::filesystem::path& root, const ProjectAssetRegistry& registry);
    void SetMeshCPUCache(ProjectMeshCPUCache* cache) noexcept { m_meshCPUCache = cache; }
    void SetTextureStreamingEnabled(bool enabled) noexcept { m_textureStreamingEnabled = enabled; }

    bool RequestScene(std::string_view sceneName);

    // Explicit packaged diagnostic source; no filesystem scene path.
    bool RequestArchiveScene(std::shared_ptr<const CookedScene::ArchiveScene> source, std::string* errorMessage = nullptr);
    bool HasPendingFileSceneRequest() const noexcept
    {
        return !m_requestedFilePath.empty() || bool(m_requestedArchive) || m_requestedUnsavedDocument.has_value();
    }

    // Replaces the active scene with a never-saved document through the same staging, GPU
    // drain/release and commit path as opening a file. Writes nothing.
    bool RequestUnsavedScene(SceneDocument document, std::string* errorMessage = nullptr);
    bool RequestFileScene(
        const std::filesystem::path& path,
        std::string* errorMessage = nullptr);

    // Explicit reload uses the same metadata-first policy as ordinary file opening.
    bool RequestMetadataOnlyFileScene(const std::filesystem::path& path, std::string* errorMessage = nullptr);

#ifdef _DEBUG
    bool RequestSynchronousFileSceneDiagnostic(const std::filesystem::path& path, std::string* errorMessage = nullptr);
#endif

    // Call on the Engine thread outside Renderer command recording. Ordinary
    // staging prepares metadata; no active-scene resources are changed.
    bool StageRequestedFileScene(
        ID3D12Device* device,
        ResourceUploader& uploader,
        DescriptorHeap& descriptorHeap,
        const Shader* vertexShader,
        const Shader* pixelShader,
        std::string* errorMessage = nullptr);

    // Borrowed access only. Staging never initializes Renderer scene resources.
    IDemoScene* GetStagedFileScene() noexcept;
    const IDemoScene* GetStagedFileScene() const noexcept;
    std::string_view GetStagedFileSceneName() const noexcept;
    const std::filesystem::path& GetRequestedFileScenePath() const noexcept;

    // Caller must first resolve GPU use and release Renderer references to the old scene.
    // No Renderer initialization or rollback is performed here.
    bool CommitStagedFileScene(std::string* errorMessage = nullptr);

    // Only while Renderer has never acquired references to the staged candidate.
    void CancelFileSceneRequest();

    bool HasPendingSceneChange() const;

    bool ApplyPendingSceneChange(
        ID3D12Device* device,
        ResourceUploader& uploader,
        DescriptorHeap& descriptorHeap,
        const Shader* vertexShader,
        const Shader* pixelShader);

    void SetAspectRatio(float aspectRatio);

    IDemoScene& GetActiveScene();
    const IDemoScene& GetActiveScene() const;

    IDemoScene* TryGetActiveScene() noexcept;
    const IDemoScene* TryGetActiveScene() const noexcept;

    SphereGridScene* GetActiveSphereGridScene();

    std::span<const DemoSceneEntry> GetAvailableScenes() const;
    std::string_view GetActiveSceneName() const;

    private:
    void ConfigureProjectAssets(IDemoScene& scene) const;

    std::filesystem::path m_projectRoot;
    const ProjectAssetRegistry* m_registry = nullptr;
    ProjectMeshCPUCache* m_meshCPUCache = nullptr;

    std::unique_ptr<IDemoScene> m_activeScene;
    std::unique_ptr<IDemoScene> m_stagedFileScene;

    std::shared_ptr<const CookedScene::ArchiveScene> m_requestedArchive;
    std::filesystem::path m_requestedFilePath;
    std::optional<SceneDocument> m_requestedUnsavedDocument;
    std::string m_stagedFileName;
    std::string m_activeFileName;

    const DemoSceneEntry* m_activeSceneEntry = nullptr;
    const DemoSceneEntry* m_requestedSceneEntry = nullptr;

    float m_aspectRatio = 1.0f;

    bool m_hasPendingSceneChange = false;
    bool m_textureStreamingEnabled = false;
    bool m_requestedMetadataOnly = false;
};
