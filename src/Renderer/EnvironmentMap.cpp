#include "EnvironmentMap.h"

#include "Core/Log.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/ImageLoader.h"
#include "Renderer/ResourceUploader.h"

#include <algorithm>
#include <cmath>
#include <vector>

bool EnvironmentMap::Initialize(
    ID3D12Device* device,
    const std::filesystem::path& path,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap)
{
    HDRImageData image{};

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

    std::vector<float> conditionalCDF(
        static_cast<size_t>(image.width) *
        static_cast<size_t>(image.height)
    );

    std::vector<float> marginalCDF(
        image.height
    );

    std::vector<double> rowWeights(
        image.height,
        0.0
    );

    m_importanceWidth =
        image.width;

    m_importanceHeight =
        image.height;

    m_importanceTotalWeight =
        0.0f;

    double totalWeight =
        0.0;

    constexpr float pi =
        3.14159265358979323846f;

    for(unsigned int y = 0; y < image.height; ++y)
    {
        const float theta =
            pi *
            (
                static_cast<float>(y) +
                0.5f
            ) /
            static_cast<float>(image.height);

        const float solidAngleWeight =
            std::sin(theta);

        double rowWeight =
            0.0;

        for(unsigned int x = 0; x < image.width; ++x)
        {
            const size_t pixelIndex =
                static_cast<size_t>(y) *
                static_cast<size_t>(image.width) +
                static_cast<size_t>(x);

            const size_t pixelOffset =
                pixelIndex * 4;

            const float red =
                image.pixels[pixelOffset + 0];

            const float green =
                image.pixels[pixelOffset + 1];

            const float blue =
                image.pixels[pixelOffset + 2];

            const float luminance =
                (std::max)(
                    0.2126f * red +
                    0.7152f * green +
                    0.0722f * blue,
                    0.0f
                );

            const float weight =
                luminance *
                solidAngleWeight;

            rowWeight +=
                weight;

            conditionalCDF[pixelIndex] = static_cast<float>(rowWeight);
        }

        rowWeights[y] =
            rowWeight;

        totalWeight +=
            rowWeight;

        if(rowWeight > 0.0f)
        {
            const size_t rowStart =
                static_cast<size_t>(y) *
                static_cast<size_t>(image.width);

            for(unsigned int x = 0; x < image.width; ++x)
            {
                conditionalCDF[
                    rowStart +
                    static_cast<size_t>(x)
                ] /=
                    static_cast<float>(rowWeight);
            }
        }
        else
        {
            const size_t rowStart =
                static_cast<size_t>(y) *
                static_cast<size_t>(image.width);

            for(unsigned int x = 0; x < image.width; ++x)
            {
                conditionalCDF[
                    rowStart +
                    static_cast<size_t>(x)
                ] =
                    (
                        static_cast<float>(x) +
                        1.0f
                    ) /
                    static_cast<float>(image.width);
            }
        }
    }

    if(totalWeight <= 0.0)
    {
        Log::Error(
            "Failed to build environment importance distribution."
        );

        return false;
    }

    m_importanceTotalWeight =
        static_cast<float>(totalWeight);

    double accumulatedMarginalWeight =
        0.0;

    for(unsigned int y = 0; y < image.height; ++y)
    {
        accumulatedMarginalWeight +=
            rowWeights[y];

        marginalCDF[y] =
            static_cast<float>(
                accumulatedMarginalWeight /
                totalWeight
            );
    }

    // Force the final entries to exactly 1.0f.
    // This avoids floating-point accumulation leaving the CDF
    // slightly below one.
    for(unsigned int y = 0; y < image.height; ++y)
    {
        conditionalCDF[
            static_cast<size_t>(y) *
            static_cast<size_t>(image.width) +
            static_cast<size_t>(image.width - 1)
        ] =
            1.0f;
    }

    marginalCDF.back() =
        1.0f;

    if(!m_importanceConditionalCDF.Initialize(
        device,
        uploader,
        conditionalCDF.data(),
        sizeof(float),
        static_cast<unsigned int>(
            conditionalCDF.size()
        ),
        descriptorHeap))
    {
        Log::Error(
            "Failed to upload environment conditional CDF."
        );

        return false;
    }

    if(!m_importanceMarginalCDF.Initialize(
        device,
        uploader,
        marginalCDF.data(),
        sizeof(float),
        static_cast<unsigned int>(
            marginalCDF.size()
        ),
        descriptorHeap))
    {
        Log::Error(
            "Failed to upload environment marginal CDF."
        );

        return false;
    }

    Log::Info(
        "Environment importance CDF built: " +
        std::to_string(m_importanceWidth) +
        "x" +
        std::to_string(m_importanceHeight) +
        ", total weight = " +
        std::to_string(m_importanceTotalWeight)
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

    constexpr unsigned int diffuseIrradianceSize = 128;

    if(!m_diffuseIrradiance.Initialize(device, diffuseIrradianceSize, diffuseIrradianceSize, 6, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET)){
        Log::Error("Failed to init diffuse irradiance cubemap.");
        return false;
    }

    if(!m_diffuseIrradiance.CreateCubeSRV(device, descriptorHeap)){
        Log::Error("Failed to create diffuse irradiance cubemap SRV.");
        return false;
    }

    m_diffuseIrradianceArrayIndex = descriptorHeap.Allocate();
    if(m_diffuseIrradianceArrayIndex == UINT_MAX){
        Log::Error("Failed to allocate diffuse irradiance array SRV.");
        return false;
    }
    m_diffuseIrradianceArrayHeap = &descriptorHeap;
    D3D12_SHADER_RESOURCE_VIEW_DESC arrayView{};
    arrayView.Format = m_diffuseIrradiance.GetResource()->GetDesc().Format;
    arrayView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    arrayView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    arrayView.Texture2DArray.MostDetailedMip = 0;
    arrayView.Texture2DArray.MipLevels = 1;
    arrayView.Texture2DArray.FirstArraySlice = 0;
    arrayView.Texture2DArray.ArraySize = 6;
    device->CreateShaderResourceView(m_diffuseIrradiance.GetResource(), &arrayView,
        descriptorHeap.GetCPUHandle(m_diffuseIrradianceArrayIndex));

    constexpr unsigned int ggxMultiscatterSize = 64;
    constexpr unsigned int ggxMultiscatterMipLevels = 7;

    if(!m_ggxMultiscatterIrradiance.Initialize(
        device,
        ggxMultiscatterSize,
        ggxMultiscatterSize,
        6,
        ggxMultiscatterMipLevels,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET))
    {
        Log::Error("Failed to init GGX multiscatter irradiance cubemap.");
        return false;
    }

    if(!m_ggxMultiscatterIrradiance.CreateCubeSRV(
        device,
        descriptorHeap))
    {
        Log::Error("Failed to create GGX multiscatter irradiance cubemap SRV.");
        return false;
    }

    return true;
}

void EnvironmentMap::Shutdown()
{
    if(m_diffuseIrradianceArrayHeap && m_diffuseIrradianceArrayIndex != UINT_MAX){
        m_diffuseIrradianceArrayHeap->Free(m_diffuseIrradianceArrayIndex);
    }
    m_diffuseIrradianceArrayHeap = nullptr;
    m_diffuseIrradianceArrayIndex = UINT_MAX;
    m_sourceTexture.Shutdown();
    m_cubemap.Shutdown();
    m_diffuseIrradiance.Shutdown();
    m_ggxMultiscatterIrradiance.Shutdown();

    m_importanceConditionalCDF.Shutdown();
    m_importanceMarginalCDF.Shutdown();

    m_importanceWidth = 0;
    m_importanceHeight = 0;
    m_importanceTotalWeight = 0.0f;
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

Texture& EnvironmentMap::GetDiffuseIrradiance()
{
    return m_diffuseIrradiance;
}

const Texture& EnvironmentMap::GetDiffuseIrradiance() const
{
    return m_diffuseIrradiance;
}

Texture& EnvironmentMap::GetGGXMultiscatterIrradiance()
{
    return m_ggxMultiscatterIrradiance;
}

const Texture& EnvironmentMap::GetGGXMultiscatterIrradiance() const
{
    return m_ggxMultiscatterIrradiance;
}

StructuredBuffer& EnvironmentMap::GetImportanceConditionalCDF()
{
    return m_importanceConditionalCDF;
}

const StructuredBuffer& EnvironmentMap::GetImportanceConditionalCDF() const
{
    return m_importanceConditionalCDF;
}

StructuredBuffer& EnvironmentMap::GetImportanceMarginalCDF()
{
    return m_importanceMarginalCDF;
}

const StructuredBuffer& EnvironmentMap::GetImportanceMarginalCDF() const
{
    return m_importanceMarginalCDF;
}

unsigned int EnvironmentMap::GetImportanceWidth() const
{
    return m_importanceWidth;
}

unsigned int EnvironmentMap::GetImportanceHeight() const
{
    return m_importanceHeight;
}

float EnvironmentMap::GetImportanceTotalWeight() const
{
    return m_importanceTotalWeight;
}
