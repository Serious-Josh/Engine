#pragma once

#include "Renderer/VertexLayout.h"
#include "Renderer/Shader.h"

#include <d3d12.h>
#include <array>

struct PipelineRenderTargets
{
    std::array<DXGI_FORMAT, 8> formats{};
    unsigned int count = 0;
};

class Pipeline
{
    public:
        Pipeline() = default;

        Pipeline(const Pipeline&) = delete;
        Pipeline& operator=(const Pipeline&) = delete;
        
        bool Initialize(ID3D12Device* device, const char* vertexShaderPath, const char* pixelShaderPath, const VertexLayout* vertexLayout, const PipelineRenderTargets& renderTargets, unsigned int rootConstantCount);
        void Shutdown();
        void Bind(ID3D12GraphicsCommandList* commandList) const;

        ID3D12RootSignature* GetRootSignature() const;

    private:
        ID3D12RootSignature* m_rootSignature = nullptr;
        ID3D12PipelineState* m_pipelineState = nullptr;

        Shader m_vertexShader;
        Shader m_pixelShader;
};