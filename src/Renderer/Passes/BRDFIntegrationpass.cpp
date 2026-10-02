#include "BRDFIntegrationPass.h"

#include "Core/Log.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/Texture.h"
#include "Renderer/Shader/ShaderLibrary.h"

bool BRDFIntegrationPass::Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary, DescriptorHeap& rtvHeap, Texture& texture)
{
    if(!device || !texture.GetResource()){
        return false;
    }

    m_rtvHeap = &rtvHeap;

    ShaderDescription vertexDescription{};
    vertexDescription.path = "BRDFIntegration.hlsl";
    vertexDescription.entryPoint = "VSMain";
    vertexDescription.stage = ShaderStage::Vertex;

    Shader* vertexShader = shaderLibrary.Load(vertexDescription);

    if(!vertexShader){
        Log::Error("Failed to load BRDF integration vertex shader.");
        return false;
    }

    ShaderDescription pixelDescription{};
    pixelDescription.path = "BRDFIntegration.hlsl";
    pixelDescription.entryPoint = "PSMain";
    pixelDescription.stage = ShaderStage::Pixel;

    Shader* pixelShader = shaderLibrary.Load(pixelDescription);

    if(!pixelShader){
        Log::Error("Failed to load BRDF integration pixel shader.");
        return false;
    }

    D3D12_RESOURCE_DESC textureDescription = texture.GetResource()->GetDesc();

    m_width = static_cast<unsigned int>(textureDescription.Width);
    m_height = textureDescription.Height;

    PipelineDescription pipelineDescription{};
    pipelineDescription.vertexShader = vertexShader;
    pipelineDescription.pixelShader = pixelShader;
    pipelineDescription.vertexLayout = nullptr;

    pipelineDescription.renderTargets.count = 1;
    pipelineDescription.renderTargets.formats[0] = textureDescription.Format;

    if(!m_pipeline.Initialize(device, pipelineDescription)){
        Log::Error("Failed to init BRDF integration pipeline.");
        return false;
    }

    m_rtvIndex = rtvHeap.Allocate();

    if(m_rtvIndex == UINT_MAX){
        Log::Error("Failed to allocate BRDF integration RTV.");
        Shutdown();
        return false;
    }

    D3D12_RENDER_TARGET_VIEW_DESC rtvDescription{};
    rtvDescription.Format = textureDescription.Format;
    rtvDescription.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtvDescription.Texture2D.MipSlice = 0;
    rtvDescription.Texture2D.PlaneSlice = 0;

    device->CreateRenderTargetView(texture.GetResource(), &rtvDescription, rtvHeap.GetCPUHandle(m_rtvIndex));
    
    return true;
}

void BRDFIntegrationPass::Execute(ID3D12GraphicsCommandList* commandList, Texture& texture)
{
    if(!commandList || !m_rtvHeap || m_rtvIndex == UINT_MAX){
        return;
    }

    texture.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_pipeline.Bind(commandList);

    D3D12_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    commandList->RSSetViewports(1, &viewport);

    D3D12_RECT scissor{};
    scissor.left = 0;
    scissor.top = 0;
    scissor.right = static_cast<LONG>(m_width);
    scissor.bottom = static_cast<LONG>(m_height);

    commandList->RSSetScissorRects(1, &scissor);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUHandle(m_rtvIndex);

    commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    commandList->DrawInstanced(3, 1, 0, 0);

    texture.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void BRDFIntegrationPass::Shutdown()
{
    if(m_rtvHeap && m_rtvIndex != UINT_MAX){
        m_rtvHeap->Free(m_rtvIndex);
        m_rtvIndex = UINT_MAX;
    }

    m_rtvHeap = nullptr;

    m_width = 0;
    m_height = 0;

    m_pipeline.Shutdown();
}