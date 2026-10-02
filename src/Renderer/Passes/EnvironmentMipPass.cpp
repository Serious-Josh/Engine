#include "EnvironmentMipPass.h"

#include "Core/Log.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/Texture.h"
#include "Renderer/Shader/ShaderLibrary.h"

#include <climits>
#include <algorithm>

bool EnvironmentMipPass::Initialize(
    ID3D12Device* device,
    ShaderLibrary& shaderLibrary,
    DescriptorHeap& rtvHeap,
    Texture& texture)
{
    if(!device || !texture.GetResource()){
        return false;
    }

    m_rtvHeap = &rtvHeap;

    ShaderDescription vertexDescription{};
    vertexDescription.path = "EnvironmentMip.hlsl";
    vertexDescription.entryPoint = "VSMain";
    vertexDescription.stage = ShaderStage::Vertex;

    Shader* vertexShader =
        shaderLibrary.Load(vertexDescription);

    if(!vertexShader){
        Log::Error(
            "Failed to load environment mip vertex shader."
        );
        return false;
    }

    ShaderDescription pixelDescription{};
    pixelDescription.path = "EnvironmentMip.hlsl";
    pixelDescription.entryPoint = "PSMain";
    pixelDescription.stage = ShaderStage::Pixel;

    Shader* pixelShader =
        shaderLibrary.Load(pixelDescription);

    if(!pixelShader){
        Log::Error(
            "Failed to load environment mip pixel shader."
        );
        return false;
    }

    D3D12_RESOURCE_DESC textureDescription =
        texture.GetResource()->GetDesc();

    m_width =
        static_cast<unsigned int>(
            textureDescription.Width
        );

    m_height =
        textureDescription.Height;

    m_mipLevels =
        textureDescription.MipLevels;

    if(m_mipLevels <= 1){
        Log::Error(
            "Environment texture has no mip chain."
        );
        return false;
    }

    PipelineDescription pipelineDescription{};
    pipelineDescription.vertexShader = vertexShader;
    pipelineDescription.pixelShader = pixelShader;
    pipelineDescription.vertexLayout = nullptr;

    pipelineDescription.renderTargets.count = 1;
    pipelineDescription.renderTargets.formats[0] =
        textureDescription.Format;

    pipelineDescription.sampler.filter =
        D3D12_FILTER_MIN_MAG_MIP_LINEAR;

    pipelineDescription.sampler.addressU =
        D3D12_TEXTURE_ADDRESS_MODE_WRAP;

    pipelineDescription.sampler.addressV =
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    pipelineDescription.sampler.addressW =
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    pipelineDescription.rootParameters.push_back(
        {
            RootParameterType::Constants,
            0,
            2,
            D3D12_SHADER_VISIBILITY_PIXEL
        }
    );

    if(!m_pipeline.Initialize(
        device,
        pipelineDescription))
    {
        Log::Error(
            "Failed to initialize environment mip pipeline."
        );
        return false;
    }

    m_rtvIndices.assign(
        m_mipLevels,
        UINT_MAX
    );

    // Mip 0 already contains the uploaded HDR image.
    // Only mips 1...N need RTVs.
    for(unsigned int mip = 1;
        mip < m_mipLevels;
        ++mip)
    {
        unsigned int descriptorIndex =
            rtvHeap.Allocate();

        if(descriptorIndex == UINT_MAX){
            Log::Error(
                "Failed to allocate environment mip RTV."
            );
            Shutdown();
            return false;
        }

        m_rtvIndices[mip] =
            descriptorIndex;

        D3D12_RENDER_TARGET_VIEW_DESC rtvDescription{};
        rtvDescription.Format =
            textureDescription.Format;

        rtvDescription.ViewDimension =
            D3D12_RTV_DIMENSION_TEXTURE2D;

        rtvDescription.Texture2D.MipSlice =
            mip;

        rtvDescription.Texture2D.PlaneSlice =
            0;

        device->CreateRenderTargetView(
            texture.GetResource(),
            &rtvDescription,
            rtvHeap.GetCPUHandle(
                descriptorIndex
            )
        );
    }

    return true;
}

void EnvironmentMipPass::Execute(
    ID3D12GraphicsCommandList* commandList,
    Texture& texture)
{
    if(!commandList || !m_rtvHeap){
        return;
    }

    commandList->IASetPrimitiveTopology(
        D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
    );

    m_pipeline.Bind(commandList);

    for(unsigned int mip = 1;
        mip < m_mipLevels;
        ++mip)
    {
        const unsigned int sourceMip =
            mip - 1;

        texture.Transition(
            commandList,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            sourceMip
        );

        texture.Transition(
            commandList,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            mip
        );

        const unsigned int mipWidth =
            (std::max)(1u, m_width >> mip);

        const unsigned int mipHeight =
            (std::max)(1u, m_height >> mip);

        D3D12_VIEWPORT viewport{};
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width =
            static_cast<float>(mipWidth);
        viewport.Height =
            static_cast<float>(mipHeight);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        commandList->RSSetViewports(
            1,
            &viewport
        );

        D3D12_RECT scissor{};
        scissor.left = 0;
        scissor.top = 0;
        scissor.right =
            static_cast<LONG>(mipWidth);
        scissor.bottom =
            static_cast<LONG>(mipHeight);

        commandList->RSSetScissorRects(
            1,
            &scissor
        );

        D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            m_rtvHeap->GetCPUHandle(
                m_rtvIndices[mip]
            );

        commandList->OMSetRenderTargets(
            1,
            &rtv,
            FALSE,
            nullptr
        );

        unsigned int constants[2] =
        {
            texture.GetDescriptorIndex(),
            sourceMip
        };

        commandList->SetGraphicsRoot32BitConstants(
            0,
            2,
            constants,
            0
        );

        commandList->DrawInstanced(
            3,
            1,
            0,
            0
        );
    }

    texture.Transition(
        commandList,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );
}

void EnvironmentMipPass::Shutdown()
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

    m_rtvIndices.clear();

    m_width = 0;
    m_height = 0;
    m_mipLevels = 0;

    m_pipeline.Shutdown();
}