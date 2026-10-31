#pragma once

#include "Renderer/Texture.h"
#include "Renderer/StructuredBuffer.h"

#include <filesystem>

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
    void Shutdown();

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
