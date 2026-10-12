#include "EquirectangularToCubemapPass.h"

#include "Core/Log.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/Texture.h"
#include "Renderer/Shader/ShaderLibrary.h"

#include <climits>
#include <algorithm>

bool EquirectangularToCubemapPass::Initialize(
    ID3D12Device* device,
    ShaderLibrary& shaderLibrary,
    DescriptorHeap& rtvHeap,
    Texture& cubemap)
{
    if (!device || !cubemap.GetResource())
    {
        return false;
    }

    m_rtvHeap = &rtvHeap;

    ShaderDescription vertexDescription{};
    vertexDescription.path = "EquirectangularToCubemap.hlsl";
    vertexDescription.entryPoint = "VSMain";
    vertexDescription.stage = ShaderStage::Vertex;

    Shader* vertexShader = shaderLibrary.Load(vertexDescription);

    if (!vertexShader)
    {
        Log::Error("Failed to load equirectangular-to-cubemap vertex shader.");
        return false;
    }

    ShaderDescription pixelDescription{};

    pixelDescription.path = "EquirectangularToCubemap.hlsl";
    pixelDescription.entryPoint = "PSMain";
    pixelDescription.stage = ShaderStage::Pixel;

    Shader* pixelShader = shaderLibrary.Load(pixelDescription);

    if (!pixelShader)
    {
        Log::Error("Failed to load equirectangular-to-cubemap pixel shader.");
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

    pipelineDescription.rootParameters.push_back({RootParameterType::Constants, 0, 4, D3D12_SHADER_VISIBILITY_PIXEL});

    if (!m_pipeline.Initialize(device, pipelineDescription))
    {
        Log::Error("Failed to init equirectangular-to-cubemap pipeline.");
        return false;
    }

    D3D12_RESOURCE_DESC cubemapDescription = cubemap.GetResource()->GetDesc();

    m_size = static_cast<unsigned int>(cubemapDescription.Width);
    m_mipLevels = cubemapDescription.MipLevels;
    m_rtvIndices.assign(6 * m_mipLevels, UINT_MAX);

    if (cubemapDescription.DepthOrArraySize != 6)
    {
        Log::Error("Cubemap resource must contain six array slices.");
        return false;
    }

    for (unsigned int mip = 0; mip < m_mipLevels; ++mip)
    {
        for (unsigned int face = 0; face < 6; ++face)
        {
            unsigned int descriptorIndex = rtvHeap.Allocate();

            if (descriptorIndex == UINT_MAX)
            {
                Log::Error("Failed to allocate cubemap face RTV.");
                Shutdown();
                return false;
            }

            const unsigned int rtvIndex = mip * 6 + face;

            m_rtvIndices[rtvIndex] = descriptorIndex;

            D3D12_RENDER_TARGET_VIEW_DESC rtvDescription{};
            rtvDescription.Format = cubemapDescription.Format;

            rtvDescription.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;

            rtvDescription.Texture2DArray.MipSlice = mip;

            rtvDescription.Texture2DArray.FirstArraySlice = face;

            rtvDescription.Texture2DArray.ArraySize = 1;
            rtvDescription.Texture2DArray.PlaneSlice = 0;

            device->CreateRenderTargetView(cubemap.GetResource(), &rtvDescription,
                                           rtvHeap.GetCPUHandle(descriptorIndex));
        }
    }

    return true;
}

void EquirectangularToCubemapPass::Execute(
    ID3D12GraphicsCommandList* commandList,
    Texture& sourceTexture,
    Texture& cubemap)
{
    if (!commandList || !m_rtvHeap)
    {
        return;
    }

    cubemap.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_pipeline.Bind(commandList);

    for (unsigned int mip = 0; mip < m_mipLevels; ++mip)
    {
        const unsigned int mipSize = (std::max)(1u, m_size >> mip);

        D3D12_VIEWPORT viewport{};
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = static_cast<float>(mipSize);

        viewport.Height = static_cast<float>(mipSize);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        commandList->RSSetViewports(1, &viewport);

        D3D12_RECT scissor{};
        scissor.left = 0;
        scissor.top = 0;
        scissor.right = static_cast<LONG>(mipSize);

        scissor.bottom = static_cast<LONG>(mipSize);

        commandList->RSSetScissorRects(1, &scissor);

        for (unsigned int face = 0; face < 6; ++face)
        {
            const unsigned int rtvIndex = mip * 6 + face;

            D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUHandle(m_rtvIndices[rtvIndex]);

            commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

            unsigned int constants[4] = {sourceTexture.GetDescriptorIndex(), face, mip, m_mipLevels};

            commandList->SetGraphicsRoot32BitConstants(0, 4, constants, 0);

            commandList->DrawInstanced(3, 1, 0, 0);
        }
    }

    cubemap.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void EquirectangularToCubemapPass::Shutdown()
{
    if (m_rtvHeap)
    {

        for (unsigned int& index : m_rtvIndices)
        {

            if (index != UINT_MAX)
            {
                m_rtvHeap->Free(index);

                index = UINT_MAX;
            }
        }
    }

    m_rtvHeap = nullptr;

    m_rtvIndices.clear();

    m_size = 0;
    m_mipLevels = 0;

    m_pipeline.Shutdown();
}
