#include "Renderer.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <fstream>
#include <vector>
#include <cstring>
#include <dxgi1_6.h>
#include <d3d12.h>


struct Vertex
{
    float x;
    float y;
    float z;
};


// --------------
// Helpers
// --------------

static std::vector<char> LoadBinaryFile(const char* path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);

    if(!file){
        return {};
    }

    const std::streamsize size = file.tellg();

    if(size <= 0){
        return {};
    }

    std::vector<char> data(static_cast<size_t>(size));

    file.seekg(0, std::ios::beg);
    file.read(data.data(), size);

    if(!file){
        return {};
    }

    return data;
}


// --------------
// Initialization
// --------------

bool Renderer::Initialize(void* windowHandle, unsigned int width, unsigned int height)
{
    #if defined(_DEBUG)
        {
            ID3D12Debug* debugController = nullptr;

            if(SUCCEEDED(D3D12GetDebugInterface(
                IID_PPV_ARGS(&debugController))))
            {
                debugController->EnableDebugLayer();
                debugController->Release();
            }
        }
    #endif

    m_width = width;
    m_height = height;

    if(!InitializeDevice())
    {
        return false;
    }

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
    // Getting graphics adapter and creating D3D12 device

    HRESULT result = CreateDXGIFactory2(
        0,
        IID_PPV_ARGS(&m_factory)
    );

    if(FAILED(result))
    {
        return false;
    }

    IDXGIAdapter1* adapter = nullptr;

    for(
        UINT index = 0;
        m_factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND;
        ++index
    )
    {
        DXGI_ADAPTER_DESC1 description{};

        adapter->GetDesc1(&description);

        if(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            adapter->Release();
            adapter = nullptr;
            continue;
        }

        result = D3D12CreateDevice(
            adapter,
            D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(&m_device)
        );

        if(SUCCEEDED(result))
        {
            break;
        }

        adapter->Release();
        adapter = nullptr;
    }

    if(adapter)
    {
        adapter->Release();
    }

    if(m_device == nullptr)
    {
        return false;
    }

    // Creating the command queue

    D3D12_COMMAND_QUEUE_DESC queueDescription{};

    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDescription.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDescription.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDescription.NodeMask = 0;

    result = m_device->CreateCommandQueue(
        &queueDescription,
        IID_PPV_ARGS(&m_commandQueue)
    );

    if(FAILED(result))
    {
        return false;
    }

    // Creating command allocator

    result = m_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_commandAllocator)
    );

    if(FAILED(result))
    {
        return false;
    }

    // Creating command list

    result = m_device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocator,
        nullptr,
        IID_PPV_ARGS(&m_commandList)
    );

    if(FAILED(result))
    {
        return false;
    }

    result = m_commandList->Close();

    if(FAILED(result))
    {
        return false;
    }

    // Creating fence

    result = m_device->CreateFence(
        0,
        D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&m_fence)
    );

    if(FAILED(result))
    {
        return false;
    }

    m_fenceValue = 1;

    m_fenceEvent = CreateEventW(
        nullptr,
        FALSE,
        FALSE,
        nullptr
    );

    if(m_fenceEvent == nullptr)
    {
        return false;
    }

    return true;
}

bool Renderer::InitializeSwapChain(void* windowHandle)
{
    // Creating the swap chain

    DXGI_SWAP_CHAIN_DESC1 swapChainDescription{};

    swapChainDescription.Width = m_width;
    swapChainDescription.Height = m_height;
    swapChainDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDescription.Stereo = FALSE;
    swapChainDescription.SampleDesc.Count = 1;
    swapChainDescription.SampleDesc.Quality = 0;
    swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDescription.BufferCount = 2;
    swapChainDescription.Scaling = DXGI_SCALING_STRETCH;
    swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDescription.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapChainDescription.Flags = 0;

    IDXGISwapChain1* swapChain = nullptr;

    HRESULT result = m_factory->CreateSwapChainForHwnd(
        m_commandQueue,
        static_cast<HWND>(windowHandle),
        &swapChainDescription,
        nullptr,
        nullptr,
        &swapChain
    );

    if(FAILED(result))
    {
        return false;
    }

    result = swapChain->QueryInterface(
        IID_PPV_ARGS(&m_swapChain)
    );

    swapChain->Release();

    if(FAILED(result))
    {
        return false;
    }

    // Creating RTV heap for swap chain back buffers

    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};

    heapDescription.NumDescriptors = 2;
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    heapDescription.NodeMask = 0;

    result = m_device->CreateDescriptorHeap(
        &heapDescription,
        IID_PPV_ARGS(&m_renderTargetHeap)
    );

    if(FAILED(result))
    {
        return false;
    }

    // Retrieving back buffers

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        m_renderTargetHeap->GetCPUDescriptorHandleForHeapStart();

    m_rtvDescriptorSize =
        m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV
        );

    for(UINT index = 0; index < 2; ++index)
    {
        result = m_swapChain->GetBuffer(
            index,
            IID_PPV_ARGS(&m_backBuffers[index])
        );

        if(FAILED(result))
        {
            return false;
        }

        m_device->CreateRenderTargetView(
            m_backBuffers[index],
            nullptr,
            rtvHandle
        );

        rtvHandle.ptr += m_rtvDescriptorSize;
    }

    return true;
}

bool Renderer::InitializePipeline()
{
    // Creating root signature

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDescription{};

    rootSignatureDescription.NumParameters = 0;
    rootSignatureDescription.pParameters = nullptr;
    rootSignatureDescription.NumStaticSamplers = 0;
    rootSignatureDescription.pStaticSamplers = nullptr;
    rootSignatureDescription.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* rootSignatureBlob = nullptr;
    ID3DBlob* rootSignatureError = nullptr;

    HRESULT result = D3D12SerializeRootSignature(
        &rootSignatureDescription,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &rootSignatureBlob,
        &rootSignatureError
    );

    if(FAILED(result))
    {
        if(rootSignatureError)
        {
            rootSignatureError->Release();
        }

        return false;
    }

    result = m_device->CreateRootSignature(
        0,
        rootSignatureBlob->GetBufferPointer(),
        rootSignatureBlob->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature)
    );

    rootSignatureBlob->Release();

    if(rootSignatureError)
    {
        rootSignatureError->Release();
    }

    if(FAILED(result))
    {
        return false;
    }

    // Loading shaders

    const std::vector<char> vertexShader =
        LoadBinaryFile("src/Renderer/Shaders/TriangleVS.cso");

    const std::vector<char> pixelShader =
        LoadBinaryFile("src/Renderer/Shaders/TrianglePS.cso");

    if(vertexShader.empty() || pixelShader.empty())
    {
        return false;
    }

    // Creating PSO

    D3D12_INPUT_ELEMENT_DESC inputElement{};

    inputElement.SemanticName = "POSITION";
    inputElement.SemanticIndex = 0;
    inputElement.Format = DXGI_FORMAT_R32G32B32_FLOAT;
    inputElement.InputSlot = 0;
    inputElement.AlignedByteOffset = 0;
    inputElement.InputSlotClass =
        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    inputElement.InstanceDataStepRate = 0;

    D3D12_RASTERIZER_DESC rasterizerDescription{};

    rasterizerDescription.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDescription.CullMode = D3D12_CULL_MODE_BACK;
    rasterizerDescription.FrontCounterClockwise = FALSE;
    rasterizerDescription.DepthBias = 0;
    rasterizerDescription.DepthBiasClamp = 0.0f;
    rasterizerDescription.SlopeScaledDepthBias = 0.0f;
    rasterizerDescription.DepthClipEnable = TRUE;
    rasterizerDescription.MultisampleEnable = FALSE;
    rasterizerDescription.AntialiasedLineEnable = FALSE;
    rasterizerDescription.ForcedSampleCount = 0;
    rasterizerDescription.ConservativeRaster =
        D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_BLEND_DESC blendDescription{};

    blendDescription.AlphaToCoverageEnable = FALSE;
    blendDescription.IndependentBlendEnable = FALSE;

    blendDescription.RenderTarget[0].BlendEnable = FALSE;
    blendDescription.RenderTarget[0].LogicOpEnable = FALSE;
    blendDescription.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blendDescription.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    blendDescription.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendDescription.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDescription.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blendDescription.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDescription.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    blendDescription.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_DEPTH_STENCIL_DESC depthStencilDescription{};

    depthStencilDescription.DepthEnable = FALSE;
    depthStencilDescription.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    depthStencilDescription.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depthStencilDescription.StencilEnable = FALSE;
    depthStencilDescription.StencilReadMask =
        D3D12_DEFAULT_STENCIL_READ_MASK;
    depthStencilDescription.StencilWriteMask =
        D3D12_DEFAULT_STENCIL_WRITE_MASK;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDescription{};

    pipelineDescription.pRootSignature = m_rootSignature;

    pipelineDescription.VS.pShaderBytecode = vertexShader.data();
    pipelineDescription.VS.BytecodeLength = vertexShader.size();

    pipelineDescription.PS.pShaderBytecode = pixelShader.data();
    pipelineDescription.PS.BytecodeLength = pixelShader.size();

    pipelineDescription.BlendState = blendDescription;
    pipelineDescription.SampleMask = UINT_MAX;
    pipelineDescription.RasterizerState = rasterizerDescription;
    pipelineDescription.DepthStencilState = depthStencilDescription;

    pipelineDescription.InputLayout.pInputElementDescs =
        &inputElement;
    pipelineDescription.InputLayout.NumElements = 1;

    pipelineDescription.PrimitiveTopologyType =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    pipelineDescription.NumRenderTargets = 1;
    pipelineDescription.RTVFormats[0] =
        DXGI_FORMAT_R8G8B8A8_UNORM;

    pipelineDescription.SampleDesc.Count = 1;

    result = m_device->CreateGraphicsPipelineState(
        &pipelineDescription,
        IID_PPV_ARGS(&m_pipelineState)
    );

    if(FAILED(result))
    {
        return false;
    }

    return true;
}

bool Renderer::InitializeGeometry()
{
    const Vertex vertices[] =
    {
        {0.0f, 0.5f, 0.0f},
        {0.5f, -0.5f, 0.0f},
        {-0.5f, -0.5f, 0.0f}
    };

    // Creating vertex buffer

    D3D12_HEAP_PROPERTIES heapProperties{};

    heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC resourceDescription{};

    resourceDescription.Dimension =
        D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDescription.Width = sizeof(vertices);
    resourceDescription.Height = 1;
    resourceDescription.DepthOrArraySize = 1;
    resourceDescription.MipLevels = 1;
    resourceDescription.Format = DXGI_FORMAT_UNKNOWN;
    resourceDescription.SampleDesc.Count = 1;
    resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT result = m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDescription,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_vertexBuffer)
    );

    if(FAILED(result))
    {
        return false;
    }

    // Putting vertices into buffer

    void* mappedData = nullptr;

    D3D12_RANGE readRange{};

    readRange.Begin = 0;
    readRange.End = 0;

    result = m_vertexBuffer->Map(
        0,
        &readRange,
        &mappedData
    );

    if(FAILED(result))
    {
        return false;
    }

    memcpy(
        mappedData,
        vertices,
        sizeof(vertices)
    );

    m_vertexBuffer->Unmap(0, nullptr);

    m_vertexBufferView.BufferLocation =
        m_vertexBuffer->GetGPUVirtualAddress();

    m_vertexBufferView.StrideInBytes = sizeof(Vertex);
    m_vertexBufferView.SizeInBytes = sizeof(vertices);

    return true;
}


// --------------
// Other Functions Before Render
// --------------

void Renderer::WaitForGPU()
{
    const unsigned int long long fenceValue = m_fenceValue;

    HRESULT result = m_commandQueue->Signal(m_fence, fenceValue);

    if(FAILED(result)){
        return;
    }

    m_fenceValue++;

    if(m_fence->GetCompletedValue() < fenceValue){
        result = m_fence->SetEventOnCompletion(fenceValue, static_cast<HANDLE>(m_fenceEvent));

        if(FAILED(result)){
            return;
        }

        WaitForSingleObject(static_cast<HANDLE>(m_fenceEvent), INFINITE);
    }
}

void Renderer::Resize(unsigned int width, unsigned int height)
{
    if(width == 0 || height == 0){
        return;
    }

    WaitForGPU();

    for(ID3D12Resource*& BackBuffer : m_backBuffers){
        if(BackBuffer){
            BackBuffer->Release();
            BackBuffer = nullptr;
        }
    }

    HRESULT result = m_swapChain->ResizeBuffers(2, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);

    if(FAILED(result)){
        return;
    }

    m_width = width;
    m_height = height;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_renderTargetHeap->GetCPUDescriptorHandleForHeapStart();

    for(UINT index = 0; index < 0; ++index){
        result = m_swapChain->GetBuffer(index, IID_PPV_ARGS(&m_backBuffers[index]));

        if(FAILED(result)){
            return;
        }

        m_device->CreateRenderTargetView(m_backBuffers[index], nullptr, rtvHandle);

        rtvHandle.ptr += m_rtvDescriptorSize;
    }

    m_currentBackBuffer = m_swapChain->GetCurrentBackBufferIndex();
}


// --------------
// Rendering
// --------------

void Renderer::Render()
{
    HRESULT result = m_commandAllocator->Reset();

    if(FAILED(result))
    {
        return;
    }

    result = m_commandList->Reset(
        m_commandAllocator,
        nullptr
    );

    if(FAILED(result))
    {
        return;
    }

    ID3D12Resource* backBuffer =
        m_backBuffers[m_currentBackBuffer];

    D3D12_RESOURCE_BARRIER barrier{};

    barrier.Type =
        D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags =
        D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource =
        backBuffer;
    barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore =
        D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_RENDER_TARGET;

    m_commandList->ResourceBarrier(1, &barrier);

    m_commandList->SetPipelineState(m_pipelineState);
    m_commandList->SetGraphicsRootSignature(m_rootSignature);

    m_commandList->IASetPrimitiveTopology(
        D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
    );

    m_commandList->IASetVertexBuffers(
        0,
        1,
        &m_vertexBufferView
    );

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        m_renderTargetHeap->GetCPUDescriptorHandleForHeapStart();

    rtvHandle.ptr += static_cast<SIZE_T>(
        m_currentBackBuffer * m_rtvDescriptorSize
    );

    m_commandList->OMSetRenderTargets(
        1,
        &rtvHandle,
        FALSE,
        nullptr
    );

    D3D12_VIEWPORT viewport{};

    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    m_commandList->RSSetViewports(
        1,
        &viewport
    );

    D3D12_RECT scissorRect{};

    scissorRect.left = 0;
    scissorRect.top = 0;
    scissorRect.right = static_cast<LONG>(m_width);
    scissorRect.bottom = static_cast<LONG>(m_height);

    m_commandList->RSSetScissorRects(
        1,
        &scissorRect
    );

    const float clearColor[] =
    {
        0.1f,
        0.2f,
        0.4f,
        1.0f
    };

    m_commandList->ClearRenderTargetView(
        rtvHandle,
        clearColor,
        0,
        nullptr
    );

    m_commandList->DrawInstanced(
        3,
        1,
        0,
        0
    );

    barrier.Transition.StateBefore =
        D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_PRESENT;

    m_commandList->ResourceBarrier(
        1,
        &barrier
    );

    result = m_commandList->Close();

    if(FAILED(result))
    {
        return;
    }

    ID3D12CommandList* commandLists[] =
    {
        m_commandList
    };

    m_commandQueue->ExecuteCommandLists(
        1,
        commandLists
    );

    WaitForGPU();

    m_swapChain->Present(1, 0);

    m_currentBackBuffer =
        m_swapChain->GetCurrentBackBufferIndex();
}

// --------------
// Shutdown
// --------------

void Renderer::Shutdown()
{
    if(m_fence)
    {
        m_fence->Release();
        m_fence = nullptr;
    }

    if(m_fenceEvent)
    {
        CloseHandle(static_cast<HANDLE>(m_fenceEvent));
        m_fenceEvent = nullptr;
    }

    for(ID3D12Resource*& backBuffer : m_backBuffers)
    {
        if(backBuffer)
        {
            backBuffer->Release();
            backBuffer = nullptr;
        }
    }

    if(m_renderTargetHeap)
    {
        m_renderTargetHeap->Release();
        m_renderTargetHeap = nullptr;
    }

    if(m_swapChain)
    {
        m_swapChain->Release();
        m_swapChain = nullptr;
    }

    if(m_commandQueue)
    {
        m_commandQueue->Release();
        m_commandQueue = nullptr;
    }

    if(m_commandList)
    {
        m_commandList->Release();
        m_commandList = nullptr;
    }

    if(m_commandAllocator)
    {
        m_commandAllocator->Release();
        m_commandAllocator = nullptr;
    }

    if(m_rootSignature)
    {
        m_rootSignature->Release();
        m_rootSignature = nullptr;
    }

    if(m_vertexBuffer)
    {
        m_vertexBuffer->Release();
        m_vertexBuffer = nullptr;
    }

    if(m_pipelineState)
    {
        m_pipelineState->Release();
        m_pipelineState = nullptr;
    }

    if(m_device)
    {
        m_device->Release();
        m_device = nullptr;
    }

    if(m_factory)
    {
        m_factory->Release();
        m_factory = nullptr;
    }
}