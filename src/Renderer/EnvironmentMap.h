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

        Texture& GetDiffuseIrradiance();
        const Texture& GetDiffuseIrradiance() const;

        Texture& GetGGXMultiscatterIrradiance();
        const Texture& GetGGXMultiscatterIrradiance() const;

    private:
        Texture m_sourceTexture;
        Texture m_cubemap;
        Texture m_diffuseIrradiance;
        Texture m_ggxMultiscatterIrradiance;
};