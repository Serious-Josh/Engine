
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

    const std::string& GetDisplayName() const noexcept;
    bool Save(std::string* errorMessage = nullptr);
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

    FileSceneAssetCatalog m_assets;
    FileSceneRuntime m_runtime;

    bool m_initialized = false;
};
