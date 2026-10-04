#include "ShadowDenoisePass.h"

#include "../DescriptorHeap.h"
#include "../GraphicsDevice.h"
#include "../Shader/ShaderLibrary.h"
#include "../GBuffer.h"

bool ShadowDenoisePass::Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary, DescriptorHeap& descriptorHeap, unsigned int width, unsigned int height)
{
    ID3D12Device* device = graphicsDevice.GetDevice();

    ShaderDescription shaderDescription{};
    shaderDescription.path = "ShadowDenoise.hlsl";
    shaderDescription.entryPoint = "CSMain";
    shaderDescription.stage = ShaderStage::Compute;

    Shader* computeShader = shaderLibrary.Load(shaderDescription);

    if(!computeShader){
        return false;
    }

    ComputePipelineDescription pipelineDescription{};
    pipelineDescription.computeShader = computeShader;

    RootParameterDescription constants{};
    constants.type = RootParameterType::ConstantBuffer;
    constants.shaderRegister = 0;
    constants.visibility = D3D12_SHADER_VISIBILITY_ALL;

    pipelineDescription.rootParameters.push_back(constants);

    if(!m_pipeline.Initialize(device, pipelineDescription)){
        return false;
    }

    for(unsigned int i = 0; i < 2; ++i)
    {
        if(!m_filteredVisibility[i].Initialize(device, width, height, 1, 1, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            return false;
        }

        if(!m_filteredVisibility[i].CreateSRV(device, descriptorHeap))
        {
            return false;
        }

        if(!m_filteredVisibility[i].CreateUAV(device, descriptorHeap))
        {
            return false;
        }
    }

    m_outputIndex = 0;

    return true;
}

void ShadowDenoisePass::Resize(GraphicsDevice& graphicsDevice, DescriptorHeap& descriptorHeap, unsigned int width, unsigned int height)
{
    ID3D12Device* device = graphicsDevice.GetDevice();

    for(Texture& texture : m_filteredVisibility)
    {
        texture.Shutdown();
    }

    for(unsigned int i = 0; i < 2; ++i)
    {
        if(!m_filteredVisibility[i].Initialize(device, width, height, 1, 1, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            return;
        }

        if(!m_filteredVisibility[i].CreateSRV(device, descriptorHeap))
        {
            return;
        }

        if(!m_filteredVisibility[i].CreateUAV(device, descriptorHeap))
        {
            return;
        }
    }

    m_outputIndex = 0;
}

void ShadowDenoisePass::Shutdown()
{
    m_pipeline.Shutdown();

    for(Texture& texture : m_filteredVisibility)
    {
        texture.Shutdown();
    }

    m_outputIndex = 0;
}

void ShadowDenoisePass::Execute(ID3D12GraphicsCommandList* commandList, Texture& inputVisibility, GBuffer& gBuffer, UploadBuffer& uploadBuffer, const Matrix4& inverseViewProjection, unsigned int width, unsigned int height)
{
    inputVisibility.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const unsigned int stepWidths[] = {1, 2, 4};
    const unsigned int groupCountX = (width + 7) / 8;
    const unsigned int groupCountY = (height + 7) / 8;

    Texture* currentInput = &inputVisibility;

    for(unsigned int passIndex = 0; passIndex < 3; ++passIndex){
        const unsigned int writeIndex = passIndex % 2;

        Texture& output = m_filteredVisibility[writeIndex];

        currentInput->Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        output.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        DenoiseConstants constants{};
        constants.inverseViewProjection = inverseViewProjection;
        constants.visibilityIndex = currentInput->GetDescriptorIndex();
        constants.depthIndex = gBuffer.GetDepth().GetSRVIndex();
        constants.surfaceIndex = gBuffer.GetSurface().GetSRVIndex();
        constants.outputIndex = output.GetUAVIndex();
        constants.width = width;
        constants.height = height;
        constants.stepWidth = stepWidths[passIndex];

        const D3D12_GPU_VIRTUAL_ADDRESS gpuVirtualAddress = uploadBuffer.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(gpuVirtualAddress == 0){
            return;
        }

        m_pipeline.Bind(commandList);

        commandList->SetComputeRootConstantBufferView(0, gpuVirtualAddress);
        commandList->Dispatch(groupCountX, groupCountY, 1);

        output.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        currentInput = &output;
        m_outputIndex = writeIndex;
    }

    m_filteredVisibility[m_outputIndex].Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

Texture& ShadowDenoisePass::GetFilteredVisibilityTexture()
{
    return m_filteredVisibility[m_outputIndex];
}