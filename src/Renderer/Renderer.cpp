#include "Core/Log.h"
#include "Renderer.h"
#include "DemoContent/DemoScene.h"
#include "Renderer/Texture.h"
#include "Material.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstring>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>


// --------------
// Initialization
// --------------

bool Renderer::Initialize(void* windowHandle, unsigned int width, unsigned int height)
{
    m_width = width;
    m_height = height;

    if(!InitializeDevice())
    {
        return false;
    }

    ID3D12Device* device = m_graphicsDevice.GetDevice();

    PipelineRenderTargets compositeTargets{};
    compositeTargets.count = 1;
    compositeTargets.formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

    if(!m_compositePipeline.Initialize(
        device,
        ENGINE_SHADER_DIR "/CompositeVS.cso",
        ENGINE_SHADER_DIR "/CompositePS.cso",
        nullptr,
        compositeTargets,
        2
    )){
        Log::Error("Failed to init composite test pipeline.");
        return false;
    }

    if(!m_resourceUploader.Initialize(device)){
        Log::Error("Failed to init resource uploader.");
        return false;
    }

    if(!m_resourceDescriptorHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, true)){
        Log::Error("Failed to init resource descriptor heap.");
        return false;
    }

    if(!m_rtvDescriptorheap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 16, false)){
        Log::Error("Failed to init rtv descriptor heap.");
        return false;
    }

    if(!m_gBuffer.Initialize(device, m_width, m_height, m_rtvDescriptorheap, m_resourceDescriptorHeap)){
        Log::Error("Failed to init G-Buffer.");
        return false;
    }



    if(!InitializeSwapChain(windowHandle))
    {
        return false;
    }


    return true;
}

bool Renderer::InitializeDevice()
{
    if(!m_graphicsDevice.Initialize()){
        Log::Error("Failed to init graphics device.");
        return false;
    }

    if(!m_commandContext.Initialize(m_graphicsDevice.GetDevice())){
        Log::Error("Failed to init command context");
        return false;
    }

    return true;
}

bool Renderer::InitializeSwapChain(void* windowHandle)
{
    return m_swapChain.Initialize(m_graphicsDevice.GetFactory(), m_graphicsDevice.GetDevice(), m_commandContext.GetCommandQueue(), windowHandle, m_width, m_height);
}


//Other functions needed for Render
void Renderer::Resize(unsigned int width, unsigned int height)
{
    if(width == 0 || height == 0){
        return;
    }

    m_commandContext.WaitForGPU();

    if(!m_swapChain.Resize(width, height)){
        Log::Error("Failed to resize swap chain.");
        return;
    }

    if(!m_gBuffer.Resize(m_graphicsDevice.GetDevice(), width, height, m_rtvDescriptorheap, m_resourceDescriptorHeap)){
        Log::Error("Failed to resize G-Buffer.");
        return;
    }

    m_width = width;
    m_height = height;
}

void Renderer::Draw(ID3D12GraphicsCommandList* commandList, const RenderObject& object)
{
    if(!object.mesh || !object.material || !object.material->pipeline || !object.material->texture){
        return;
    }

    object.material->pipeline->Bind(commandList);

    commandList->SetGraphicsRoot32BitConstant(0, object.material->texture->GetDescriptorIndex(), 0);

    object.mesh->Bind(commandList);

    commandList->DrawInstanced(static_cast<UINT>(object.mesh->GetVertexCount()), 1, 0, 0);
}


// --------------
// Rendering
// --------------

void Renderer::Render(const RenderObject& object)
{

    if(!m_commandContext.Begin()){
        Log::Error("failed to begin command recording.");
        return;
    }

    ID3D12GraphicsCommandList* commandList = m_commandContext.GetCommandList();

    D3D12_CPU_DESCRIPTOR_HANDLE albedoRTV = m_gBuffer.GetAlbedo().GetRTV();
    D3D12_CPU_DESCRIPTOR_HANDLE normalRTV = m_gBuffer.GetNormal().GetRTV();

    const float albedoClearColor[] = {
        1.0f,
        0.0f,
        1.0f,
        1.0f
    };

    const float normalClearColor[] = {
        0.0f,
        1.0f,
        0.0f,
        1.0f
    };

    m_gBuffer.GetAlbedo().Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_gBuffer.GetNormal().Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);

    commandList->ClearRenderTargetView(albedoRTV, albedoClearColor, 0, nullptr);
    commandList->ClearRenderTargetView(normalRTV, normalClearColor, 0, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_swapChain.GetCurrentBackBuffer();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    commandList->ResourceBarrier(1, &barrier);

    ID3D12DescriptorHeap* descriptorHeaps[] = {m_resourceDescriptorHeap.GetHeap()};
    commandList->SetDescriptorHeaps(1, descriptorHeaps);

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_swapChain.GetCurrentRTV();

    D3D12_CPU_DESCRIPTOR_HANDLE gBufferRTVs[] = {albedoRTV, normalRTV};

    commandList->OMSetRenderTargets(2, gBufferRTVs, FALSE, nullptr);

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

    const float clearColor[] = {
        0.1f,
        0.2f,
        0.4f,
        1.0f
    };

    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    Draw(commandList, object);

    m_gBuffer.GetAlbedo().Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_gBuffer.GetNormal().Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    m_compositePipeline.Bind(commandList);

    unsigned int gBufferIndices[] =
    {
        m_gBuffer.GetAlbedo().GetSRVIndex(),
        m_gBuffer.GetNormal().GetSRVIndex()
    };

    commandList->SetGraphicsRoot32BitConstants(0, 2, gBufferIndices, 0);
    commandList->DrawInstanced(3, 1, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

    commandList->ResourceBarrier(1, &barrier);

    if(!m_commandContext.End()){
        Log::Error("Failed to end command recording.");
        return;
    }

    m_commandContext.Execute();
    m_commandContext.WaitForGPU();

    if(!m_swapChain.Present()){
        Log::Error("Failed to present swap chain.");
    }
}


//Demo Scene
bool Renderer::InitializeDemoScene(DemoScene& scene)
{

    bool result = scene.Initialize(
        m_graphicsDevice.GetDevice(),
        m_resourceUploader,
        m_resourceDescriptorHeap
    );


    return result;
}


// --------------
// Shutdown
// --------------

void Renderer::Shutdown()
{
    m_commandContext.WaitForGPU();
    m_swapChain.Shutdown();
    m_commandContext.Shutdown();
    m_resourceUploader.Shutdown();

    m_gBuffer.Shutdown();
    m_compositePipeline.Shutdown();
    
    m_resourceDescriptorHeap.Shutdown();
    m_rtvDescriptorheap.Shutdown();
    m_graphicsDevice.Shutdown();
}