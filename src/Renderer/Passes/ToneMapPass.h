#pragma once

#include "Renderer/Pipeline.h"

#include <d3d12.h>

class ShaderLibrary;
class RenderTarget;
class Texture;

// Temporary renderer diagnostics; values match ToneMap.hlsl.
enum class ToneMapDebugView : unsigned int
{
    None, SpecularTrackingDistance, NrdViewZ, NrdNormal,
    NrdRoughness, NrdMotion, NrdSpecularRadiance, NrdHitDistance,
    NrdResolvedReflection
};

class ToneMapPass
{
    public:
        bool Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary);
        void Execute(ID3D12GraphicsCommandList* commandList, RenderTarget& sceneColor, D3D12_CPU_DESCRIPTOR_HANDLE outputRTV, Texture* debugTexture = nullptr, ToneMapDebugView debugView = ToneMapDebugView::None, float nrdDenoisingRange = 0.0f);
        void Shutdown();

    private:
        Pipeline m_pipeline;
};
