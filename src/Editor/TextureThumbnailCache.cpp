#include "TextureThumbnailCache.h"
#include "Renderer/ImageLoader.h"
#include "Renderer/DDSLoader.h"
#include "Renderer/ResourceUploader.h"
#include "Renderer/DescriptorHeap.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <stdexcept>

void TextureThumbnailCache::Configure(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& heap)
{
    m_device = device;
    m_uploader = &uploader;
    m_heap = &heap;
}

void TextureThumbnailCache::BeginFrame()
{
    m_createdThisFrame = false;
}

const TextureThumbnailCache::Preview* TextureThumbnailCache::FindOrCreate(
    const ProjectPaths& paths,
    const ProjectAssetRegistry& registry,
    const AssetRecord& record)
{
    const auto key = std::make_pair(paths.GetRoot().lexically_normal(), record.id.ToReference() + ":" + record.relativePath.generic_string());
    const auto existing = m_previews.find(key);

    if(existing != m_previews.end())
    {
        return &existing->second;
    }

    // Bounded GPU memory/descriptors and one decode/upload per frame.
    if(m_previews.size() >= 64)
    {
        // Retire at the next checked Engine boundary, never while this frame's
        // ImGui draw commands or an earlier submission can reference the cache.
        m_retirementRequested = true;
        return nullptr;
    }

    if(!m_device || m_createdThisFrame)
    {
        return nullptr;
    }

    m_createdThisFrame = true;
    auto& preview = m_previews.try_emplace(key).first->second;

    try
    {
        const auto resolved = ProjectAssetResolver::Resolve(paths, registry, record.id);

        if(record.type != AssetType::Texture || resolved.status != AssetAvailability::Available)
        {
            throw std::runtime_error("Texture unavailable. Refresh after restoring the file.");
        }

        std::string extension = resolved.resolvedPath.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value)
        {
            return static_cast<char>(std::tolower(value));
        });
        DirectX::ScratchImage decoded;
        ImageData standard;
        DirectX::Image source{};

        if(extension == ".dds")
        {
            DirectX::ScratchImage compressed;

            if(!DDSLoader::Load(resolved.resolvedPath, compressed))
            {
                throw std::runtime_error("Cannot decode DDS thumbnail.");
            }

            const auto format = compressed.GetMetadata().format;

            if(format != DXGI_FORMAT_BC1_UNORM && format != DXGI_FORMAT_BC1_UNORM_SRGB &&
               format != DXGI_FORMAT_BC4_UNORM && format != DXGI_FORMAT_BC5_UNORM)
            {
                throw std::runtime_error("Unsupported DDS thumbnail format.");
            }

            if(FAILED(DirectX::Decompress(*compressed.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM, decoded)))
            {
                throw std::runtime_error("Cannot decompress DDS thumbnail.");
            }

            source = *decoded.GetImage(0, 0, 0);
        }
        else if(extension == ".hdr")
        {
            HDRImageData hdr;

            if(!ImageLoader::LoadHDR(resolved.resolvedPath, hdr))
            {
                throw std::runtime_error("Cannot decode HDR thumbnail.");
            }

            standard.width = hdr.width;
            standard.height = hdr.height;
            standard.pixels.resize(static_cast<size_t>(hdr.width) * hdr.height * 4);

            for(size_t index = 0; index < standard.pixels.size(); ++index)
            {
                float value = hdr.pixels[index];
                value = std::isfinite(value) ? (std::max)(0.0f, value) : 0.0f;
                standard.pixels[index] = index % 4 == 3 ? 255 :
                    static_cast<unsigned char>(255.0f * std::pow(value / (1.0f + value), 1.0f / 2.2f));
            }
        }
        else if(!ImageLoader::Load(resolved.resolvedPath, standard))
        {
            throw std::runtime_error("Cannot decode image thumbnail.");
        }

        if(extension != ".dds")
        {
            source = {standard.width, standard.height, DXGI_FORMAT_R8G8B8A8_UNORM,
                static_cast<size_t>(standard.width) * 4, standard.pixels.size(), standard.pixels.data()};
        }

        const double scale = (std::min)(1.0, 96.0 / static_cast<double>((std::max)(source.width, source.height)));
        preview.width = static_cast<unsigned int>((std::max)(1.0, std::round(source.width * scale)));
        preview.height = static_cast<unsigned int>((std::max)(1.0, std::round(source.height * scale)));
        DirectX::ScratchImage resized;

        if(FAILED(DirectX::Resize(source, preview.width, preview.height, DirectX::TEX_FILTER_DEFAULT, resized)))
        {
            throw std::runtime_error("Cannot resize thumbnail.");
        }

        const auto* image = resized.GetImage(0, 0, 0);
        const TextureSubresourceData upload{image->pixels, image->rowPitch, image->slicePitch};

        if(!preview.texture.InitializeMipmapped(m_device, preview.width, preview.height, 1, DXGI_FORMAT_R8G8B8A8_UNORM) ||
           !m_uploader->UploadTexture(preview.texture.GetResource(), &upload, 1) ||
           !preview.texture.CreateSRV(m_device, *m_heap))
        {
            throw std::runtime_error("Cannot upload thumbnail or allocate its descriptor.");
        }
    }
    catch(const std::exception& exception)
    {
        // Upload failures return only before submission or after proven completion;
        // ResourceUploader aborts on uncertain completion, preserving fail-closed ownership.
        preview.texture.Shutdown();
        preview.error = exception.what();
    }

    return &preview;
}

std::vector<Texture*> TextureThumbnailCache::GetTextures()
{
    std::vector<Texture*> textures;

    for(auto& [key, preview] : m_previews)
    {
        if(preview.texture.GetResource())
        {
            textures.push_back(&preview.texture);
        }
    }

    return textures;
}

D3D12_GPU_DESCRIPTOR_HANDLE TextureThumbnailCache::GetHandle(const Preview& preview) const
{
    return m_heap->GetGPUHandle(preview.texture.GetDescriptorIndex());
}

void TextureThumbnailCache::ClearAfterGPUDrain()
{
    for(auto& [key, preview] : m_previews)
    {
        preview.texture.Shutdown();
    }

    m_previews.clear();
    m_retirementRequested = false;
}
