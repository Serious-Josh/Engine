#pragma once

#include "Math/Vector3.h"

#include "Renderer/Pipeline.h"

#include <d3d12.h>

class ShaderLibrary;
class RenderTarget;
class Texture;

// Renderer diagnostic views; values match ToneMap.hlsl.
enum class ToneMapDebugView : unsigned int
{
    None,
    SpecularTrackingDistance,
    NrdViewZ,
    NrdNormal,
    NrdRoughness,
    NrdMotion,
    NrdSpecularRadiance,
    NrdHitDistance,
    NrdResolvedReflection,
    GBufferMotion,
    SelectionMask,
    GIRaw,
    GIHitDistance,
    GIDenoised,
    AORaw = 14,
    AOTemporal = 15,
    AOFiltered = 16,
    GBufferWorldNormal = 17,
    GBufferAlbedo = 18,
    EnvironmentSpecularOnly = 19,
    DirectLightingOnly = 20,
    HDRIDiffuseAmbientOnly = 21,
    HDRIDiffuseAmbientWithoutAO = 22,
    HDRIDiffuseAmbientDirectIntegral = 23,
    IrradianceCubemapMinusDirectIntegral = 24,
    HDRIIrradianceTangentFrameDifference = 25,
    HDRIDiffuseAmbientMultiOrientation = 26,
    MultiOrientationMinusSingleOrientation = 27,
    HDRIDirectIntegralMip0 = 28,
    HDRIDirectIntegralMip4 = 29,
    HDRIDirectIntegralMip6 = 30,
    GISignedReconstructed = 31,
    ShadingNormalViewHorizon = 32
};

class ToneMapPass
{
    public:
    bool Initialize(
      ID3D12Device* device,
      ShaderLibrary& shaderLibrary);
    void Execute(ID3D12GraphicsCommandList* commandList, RenderTarget& sceneColor,
                 D3D12_CPU_DESCRIPTOR_HANDLE outputRTV, Texture* debugTexture = nullptr,
                 ToneMapDebugView debugView = ToneMapDebugView::None, float nrdDenoisingRange = 0.0f,
                 RenderTarget* debugRenderTarget = nullptr, RenderTarget* selectionMask = nullptr,
                 const Vector3& outlineColor = Vector3{1.0f, 0.604f, 0.0f}, unsigned int outlineThickness = 2,
                 unsigned int debugDepthIndex = 0);
    void Shutdown();

    private:
    Pipeline m_pipeline;
};
