#pragma once

#include "Renderer/Texture.h"
#include "assets/ProjectAssetResolver.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

class ResourceUploader;
class DescriptorHeap;

class TextureThumbnailCache
{
public:
    struct Preview
    {
        Texture texture;
        unsigned int width = 0;
        unsigned int height = 0;
        std::string error;

        std::uint64_t lastUse = 0;
        std::uint64_t lastSubmissionFence = 0;
        bool awaitingSubmission = false;
        bool invalidated = false;
        std::filesystem::path sourcePath;
    };

    void Configure(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& heap);
    void BeginFrame();
    void Invalidate(const std::filesystem::path& path);
    // Completion belongs to the Renderer command-context lifetime. Clear before reconfiguration.
    void SetCompletedFence(std::uint64_t completedFence);
    void ConfirmSubmission(std::uint64_t submissionFence);
    const Preview* FindOrCreate(const ProjectPaths& paths, const ProjectAssetRegistry& registry, const AssetRecord& record);
    std::vector<Texture*> GetTextures();
    D3D12_GPU_DESCRIPTOR_HANDLE GetHandle(const Preview& preview) const;
    // Caller has drained the render queue, before constructing any new ImGui commands.
    void ClearAfterGPUDrain();

private:
    ID3D12Device* m_device = nullptr;
    ResourceUploader* m_uploader = nullptr;
    DescriptorHeap* m_heap = nullptr;

    std::map<std::pair<std::filesystem::path, std::string>, Preview> m_previews;
    bool m_createdThisFrame = false;
    std::uint64_t m_useSerial = 0;
    std::uint64_t m_completedFence = 0;
};
