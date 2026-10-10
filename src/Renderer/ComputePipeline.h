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

class ComputePipeline
{
    public:
        ComputePipeline() = default;

        ComputePipeline(const ComputePipeline&) = delete;
        ComputePipeline& operator=(const ComputePipeline&) = delete;

        bool Initialize(ID3D12Device* device, const ComputePipelineDescription& description);
        void Shutdown();

        void Bind(ID3D12GraphicsCommandList* commandList) const;

        ID3D12RootSignature* GetRootSignature() const;

    private:
        ID3D12RootSignature* m_rootSignature = nullptr;
        ID3D12PipelineState* m_pipelineState = nullptr;
};