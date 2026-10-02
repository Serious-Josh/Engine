#include "EnvironmentMap.h"

#include "Core/Log.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/ImageLoader.h"
#include "Renderer/ResourceUploader.h"

#include <algorithm>
#include <cmath>

bool EnvironmentMap::Initialize(
    ID3D12Device* device,
    const std::filesystem::path& path,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap)
{
    ImageData image{};

    if(!ImageLoader::LoadHDR(path, image)){
        Log::Error("Failed to load HDR environment.");
        return false;
    }

    Log::Info(
        "HDR environment loaded: " +
        std::to_string(image.width) +
        "x" +
        std::to_string(image.height)
    );

    const unsigned int sourceMipLevels =
        static_cast<unsigned int>(
            std::floor(
                std::log2(
                    static_cast<double>(
                        (std::max)(
                            image.width,
                            image.height
                        )
                    )
                )
            )
        ) + 1;

    if(!m_sourceTexture.Initialize(
        device,
        image.width,
        image.height,
        1,
        sourceMipLevels,
        DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        D3D12_RESOURCE_STATE_COPY_DEST))
    {
        Log::Error("Failed to initialize environment texture.");
        return false;
    }

    m_sourceTexture.SetInitialData(
        image.pixels.data(),
        image.pixels.size() * sizeof(float)
    );

    if(!uploader.UploadTexture(
        m_sourceTexture.GetResource(),
        m_sourceTexture.GetInitialData(),
        m_sourceTexture.GetInitialDataSize()))
    {
        Log::Error("Failed to upload environment texture.");
        return false;
    }

    if(!m_sourceTexture.CreateSRV(device, descriptorHeap)){
        Log::Error("Failed to create environment texture SRV.");
        return false;
    }

    constexpr unsigned int cubemapSize = 1024;
    constexpr unsigned int cubemapMipLevels = 11;

    if(!m_cubemap.Initialize(device, cubemapSize, cubemapSize, 6, cubemapMipLevels, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET)){
        Log::Error("Failed to init environment cubemap.");
        return false;
    }

    if(!m_cubemap.CreateCubeSRV(device, descriptorHeap)){
        Log::Error("Failed to create environmental cubemap SRV");
        return false;
    }

    return true;
}

void EnvironmentMap::Shutdown()
{
    m_sourceTexture.Shutdown();
    m_cubemap.Shutdown();
}

Texture& EnvironmentMap::GetSourceTexture()
{
    return m_sourceTexture;
}

const Texture& EnvironmentMap::GetSourceTexture() const
{
    return m_sourceTexture;
}

Texture& EnvironmentMap::GetCubemap()
{
    return m_cubemap;
}

const Texture& EnvironmentMap::GetCubemap() const
{
    return m_cubemap;
}