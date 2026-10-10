#pragma once

#include <DirectXTex.h>
#include <filesystem>

#include "Renderer/Texture.h"

class ResourceUploader;
class DescriptorHeap;

class DDSLoader
{
    public:
        static bool Load(
            const std::filesystem::path& path,
            DirectX::ScratchImage& image);

        static bool LoadTexture(
            ID3D12Device* device,
            ResourceUploader& uploader,
            DescriptorHeap& descriptorHeap,
            const std::filesystem::path& path,
            Texture& texture,
            bool srgb);
};