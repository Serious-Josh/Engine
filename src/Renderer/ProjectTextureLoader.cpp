#include "Renderer/ProjectTextureLoader.h"
#include "Renderer/ResourceUploader.h"
#include "Renderer/ImageLoader.h"
#include "Renderer/DDSLoader.h"
#include <algorithm>
#include <cctype>
#include <limits>

bool LoadProjectTexture(
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& heap,
    const std::filesystem::path& path,
    ProjectTextureInterpretation interpretation,
    Texture& texture,
    std::string* errorMessage)
{
    const auto fail = [&](const std::string& message)
    {
        if(errorMessage)
        {
            *errorMessage = message;
        }

        return false;
    };

    MipFilterMode filter;

    switch(interpretation)
    {
    case ProjectTextureInterpretation::SRGBColor:
        filter = MipFilterMode::SRGBColor;
        break;
    case ProjectTextureInterpretation::LinearData:
        filter = MipFilterMode::LinearData;
        break;
    case ProjectTextureInterpretation::NormalMap:
        filter = MipFilterMode::NormalMap;
        break;
    default:
        return fail("Unsupported project texture sampling interpretation.");
    }

    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });

    if(extension == ".dds")
    {
        // Preserve DDSLoader's format restrictions and authored mip chain.
        if(!DDSLoader::LoadTexture(device, uploader, heap,
                                   path, texture,
                                   interpretation == ProjectTextureInterpretation::SRGBColor))
        {
            texture.Shutdown();
            return fail("DDS texture load/upload failed: " + path.string() +
                        ". Supported formats are BC1, BC4, and BC5; only BC1 supports sRGB.");
        }
    }
    else if(extension == ".hdr")
    {
        if(interpretation != ProjectTextureInterpretation::LinearData)
        {
            return fail("HDR textures require linear interpretation; sRGB and normal-map HDR loading are unsupported.");
        }

        HDRImageData image;

        if(!ImageLoader::LoadHDR(path, image))
        {
            return fail("Cannot decode HDR texture: " + path.string());
        }

        // Existing HDR decoding is float RGBA and has no CPU float mip generator.
        if(!texture.Initialize(device, image.pixels.data(), image.width, image.height,
                               DXGI_FORMAT_R32G32B32A32_FLOAT, sizeof(float) * 4) ||
           !uploader.UploadTexture(texture.GetResource(), texture.GetInitialData(), texture.GetInitialDataSize()) ||
           !texture.CreateSRV(device, heap))
        {
            texture.Shutdown();
            return fail("Failed to create/upload HDR texture or allocate its SRV.");
        }
    }
    else
    {
        const bool commonImage = extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga";
        const bool additionalImage = extension == ".bmp" || extension == ".psd" || extension == ".gif" || extension == ".pic";
        const bool portableImage = extension == ".pnm" || extension == ".ppm" || extension == ".pgm";

        if(!commonImage && !additionalImage && !portableImage)
        {
            return fail("Unsupported project texture extension: " + extension);
        }

        ImageData image;
        ImageMipChain mipChain;

        if(!ImageLoader::Load(path, image) ||
           !ImageLoader::GenerateMipChain(image, filter, mipChain))
        {
            return fail("Cannot decode texture or generate its mip chain: " + path.string());
        }

        if(mipChain.levels.size() > (std::numeric_limits<unsigned int>::max)())
        {
            return fail("Texture mip count exceeds the supported limit.");
        }

        std::vector<TextureSubresourceData> subresources;
        subresources.reserve(mipChain.levels.size());

        for(const auto& level : mipChain.levels)
        {
            subresources.push_back({level.pixels.data(), static_cast<size_t>(level.width) * 4, level.pixels.size()});
        }

        const auto mipCount = static_cast<unsigned int>(subresources.size());
        const DXGI_FORMAT srvFormat = interpretation == ProjectTextureInterpretation::SRGBColor
            ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;

        if(!texture.InitializeMipmapped(device, image.width, image.height, mipCount, DXGI_FORMAT_R8G8B8A8_UNORM) ||
           !uploader.UploadTexture(texture.GetResource(), subresources.data(), mipCount) ||
           !texture.CreateSRV(device, heap, srvFormat))
        {
            texture.Shutdown();
            return fail("Failed to create/upload image texture or allocate its SRV.");
        }
    }

    return true;
}
