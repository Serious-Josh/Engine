#include "EnvironmentMap.h"
#include "Debug/MemoryRegistry.h"
#include "Debug/TransitionTiming.h"

#include "Core/Log.h"
#include "Renderer/BuiltinEnvironment.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/ImageLoader.h"
#include "Renderer/ResourceUploader.h"
#include "Renderer/TextureMemoryAllocator.h"

#include <algorithm>
#include <optional>
#include <cmath>
#include <vector>

bool EnvironmentMap::Initialize(
    ID3D12Device* device,
    const std::filesystem::path& path,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    float radianceScale)
{
    HDRImageData image{};

    if(path.empty())
    {
        image = MakeNeutralEnvironmentImage();
    }
    else
    {
        // M0c.5 diagnostics (ENGINE_STARTUP_TIMING): the CPU decode of the .hdr file.
        const TransitionTiming::Scope decodeTiming("Environment HDR decode " + path.filename().string(), 0.0,
            TransitionTiming::Startup);

        if (!ImageLoader::LoadHDR(path, image))
        {
            Log::Error("Failed to load HDR environment.");
            return false;
        }
    }

    return Initialize(device,image,uploader,descriptorHeap,radianceScale);
}
// Shared by the synchronous path and the S0.5 preparation worker, so both produce the same distribution bit for bit.
float EnvironmentMap::BuildImportanceDistribution(const HDRImageData& image, std::vector<float>& conditionalCDF,
    std::vector<float>& marginalCDF)
{
    conditionalCDF.assign(static_cast<size_t>(image.width) * static_cast<size_t>(image.height), 0.0f);

    marginalCDF.assign(image.height, 0.0f);

    std::vector<double> rowWeights(
        image.height,
        0.0);

    double totalWeight = 0.0;

    constexpr float pi = 3.14159265358979323846f;

    for (unsigned int y = 0; y < image.height; ++y)
    {
        const float theta = pi * (static_cast<float>(y) + 0.5f) / static_cast<float>(image.height);

        const float solidAngleWeight = std::sin(theta);

        double rowWeight = 0.0;

        for (unsigned int x = 0; x < image.width; ++x)
        {
            const size_t pixelIndex =
                static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x);

            const size_t pixelOffset = pixelIndex * 4;

            const float red = image.pixels[pixelOffset + 0];

            const float green = image.pixels[pixelOffset + 1];

            const float blue = image.pixels[pixelOffset + 2];

            const float luminance = (std::max)(0.2126f * red + 0.7152f * green + 0.0722f * blue, 0.0f);

            const float weight = luminance * solidAngleWeight;

            rowWeight += weight;

            conditionalCDF[pixelIndex] = static_cast<float>(rowWeight);
        }

        rowWeights[y] = rowWeight;

        totalWeight += rowWeight;

        if (rowWeight > 0.0f)
        {
            const size_t rowStart = static_cast<size_t>(y) * static_cast<size_t>(image.width);

            for (unsigned int x = 0; x < image.width; ++x)
            {
                conditionalCDF[rowStart + static_cast<size_t>(x)] /= static_cast<float>(rowWeight);
            }
        }
        else
        {
            const size_t rowStart = static_cast<size_t>(y) * static_cast<size_t>(image.width);

            for (unsigned int x = 0; x < image.width; ++x)
            {
                conditionalCDF[rowStart + static_cast<size_t>(x)] =
                    (static_cast<float>(x) + 1.0f) / static_cast<float>(image.width);
            }
        }
    }

    double accumulatedMarginalWeight = 0.0;

    for (unsigned int y = 0; y < image.height; ++y)
    {
        accumulatedMarginalWeight += rowWeights[y];

        // A black environment still owns valid CDF descriptors. Zero total weight disables
        // importance sampling in the path tracer, RTGI and RTXDI; never invent energy.
        marginalCDF[y] = totalWeight > 0.0 ? static_cast<float>(accumulatedMarginalWeight / totalWeight) :
            (static_cast<float>(y) + 1.0f) / static_cast<float>(image.height);
    }

    // Force the final entries to exactly 1.0f.
    // This avoids floating-point accumulation leaving the CDF
    // slightly below one.

    for (unsigned int y = 0; y < image.height; ++y)
    {
        conditionalCDF[static_cast<size_t>(y) * static_cast<size_t>(image.width) +
                       static_cast<size_t>(image.width - 1)] = 1.0f;
    }

    marginalCDF.back() = 1.0f;

    return static_cast<float>(totalWeight);
}

bool EnvironmentMap::Initialize(ID3D12Device* device,const HDRImageData& source,ResourceUploader& uploader,DescriptorHeap& descriptorHeap,
    float radianceScale)
{
    const MemoryCategoryScope memoryScope(MemoryCategory::MeshesTextures);
    // The source and the cube may be placed in Engine-owned heaps: the uploader activates the source before its copy and
    // the renderer's environment preprocessing activates the cube before its first draw.
    const TextureMemoryAllocator::RenderablePlacementScope placement;

    if(source.width == 0 || source.height == 0 || source.channels != 4 ||
        source.pixels.size() != static_cast<size_t>(source.width) * source.height * 4)
    {
        Log::Error("Invalid environment image dimensions or pixel storage.");
        return false;
    }

    if(!std::isfinite(radianceScale) || radianceScale < 0.0f)
    {
        Log::Error("Invalid environment radiance scale.");
        return false;
    }

    // The calibrated copy feeds the source texture and the importance distribution alike (the CDF is normalized).
    HDRImageData scaled;

    if(radianceScale != 1.0f)
    {
        scaled = source;

        for(size_t index = 0; index < scaled.pixels.size(); ++index)
        {
            if(index % 4 != 3)
            {
                scaled.pixels[index] *= radianceScale;
            }
        }
    }

    const HDRImageData& image = radianceScale != 1.0f ? scaled : source;

    Log::Info("HDR environment loaded: " + std::to_string(image.width) + "x" + std::to_string(image.height));

    // M0c.5 diagnostics (ENGINE_STARTUP_TIMING): the CPU importance distribution, then the GPU resources and precompute.
    std::optional<TransitionTiming::Scope> phaseTiming;
    phaseTiming.emplace("Environment importance CDF (CPU) and CDF buffers", 0.0, TransitionTiming::Startup);

    std::vector<float> conditionalCDF;
    std::vector<float> marginalCDF;

    m_importanceWidth = image.width;
    m_importanceHeight = image.height;
    m_importanceTotalWeight = BuildImportanceDistribution(image, conditionalCDF, marginalCDF);

    if (!m_importanceConditionalCDF.Initialize(device, uploader, conditionalCDF.data(), sizeof(float),
                                               static_cast<unsigned int>(conditionalCDF.size()), descriptorHeap))
    {
        Log::Error("Failed to upload environment conditional CDF.");

        return false;
    }

    if (!m_importanceMarginalCDF.Initialize(device, uploader, marginalCDF.data(), sizeof(float),
                                            static_cast<unsigned int>(marginalCDF.size()), descriptorHeap))
    {
        Log::Error("Failed to upload environment marginal CDF.");

        return false;
    }

    Log::Info("Environment importance CDF built: " + std::to_string(m_importanceWidth) + "x" +
              std::to_string(m_importanceHeight) + ", total weight = " + std::to_string(m_importanceTotalWeight));
    phaseTiming.reset();
    phaseTiming.emplace("Environment source, cubemap and irradiance resources and precompute", 0.0, TransitionTiming::Startup);

    if (!InitializeSourceTexture(device, image.width, image.height))
    {
        return false;
    }

    m_sourceTexture.SetInitialData(image.pixels.data(), image.pixels.size() * sizeof(float));

    if (!uploader.UploadTexture(m_sourceTexture.GetResource(), m_sourceTexture.GetInitialData(),
                                m_sourceTexture.GetInitialDataSize()))
    {
        Log::Error("Failed to upload environment texture.");
        return false;
    }

    if (!m_sourceTexture.CreateSRV(device, descriptorHeap))
    {
        Log::Error("Failed to create environment texture SRV.");
        return false;
    }

    return CreatePrecomputeTargets(device, descriptorHeap);
}

unsigned int EnvironmentMap::SourceMipLevels(unsigned int width, unsigned int height)
{
    return static_cast<unsigned int>(std::floor(std::log2(static_cast<double>((std::max)(width, height))))) + 1;
}

// The source texture starts in COPY_DEST: the synchronous uploader or the S0.5 frame-recorded copies write mip 0.
bool EnvironmentMap::InitializeSourceTexture(ID3D12Device* device, unsigned int width, unsigned int height)
{
    if (!m_sourceTexture.Initialize(device, width, height, 1, SourceMipLevels(width, height),
                                    DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                    D3D12_RESOURCE_STATE_COPY_DEST))
    {
        Log::Error("Failed to initialize environment texture.");
        return false;
    }

    return true;
}

namespace
{
constexpr unsigned int CubemapSize = 1024;
constexpr unsigned int CubemapMipLevels = 11;
constexpr unsigned int DiffuseIrradianceSize = 128;
constexpr unsigned int GGXMultiscatterSize = 64;
constexpr unsigned int GGXMultiscatterMipLevels = 7;
}

// The cubemap, irradiance and multiscatter targets the precompute passes render. trackMemory = false: worker-safe (no
// registry entry, no logging).
bool EnvironmentMap::CreatePrecomputeTargetResources(ID3D12Device* device, bool trackMemory)
{
    return m_cubemap.Initialize(device, CubemapSize, CubemapSize, 6, CubemapMipLevels, DXGI_FORMAT_R16G16B16A16_FLOAT,
               D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, trackMemory) &&
        m_diffuseIrradiance.Initialize(device, DiffuseIrradianceSize, DiffuseIrradianceSize, 6, 1,
            DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
            trackMemory) &&
        m_ggxMultiscatterIrradiance.Initialize(device, GGXMultiscatterSize, GGXMultiscatterSize, 6, GGXMultiscatterMipLevels,
            DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
            trackMemory);
}

// Their shader views (main thread: descriptor allocation).
bool EnvironmentMap::CreatePrecomputeTargetViews(ID3D12Device* device, DescriptorHeap& descriptorHeap)
{
    if (!m_cubemap.CreateCubeSRV(device, descriptorHeap))
    {
        Log::Error("Failed to create environmental cubemap SRV");
        return false;
    }

    if (!m_diffuseIrradiance.CreateCubeSRV(device, descriptorHeap))
    {
        Log::Error("Failed to create diffuse irradiance cubemap SRV.");
        return false;
    }

    m_diffuseIrradianceArrayIndex = descriptorHeap.Allocate();

    if (m_diffuseIrradianceArrayIndex == UINT_MAX)
    {
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

    if (!m_ggxMultiscatterIrradiance.CreateCubeSRV(device, descriptorHeap))
    {
        Log::Error("Failed to create GGX multiscatter irradiance cubemap SRV.");
        return false;
    }

    return true;
}

bool EnvironmentMap::CreatePrecomputeTargets(ID3D12Device* device, DescriptorHeap& descriptorHeap)
{
    if (!CreatePrecomputeTargetResources(device, true))
    {
        Log::Error("Failed to init environment cubemap or irradiance targets.");
        return false;
    }

    return CreatePrecomputeTargetViews(device, descriptorHeap);
}

bool EnvironmentMap::CreateResourcesForUpload(ID3D12Device* device, unsigned int width, unsigned int height, float totalWeight)
{
    if (!device || width == 0 || height == 0 || m_sourceTexture.GetResource())
    {
        return false;
    }

    // The source and the cube are large render-target textures: they may be placed in Engine-owned heaps, and
    // Renderer::RecordEnvironmentPreparation activates them before their first use.
    const TextureMemoryAllocator::RenderablePlacementScope placement;
    m_importanceWidth = width;
    m_importanceHeight = height;
    m_importanceTotalWeight = totalWeight;
    // The source texture starts in COPY_DEST, as InitializeSourceTexture creates it.
    return m_importanceConditionalCDF.CreateStorageResource(device, sizeof(float), width * height) &&
        m_importanceMarginalCDF.CreateStorageResource(device, sizeof(float), height) &&
        m_sourceTexture.Initialize(device, width, height, 1, SourceMipLevels(width, height), DXGI_FORMAT_R32G32B32A32_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST, false) &&
        CreatePrecomputeTargetResources(device, false);
}

bool EnvironmentMap::CreateViewsForUpload(ID3D12Device* device, DescriptorHeap& descriptorHeap)
{
    const MemoryCategoryScope memoryScope(MemoryCategory::MeshesTextures);
    m_sourceTexture.TrackMemory();
    m_cubemap.TrackMemory();
    m_diffuseIrradiance.TrackMemory();
    m_ggxMultiscatterIrradiance.TrackMemory();

    if (!m_importanceConditionalCDF.CreateStorageView(device, descriptorHeap) ||
        !m_importanceMarginalCDF.CreateStorageView(device, descriptorHeap) || !m_sourceTexture.CreateSRV(device, descriptorHeap))
    {
        Log::Error("Failed to create environment texture or importance CDF views.");
        return false;
    }

    return CreatePrecomputeTargetViews(device, descriptorHeap);
}

void EnvironmentMap::Swap(EnvironmentMap& other) noexcept
{
    std::swap(m_sourceTexture, other.m_sourceTexture);
    std::swap(m_cubemap, other.m_cubemap);
    std::swap(m_diffuseIrradiance, other.m_diffuseIrradiance);
    std::swap(m_diffuseIrradianceArrayHeap, other.m_diffuseIrradianceArrayHeap);
    std::swap(m_diffuseIrradianceArrayIndex, other.m_diffuseIrradianceArrayIndex);
    std::swap(m_ggxMultiscatterIrradiance, other.m_ggxMultiscatterIrradiance);
    m_importanceConditionalCDF.Swap(other.m_importanceConditionalCDF);
    m_importanceMarginalCDF.Swap(other.m_importanceMarginalCDF);
    std::swap(m_importanceWidth, other.m_importanceWidth);
    std::swap(m_importanceHeight, other.m_importanceHeight);
    std::swap(m_importanceTotalWeight, other.m_importanceTotalWeight);
}

void EnvironmentMap::Shutdown()
{
    if (m_diffuseIrradianceArrayHeap && m_diffuseIrradianceArrayIndex != UINT_MAX)
    {
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
