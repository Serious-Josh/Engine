#pragma once

#include "Renderer/Texture.h"
#include "Renderer/StructuredBuffer.h"

#include <filesystem>
#include <vector>

struct HDRImageData;
class ResourceUploader;
class DescriptorHeap;

class EnvironmentMap
{
    public:
    // radianceScale multiplies every texel's RGB on upload (EnvironmentRadianceMultiplier: the scene's
    // environment luminance scale times its render pre-scale); 1 uploads the source unchanged.
    bool Initialize(
      ID3D12Device* device,
      const std::filesystem::path& path,
      ResourceUploader& uploader,
      DescriptorHeap& descriptorHeap,
      float radianceScale = 1.0f);

    bool Initialize(ID3D12Device*, const HDRImageData&, ResourceUploader&, DescriptorHeap&, float radianceScale = 1.0f);
    // S0.5: the resources of a width x height source without uploading anything, in two steps. CreateResourcesForUpload
    // only creates committed resources (any thread: no memory registry entry, descriptor or log); CreateViewsForUpload then
    // registers them and creates their shader views on the main thread. The CDF buffers start in COMMON and the source
    // texture in COPY_DEST; the caller records their copies and the precompute passes.
    bool CreateResourcesForUpload(ID3D12Device*, unsigned int width, unsigned int height, float totalWeight);
    bool CreateViewsForUpload(ID3D12Device*, DescriptorHeap&);

    // Physical sky S1 (Physical_Sky_Design_Proposal.md §4.4): the sizes of one persistent sky environment set.
    struct PersistentSkyLayout
    {
        unsigned int width = 1024;
        unsigned int height = 512;
        unsigned int cubemapSize = 256;
        unsigned int cubemapMipLevels = 9;
        unsigned int irradianceSize = 64;
        unsigned int multiscatterSize = 64;
        unsigned int multiscatterMipLevels = 7;
    };

    // Physical sky S1: one of SkyAtmosphere's two persistent sets, with the same resources and getters as an HDRI environment but
    // written on the GPU instead of uploaded: an RGBA16F equirect with its full mip chain (render target for the mips, unordered
    // access for mip 0), importance CDF buffers the GPU writes, and the three cubes at the sky's sizes. Committed resources, created
    // once at renderer initialization and released only by Shutdown at renderer shutdown (never while the session runs). The total
    // weight is the constant 1 (§4.5.1). Every resource starts in a shader-read state, the CDF buffers in COMMON.
    bool InitializePersistentSky(ID3D12Device* device, DescriptorHeap& descriptorHeap, const PersistentSkyLayout& layout);
    void Shutdown();
    // Exchanges every resource, descriptor and importance value. Callers own the GPU lifetime of both sides.
    void Swap(EnvironmentMap& other) noexcept;

    // The importance distribution of an already calibrated image (row-conditional and marginal CDFs); returns the total
    // weight. Thread-safe: no device or descriptor use.
    static float BuildImportanceDistribution(const HDRImageData& image, std::vector<float>& conditionalCDF,
        std::vector<float>& marginalCDF);
    static unsigned int SourceMipLevels(unsigned int width, unsigned int height);

    Texture& GetSourceTexture();
    const Texture& GetSourceTexture() const;
    Texture& GetCubemap();
    const Texture& GetCubemap() const;
    Texture& GetDiffuseIrradiance();
    const Texture& GetDiffuseIrradiance() const;
    unsigned int GetDiffuseIrradianceArrayIndex() const
    {
        return m_diffuseIrradianceArrayIndex;
    }
    Texture& GetGGXMultiscatterIrradiance();
    const Texture& GetGGXMultiscatterIrradiance() const;
    StructuredBuffer& GetImportanceConditionalCDF();
    const StructuredBuffer& GetImportanceConditionalCDF() const;

    StructuredBuffer& GetImportanceMarginalCDF();
    const StructuredBuffer& GetImportanceMarginalCDF() const;

    unsigned int GetImportanceWidth() const;
    unsigned int GetImportanceHeight() const;
    float GetImportanceTotalWeight() const;

    private:
    bool InitializeSourceTexture(ID3D12Device* device, unsigned int width, unsigned int height);
    bool CreatePrecomputeTargets(ID3D12Device* device, DescriptorHeap& descriptorHeap);
    bool CreatePrecomputeTargetResources(ID3D12Device* device, bool trackMemory);
    // The same at explicit sizes, in initialState (the HDRI path: RENDER_TARGET).
    bool CreatePrecomputeTargetResources(ID3D12Device* device, bool trackMemory, unsigned int cubemapSize,
        unsigned int cubemapMipLevels, unsigned int irradianceSize, unsigned int multiscatterSize, unsigned int multiscatterMipLevels,
        D3D12_RESOURCE_STATES initialState);
    bool CreatePrecomputeTargetViews(ID3D12Device* device, DescriptorHeap& descriptorHeap);

    Texture m_sourceTexture;
    Texture m_cubemap;
    Texture m_diffuseIrradiance;

    DescriptorHeap* m_diffuseIrradianceArrayHeap = nullptr;
    unsigned int m_diffuseIrradianceArrayIndex = UINT_MAX;
    Texture m_ggxMultiscatterIrradiance;

    StructuredBuffer m_importanceConditionalCDF;
    StructuredBuffer m_importanceMarginalCDF;

    unsigned int m_importanceWidth = 0;
    unsigned int m_importanceHeight = 0;
    float m_importanceTotalWeight = 0.0f;
};
