#pragma once

#include "Renderer/Pipeline.h"

#include <d3d12.h>

class ShaderLibrary;
class RenderTarget;

class ToneMapPass
{
    public:
        bool Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary);
        void Execute(ID3D12GraphicsCommandList* commandList, RenderTarget& sceneColor, D3D12_CPU_DESCRIPTOR_HANDLE outputRTV);
        void Shutdown();

    private:
        Pipeline m_pipeline;
};