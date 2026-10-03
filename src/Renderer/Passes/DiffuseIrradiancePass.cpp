#include "DiffuseIrradiancePass.h"

#include "Core/Log.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/Texture.h"
#include "Renderer/Shader/ShaderLibrary.h"

bool DiffuseIrradiancePass::Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary, DescriptorHeap& rtvHeap, Texture& irradianceCubemap)
{
    if(!device || !irradianceCubemap.GetResource()){
        return false;
    }

    m_rtvHeap = &rtvHeap;
    m_rtvIndices.fill(UINT_MAX);

    ShaderDescription vertexDescription{};
    vertexDescription.path = "DiffuseIrradiance.hlsl";
    vertexDescription.entryPoint = "VSMain";
    vertexDescription.stage = ShaderStage::Vertex;

    Shader* vertexShader = shaderLibrary.Load(vertexDescription);

    if(!vertexShader){
        Log::Error("Failed to load diffuse irradiance vertex shader.");
        return false;
    }

    ShaderDescription pixelDescription{};
    pixelDescription.path = "DiffuseIrradiance.hlsl";
    pixelDescription.entryPoint = "PSMain";
    pixelDescription.stage = ShaderStage::Pixel;

    Shader* pixelShader = shaderLibrary.Load(pixelDescription);

    if(!pixelShader){
        Log::Error("Failed to load diffuse irradiance pixel shader.");
        return false;
    }

    PipelineDescription pipelineDescription{};
    pipelineDescription.vertexShader = vertexShader;
    pipelineDescription.pixelShader = pixelShader;
    pipelineDescription.vertexLayout = nullptr;
    pipelineDescription.renderTargets.count = 1;
    pipelineDescription.renderTargets.formats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pipelineDescription.sampler.filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    pipelineDescription.sampler.addressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    pipelineDescription.sampler.addressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    pipelineDescription.sampler.addressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    pipelineDescription.rootParameters.push_back({RootParameterType::Constants, 0, 2, D3D12_SHADER_VISIBILITY_PIXEL});

    if(!m_pipeline.Initialize(device, pipelineDescription)){
        Log::Error("Failed to init diffuse irradiance pipeline.");
        return false;
    }

    D3D12_RESOURCE_DESC cubemapDescription = irradianceCubemap.GetResource()->GetDesc();

    if(cubemapDescription.DepthOrArraySize != 6 || cubemapDescription.MipLevels != 1){
        Log::Error("Diffuse irradiance resource must contain six faces and one mip.");
        return false;
    }

    m_size = static_cast<unsigned int>(cubemapDescription.Width);

    for(unsigned int face = 0; face < 6; ++face){
        unsigned int descriptorIndex = rtvHeap.Allocate();

        if(descriptorIndex == UINT_MAX){
            Log::Error("Failed to allocate diffuse irradiance face RTV.");
            Shutdown();
            return false;
        }

        m_rtvIndices[face] = descriptorIndex;

        D3D12_RENDER_TARGET_VIEW_DESC rtvDescription{};
        rtvDescription.Format = cubemapDescription.Format;
        rtvDescription.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDescription.Texture2DArray.MipSlice = 0;
        rtvDescription.Texture2DArray.FirstArraySlice = face;
        rtvDescription.Texture2DArray.ArraySize = 1;
        rtvDescription.Texture2DArray.PlaneSlice = 0;

        device->CreateRenderTargetView(irradianceCubemap.GetResource(), &rtvDescription, rtvHeap.GetCPUHandle(descriptorIndex));
    }

    return true;
}

void DiffuseIrradiancePass::Execute(ID3D12GraphicsCommandList* commandList, Texture& sourceTexture, Texture& irradianceCubemap)
{
    if(!commandList || !m_rtvHeap){
        return;
    }

    irradianceCubemap.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_pipeline.Bind(commandList);

    D3D12_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(m_size);
    viewport.Height = static_cast<float>(m_size);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    commandList->RSSetViewports(1, &viewport);

    D3D12_RECT scissor{};
    scissor.left = 0;
    scissor.top = 0;
    scissor.right = static_cast<LONG>(m_size);
    scissor.bottom = static_cast<LONG>(m_size);

    commandList->RSSetScissorRects(1, &scissor);

    for(unsigned int face = 0; face < 6; ++face){
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUHandle(m_rtvIndices[face]);

        commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        unsigned int constants[2] =
        {
            sourceTexture.GetDescriptorIndex(),
            face
        };

        commandList->SetGraphicsRoot32BitConstants(0, 2, constants, 0);

        commandList->DrawInstanced(3, 1, 0, 0);
    }

    irradianceCubemap.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void DiffuseIrradiancePass::Shutdown()
{
    if(m_rtvHeap){
        for(unsigned int& index : m_rtvIndices){
            if(index != UINT_MAX){
                m_rtvHeap->Free(index);
                index = UINT_MAX;
            }
        }
    }

    m_rtvHeap = nullptr;
    m_size = 0;

    m_pipeline.Shutdown();
}