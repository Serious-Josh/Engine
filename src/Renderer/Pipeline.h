#pragma once

#include <d3d12.h>

class Pipeline
{
    public:
        bool Initialize(ID3D12Device* device, const char* vertexShaderPath, const char* pixelShaderPath);
        void Shutdown();
        void Bind(ID3D12GraphicsCommandList* commandList) const;

        ID3D12RootSignature* GetRootSignature() const;

    private:
        ID3D12RootSignature* m_rootSignature = nullptr;
        ID3D12PipelineState* m_pipelineState = nullptr;
};