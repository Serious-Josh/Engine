#pragma once

#include "Renderer/Texture.h"

#include <filesystem>

class ResourceUploader;
class DescriptorHeap;

class EnvironmentMap
{
    public:
        bool Initialize(ID3D12Device* device, const std::filesystem::path& path, ResourceUploader& uploader, DescriptorHeap& descriptorHeap);

        void Shutdown();

        Texture& GetSourceTexture();
        const Texture& GetSourceTexture() const;

        Texture& GetCubemap();
        const Texture& GetCubemap() const;
        

    private:
        Texture m_sourceTexture;
        Texture m_cubemap;
};