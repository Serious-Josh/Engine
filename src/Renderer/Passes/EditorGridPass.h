#pragma once

#include "Renderer/DepthBuffer.h"
#include "Renderer/EditorGrid.h"
#include "Renderer/Pipeline.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/Shader/ShaderLibrary.h"

// Editor-only adaptive grid (G.3). Records one fullscreen triangle into the final Editor image,
// like EditorColliderPass: after tone mapping, so lighting, temporal history, selection masks and
// object IDs never see it. It owns only its pipeline; constants travel as root constants copied
// into the command stream, so there are no descriptors, buffers or per-frame allocations.
//
// Header-only because the approved G.3 scope adds no build-list entries.
class EditorGridPass
{
public:
    bool Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary)
    {
        const Shader* vertexShader = shaderLibrary.Load({"EditorGrid.hlsl", "VSMain", ShaderStage::Vertex});
        const Shader* pixelShader = shaderLibrary.Load({"EditorGrid.hlsl", "PSMain", ShaderStage::Pixel});

        if(!vertexShader || !pixelShader)
        {
            return false;
        }

        PipelineDescription pipelineDescription;
        pipelineDescription.vertexShader = vertexShader;
        pipelineDescription.pixelShader = pixelShader;
        pipelineDescription.renderTargets.count = 1;
        pipelineDescription.renderTargets.formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pipelineDescription.alphaBlend = true;

        // The pixel shader writes the plane's depth; scene depth is read-only and occludes the
        // grid. Conventional depth: cleared to 1, nearer is smaller.
        pipelineDescription.depthEnabled = true;
        pipelineDescription.depthFormat = DXGI_FORMAT_D32_FLOAT;
        pipelineDescription.depthComparison = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pipelineDescription.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pipelineDescription.rootParameters.push_back(
            {RootParameterType::Constants, 0, EditorGrid::kConstantCount, D3D12_SHADER_VISIBILITY_PIXEL});

        return m_pipeline.Initialize(device, pipelineDescription);
    }

    bool IsInitialized() const
    {
        return m_pipeline.IsInitialized();
    }

    // Renderer calls this only after its existing GPU-confirmed teardown.
    void Shutdown()
    {
        m_pipeline.Shutdown();
    }

    void Execute(ID3D12GraphicsCommandList* commandList, RenderTarget& viewportColor, DepthBuffer& viewportDepth,
        const Matrix4& view, const Matrix4& projection, const Vector3& cameraPosition,
        const EditorGridRequest& request) const
    {
        if(!m_pipeline.IsInitialized())
        {
            return;
        }

        const bool matchingTargetDimensions = viewportColor.GetWidth() == viewportDepth.GetWidth() &&
            viewportColor.GetHeight() == viewportDepth.GetHeight();

        if(!matchingTargetDimensions)
        {
            return;
        }

        // Validate before changing any state; an invalid camera or spacing records nothing.
        EditorGrid::Constants constants;

        if(!EditorGrid::BuildConstants(view, projection, cameraPosition, request.baseSpacing, constants))
        {
            return;
        }

        const D3D12_RESOURCE_STATES previousDepthState = viewportDepth.GetState();
        viewportDepth.Transition(commandList, D3D12_RESOURCE_STATE_DEPTH_READ);

        const D3D12_CPU_DESCRIPTOR_HANDLE colorTarget = viewportColor.GetRTV();
        const D3D12_CPU_DESCRIPTOR_HANDLE readOnlyDepth = viewportDepth.GetReadOnlyDSV();
        commandList->OMSetRenderTargets(1, &colorTarget, FALSE, &readOnlyDepth);
        m_pipeline.Bind(commandList);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->SetGraphicsRoot32BitConstants(0, EditorGrid::kConstantCount, &constants, 0);
        commandList->DrawInstanced(3, 1, 0, 0);

        // Unbind the read-only DSV before restoring the tracked state for later consumers.
        commandList->OMSetRenderTargets(1, &colorTarget, FALSE, nullptr);
        viewportDepth.Transition(commandList, previousDepthState);
    }

private:
    Pipeline m_pipeline;
};
