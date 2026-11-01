#pragma once

#include "Renderer/Shader/Shader.h"
#include "PipelineDescription.h"

#include <d3d12.h>
#include <vector>

struct ComputePipelineDescription
{
    const Shader* computeShader = nullptr;

    std::vector<RootParameterDescription> rootParameters;

    std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;
};

// PSR.2 Stage 3B: Triangle.hlsl's material sampler (Pipeline's default trilinear clamp) at register s<shaderRegister>, for
// the denoisers' PSR variants (the mirror's textures, PsrDenoise.hlsli).
inline D3D12_STATIC_SAMPLER_DESC PsrDenoiseMirrorSampler(UINT shaderRegister)
{
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = shaderRegister;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return sampler;
}

class ComputePipeline
{
    public:
    ComputePipeline() = default;

    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    bool Initialize(
        ID3D12Device* device,
        const ComputePipelineDescription& description);
    void Shutdown();

    void Bind(ID3D12GraphicsCommandList* commandList) const;

    ID3D12RootSignature* GetRootSignature() const;

    private:
    ID3D12RootSignature* m_rootSignature = nullptr;

    ID3D12PipelineState* m_pipelineState = nullptr;
};
