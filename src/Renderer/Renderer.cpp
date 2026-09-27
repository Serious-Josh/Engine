#include "Core/Log.h"
#include "Renderer.h"
#include "Triangle.h"

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

    m_triangleVertexLayout = CreateTriangleVertexLayout();

    if(!InitializeDevice())
    {
        return false;
    }

    ID3D12Device* device = m_graphicsDevice.GetDevice();


    //texture testing
    const unsigned int texturePixels[] =
    {
        0xFFFF0000,
        0xFF00FF00,
        0xFF0000FF,
        0xFFFFFFFF
    };

    if(!m_testTexture.Initialize(device, texturePixels, 2, 2)){
        Log::Error("Failed to init test texture.");
        return false;
    }

    if(!m_resourceUploader.Initialize(device))
    {
        Log::Error("Failed to initialize resource uploader.");
        return false;
    }

    if(!m_resourceUploader.UploadTexture(m_testTexture.GetResource(), m_testTexture.GetInitialData(), m_testTexture.GetInitialDataSize(), 2, 2)){
        Log::Error("Failed to upload test texture.");
        return false;
    }

    if(!m_resourceDescriptorHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, true)){
        Log::Error("Failed to init resource descriptor heap.");
        return false;
    }

    if(!m_testTexture.CreateSRV(device, m_resourceDescriptorHeap, 0)){
        Log::Error("Failed to create test texture SRV.");
        return false;
    }


    //continue init
    if(!InitializeSwapChain(windowHandle))
    {
        return false;
    }

    if(!InitializePipeline())
    {
        return false;
    }

    if(!InitializeGeometry())
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

bool Renderer::InitializePipeline()
{
    return m_pipeline.Initialize(m_graphicsDevice.GetDevice(), ENGINE_SHADER_DIR "/TriangleVS.cso", ENGINE_SHADER_DIR "/TrianglePS.cso", m_triangleVertexLayout);
}

bool Renderer::InitializeGeometry()
{
    if(!m_triangleMesh.Initialize(m_graphicsDevice.GetDevice(), TriangleVertices, 3, m_triangleVertexLayout))
    {
        return false;
    }

    return true;
}

//Resize function

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

    m_width = width;
    m_height = height;
}


// --------------
// Rendering
// --------------

void Renderer::Render()
{
    if(!m_commandContext.Begin()){
        Log::Error("failed to begin command recording.");
        return;
    }

    ID3D12GraphicsCommandList* commandList = m_commandContext.GetCommandList();

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_swapChain.GetCurrentBackBuffer();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    commandList->ResourceBarrier(1, &barrier);

    m_pipeline.Bind(commandList);

    ID3D12DescriptorHeap* descriptorHeaps[] = {m_resourceDescriptorHeap.GetHeap()};
    commandList->SetDescriptorHeaps(1, descriptorHeaps);
    commandList->SetGraphicsRootDescriptorTable(0, m_resourceDescriptorHeap.GetGPUHandle(0));

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_triangleMesh.Bind(commandList);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_swapChain.GetCurrentRTV();

    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

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


// --------------
// Shutdown
// --------------

void Renderer::Shutdown()
{
    m_commandContext.WaitForGPU();

    ///

    m_pipeline.Shutdown();
    m_triangleMesh.Shutdown();

    ///

    m_swapChain.Shutdown();

    ///

    m_commandContext.Shutdown();

    ///

    m_resourceUploader.Shutdown();
    m_resourceDescriptorHeap.Shutdown();
    m_testTexture.Shutdown();

    ///

    m_graphicsDevice.Shutdown();
}