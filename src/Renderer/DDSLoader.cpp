#include "Renderer/DDSLoader.h"
#include "Renderer/ResourceUploader.h"
#include "Renderer/DescriptorHeap.h"

#include "Core/Log.h"

#include <limits>
#include <vector>

#include <string>

bool DDSLoader::Load(
    const std::filesystem::path& path,
    DirectX::ScratchImage& image)
{
    image.Release();

    DirectX::TexMetadata metadata{};

    const HRESULT result = DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image);

    if (FAILED(result))
    {
        Log::Error("Failed to load DDS texture: " + path.string() + " HRESULT: " + std::to_string(result));

        return false;
    }

    if (metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || metadata.arraySize != 1 || metadata.depth != 1 ||

        metadata.width == 0 || metadata.height == 0 || metadata.mipLevels == 0)
    {
        Log::Error("Unsupported DDS texture layout: " + path.string());

        image.Release();
        return false;
    }

    return true;
}

bool DDSLoader::LoadTexture(

    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    const std::filesystem::path& path,
    Texture& texture,
    bool srgb)
{
    DirectX::ScratchImage image;

    if (!Load(path, image))
    {
        return false;
    }

    const DirectX::TexMetadata& metadata = image.GetMetadata();

    DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;

    switch (metadata.format)
    {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
        resourceFormat = DXGI_FORMAT_BC1_UNORM;
        srvFormat = srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
        break;
    case DXGI_FORMAT_BC4_UNORM:

        if (srgb)
        {
            Log::Error("BC4 textures cannot use sRGB sampling: " + path.string());
            return false;
        }

        resourceFormat = DXGI_FORMAT_BC4_UNORM;
        srvFormat = DXGI_FORMAT_BC4_UNORM;

        break;

    case DXGI_FORMAT_BC5_UNORM:

        if (srgb)
        {
            Log::Error("BC5 textures cannot use sRGB sampling: " + path.string());
            return false;
        }

        resourceFormat = DXGI_FORMAT_BC5_UNORM;
        srvFormat = DXGI_FORMAT_BC5_UNORM;

        break;

    default:
        Log::Error("Unsupported DDS texture format: " + path.string());
        return false;
    }

    if (metadata.width > (std::numeric_limits<unsigned int>::max)() ||

        metadata.height > (std::numeric_limits<unsigned int>::max)() ||
        metadata.mipLevels > (std::numeric_limits<unsigned int>::max)())
    {
        Log::Error("DDS texture dimensions exceed supported limits: " + path.string());
        return false;
    }

    const unsigned int width = static_cast<unsigned int>(metadata.width);
    const unsigned int height = static_cast<unsigned int>(metadata.height);

    const unsigned int mipLevels = static_cast<unsigned int>(metadata.mipLevels);

    if (mipLevels > Texture::CalculateMipLevels(width, height))
    {
        Log::Error("Invalid DDS mip count: " + path.string());
        return false;
    }

    std::vector<TextureSubresourceData> subresources;
    subresources.reserve(mipLevels);

    for (unsigned int mip = 0; mip < mipLevels; ++mip)
    {
        const DirectX::Image* mipImage = image.GetImage(mip, 0, 0);

        if (!mipImage || !mipImage->pixels || mipImage->rowPitch == 0 || mipImage->slicePitch == 0)
        {
            Log::Error("Invalid DDS mip data: " + path.string());
            return false;
        }

        subresources.push_back({mipImage->pixels, mipImage->rowPitch, mipImage->slicePitch});
    }

    if (!texture.InitializeMipmapped(device, width, height, mipLevels, resourceFormat))
    {
        return false;
    }

    if (!uploader.UploadTexture(texture.GetResource(), subresources.data(),
                                static_cast<unsigned int>(subresources.size())))
    {
        texture.Shutdown();
        return false;
    }

    if (!texture.CreateSRV(device, descriptorHeap, srvFormat))
    {
        texture.Shutdown();
        return false;
    }

    return true;
}
