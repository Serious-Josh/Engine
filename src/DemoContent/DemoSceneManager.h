#pragma once

#include "DemoContent/IDemoScene.h"
#include "DemoContent/SceneRegistry.h"

#include <filesystem>
#include <string>
#include <memory>
#include <span>
#include <string_view>

class SphereGridScene;
class ProjectAssetRegistry;

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

    void Shutdown();

    // Engine owns the live registry; scene instances only borrow it.
    void SetProjectAssets(const std::filesystem::path& root, const ProjectAssetRegistry& registry);

    bool RequestScene(std::string_view sceneName);

    bool RequestFileScene(
        const std::filesystem::path& path,
        std::string* errorMessage = nullptr);

    // Call on the Engine thread outside Renderer command recording. Uploads complete
    // through ResourceUploader before success; no active-scene resources are changed.
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

    std::unique_ptr<IDemoScene> m_activeScene;
    std::unique_ptr<IDemoScene> m_stagedFileScene;

    std::filesystem::path m_requestedFilePath;
    std::string m_stagedFileName;
    std::string m_activeFileName;

    const DemoSceneEntry* m_activeSceneEntry = nullptr;
    const DemoSceneEntry* m_requestedSceneEntry = nullptr;

    float m_aspectRatio = 1.0f;

    bool m_hasPendingSceneChange = false;
};
