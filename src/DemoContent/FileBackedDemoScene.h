
#pragma once

#include "DemoContent/IDemoScene.h"
#include "Scene/FileSceneRuntime.h"
#include "Scene/FileSceneAssetCatalog.h"

#include <filesystem>
#include <string>
#include <vector>

class FileBackedDemoScene final : public IDemoScene
{
public:
    explicit FileBackedDemoScene(std::filesystem::path scenePath);

    FileBackedDemoScene(const FileBackedDemoScene&) = delete;
    FileBackedDemoScene& operator=(const FileBackedDemoScene&) = delete;

    bool Initialize(
        ID3D12Device* device,
        ResourceUploader& uploader,
        DescriptorHeap& descriptorHeap,
        const Shader* vertexShader,
        const Shader* pixelShader) override;

    void SetProjectAssets(const std::filesystem::path& root, const ProjectAssetRegistry& registry);

    const std::filesystem::path& GetScenePath() const noexcept
    {
        return m_scenePath;
    }

    bool PublishSavedMaterialParameters(const RuntimeMaterialVersions& expected, const MaterialAssetData& data,
        std::vector<GPUMaterial> table, const std::function<bool(const std::vector<GPUMaterial>&)>& publisher,
        std::string* errorMessage);
    SavedMaterialRevisionResult AdvanceSavedMaterialRevision(const RuntimeMaterialVersions& expected,
        const MaterialAssetData& data, const std::string& sourceIdentity, Renderer& renderer,
        const std::function<bool()>& sourceStillCurrent, std::string& error);
    void CancelSavedMaterialRevision();
    bool PublishWorkingMaterialParameters(const RuntimeMaterialVersions& expected,
        const std::optional<MaterialAssetData>& parameters, Renderer& renderer, std::string& error);
    const std::string& GetDisplayName() const noexcept;
    bool Save(std::string* errorMessage = nullptr);
    std::uint64_t GetRuntimeIdentity() const noexcept
    {
        return m_runtime.GetRuntimeIdentity();
    }

    std::optional<RuntimeMaterialVersions> GetMaterialVersions(
        const std::filesystem::path& canonicalProject, const AssetID& id,
        std::string* errorMessage = nullptr) const;

    std::optional<SceneObjectMetadata> GetObjectMetadata(
        RenderObjectID runtimeIndex,
        std::string* errorMessage = nullptr) const;
    std::optional<MaterialAssignmentToken> PrepareMaterialAssignment(
        const SceneObjectMetadata& selection, std::string materialReference,
        std::string* errorMessage = nullptr);
    // Load outside active recording; completed uploads remain owned by this scene.
    Material* ResolveAssignmentMaterial(std::string_view reference, std::string* errorMessage = nullptr);
    // Engine must establish Renderer/GPU safety first and retain old/new catalog assets.
    bool CommitMaterialAssignment(const MaterialAssignmentToken& token, Material& sceneOwnedMaterial,
        std::string* errorMessage = nullptr);
    bool RollbackMaterialAssignment(const MaterialAssignmentToken& token, std::string* errorMessage = nullptr);
    bool FinishMaterialAssignment(const MaterialAssignmentToken& token, std::string* errorMessage = nullptr);
    bool CancelMaterialAssignment(const MaterialAssignmentToken& token, std::string* errorMessage = nullptr);

    bool CheckUnsavedChanges(bool& dirty, std::string* errorMessage = nullptr) const;

    void Shutdown() override;
    void Update(float deltaTime) override;

    Scene& GetScene() override;
    const Scene& GetScene() const override;

    std::vector<Material*> GetMaterials() override;

    void ResetCamera() override;
    void ResetDirectionalLight() override;
    void SetAspectRatio(float aspectRatio) override;

private:
    std::filesystem::path m_scenePath;
    std::filesystem::path m_projectRoot;
    const ProjectAssetRegistry* m_registry = nullptr;

    FileSceneAssetCatalog m_assets;

    FileSceneRuntime m_runtime;

    bool m_initialized = false;
};
