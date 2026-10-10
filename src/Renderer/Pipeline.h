#pragma once

#include "Renderer/VertexLayout.h"
#include "Renderer/Shader/Shader.h"
#include "PipelineDescription.h"

#include <d3d12.h>
#include <array>
#include <vector>

struct PipelineRenderTargets
{
    std::array<DXGI_FORMAT, 8> formats{};
    unsigned int count = 0;
};

struct StaticSamplerDescription
{
    D3D12_FILTER filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    D3D12_TEXTURE_ADDRESS_MODE addressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    D3D12_TEXTURE_ADDRESS_MODE addressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    D3D12_TEXTURE_ADDRESS_MODE addressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    unsigned int shaderRegister = 0;

    D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_PIXEL;
};

struct PipelineDescription
{
    const Shader* vertexShader = nullptr;
    const Shader* pixelShader = nullptr;

    const VertexLayout* vertexLayout = nullptr;

    PipelineRenderTargets renderTargets{};
    StaticSamplerDescription sampler{};
    std::vector<D3D12_STATIC_SAMPLER_DESC> additionalSamplers;

    std::vector<RootParameterDescription> rootParameters;

    bool depthEnabled = false;
    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;

    D3D12_COMPARISON_FUNC depthComparison = D3D12_COMPARISON_FUNC_LESS;
    D3D12_DEPTH_WRITE_MASK depthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
};


class Pipeline
{
    public:
        Pipeline() = default;

        Pipeline(const Pipeline&) = delete;
        Pipeline& operator=(const Pipeline&) = delete;
        
        bool Initialize(ID3D12Device* device, const PipelineDescription& description);
        void Shutdown();
        void Bind(ID3D12GraphicsCommandList* commandList) const;

        ID3D12RootSignature* GetRootSignature() const;

    private:
        ID3D12RootSignature* m_rootSignature = nullptr;
        ID3D12PipelineState* m_pipelineState = nullptr;
};
