#pragma once

#include "Core/Log.h"
#include "Renderer/DepthBuffer.h"
#include "Renderer/Pipeline.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/Shader/ShaderLibrary.h"

#include <d3d12.h>

#include <cstdint>
#include <cstring>

// UPSCALING.2C diagnostic presentation resolve (PresentationResolve.hlsl): resamples the render-resolution HDR scene
// colour, selection mask and depth to the output extent so tone mapping and the Editor overlays run there unchanged.
// Recorded only while the render extent differs from the output extent; never in Native. Header-only so that it needs
// no build-system change; the DLSS evaluation is meant to replace its colour output.
class PresentationResolvePass
{
public:
    // Created on the first reduced-resolution frame; the packaged runtime never renders one.
    bool Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary)
    {
        if(m_pipeline.IsInitialized())
        {
            return true;
        }

        ShaderDescription vertexDescription{};
        vertexDescription.path = "PresentationResolve.hlsl";
        vertexDescription.entryPoint = "VSMain";
        vertexDescription.stage = ShaderStage::Vertex;
        Shader* vertexShader = shaderLibrary.Load(vertexDescription);

        ShaderDescription pixelDescription{};
        pixelDescription.path = "PresentationResolve.hlsl";
        pixelDescription.entryPoint = "PSMain";
        pixelDescription.stage = ShaderStage::Pixel;
        Shader* pixelShader = shaderLibrary.Load(pixelDescription);

        if(!vertexShader || !pixelShader)
        {
            Log::Error("Failed to load presentation resolve shaders.");
            return false;
        }

        PipelineDescription pipelineDescription{};
        pipelineDescription.vertexShader = vertexShader;
        pipelineDescription.pixelShader = pixelShader;
        pipelineDescription.renderTargets.count = 2;
        pipelineDescription.renderTargets.formats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pipelineDescription.renderTargets.formats[1] = DXGI_FORMAT_R8_UNORM;
        pipelineDescription.depthEnabled = true;
        pipelineDescription.depthFormat = DXGI_FORMAT_D32_FLOAT;
        pipelineDescription.depthComparison = D3D12_COMPARISON_FUNC_ALWAYS;
        pipelineDescription.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pipelineDescription.rootParameters.push_back({RootParameterType::Constants, 0, 9, D3D12_SHADER_VISIBILITY_PIXEL});

        if(!m_pipeline.Initialize(device, pipelineDescription))
        {
            Log::Error("Failed to initialize presentation resolve pipeline.");
            return false;
        }

        return true;
    }

    // The caller sets the output viewport and scissor. Leaves the outputs shader-readable. jitterX/Y: the frame's camera
    // jitter (render pixels, CameraJitterStatus convention; 0 without jitter), which places each render sample.
    void Execute(ID3D12GraphicsCommandList* commandList, RenderTarget& sceneColor, DepthBuffer& sceneDepth,
        RenderTarget& selectionMask, RenderTarget& outputColor, RenderTarget& outputSelectionMask,
        DepthBuffer& outputDepth, float jitterX, float jitterY) const
    {
        constexpr D3D12_RESOURCE_STATES shaderRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        sceneColor.Transition(commandList, shaderRead);
        sceneDepth.Transition(commandList, shaderRead);
        selectionMask.Transition(commandList, shaderRead);
        outputColor.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
        outputSelectionMask.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
        outputDepth.Transition(commandList, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        const D3D12_CPU_DESCRIPTOR_HANDLE targets[] = {outputColor.GetRTV(), outputSelectionMask.GetRTV()};
        const D3D12_CPU_DESCRIPTOR_HANDLE depth = outputDepth.GetDSV();
        commandList->OMSetRenderTargets(2, targets, FALSE, &depth);
        m_pipeline.Bind(commandList);

        std::uint32_t constants[9] = {sceneColor.GetSRVIndex(), sceneDepth.GetSRVIndex(), selectionMask.GetSRVIndex(),
            sceneColor.GetWidth(), sceneColor.GetHeight(), outputColor.GetWidth(), outputColor.GetHeight(), 0u, 0u};
        std::memcpy(&constants[7], &jitterX, sizeof(float));
        std::memcpy(&constants[8], &jitterY, sizeof(float));
        commandList->SetGraphicsRoot32BitConstants(0, 9, constants, 0);
        commandList->DrawInstanced(3, 1, 0, 0);

        outputColor.Transition(commandList, shaderRead);
        outputSelectionMask.Transition(commandList, shaderRead);
        outputDepth.Transition(commandList, shaderRead);
    }

    void Shutdown()
    {
        m_pipeline.Shutdown();
    }

private:
    Pipeline m_pipeline;
};
