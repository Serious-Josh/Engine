#include "Core/Log.h"
#include "Renderer.h"
#include "DemoContent/DemoScene.h"
#include "Renderer/Texture.h"
#include "Material.h"
#include "Scene/Camera.h"
#include "ShaderData.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstring>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>


// --------------
// Initialization
// --------------

bool Renderer::Initialize(void* windowHandle, unsigned int width, unsigned int height, const std::filesystem::path& assetRoot)
{
    m_width = width;
    m_height = height;

    if(!InitializeDevice())
    {
        return false;
    }

    ID3D12Device* device = m_graphicsDevice.GetDevice();

    const std::filesystem::path shaderRoot = assetRoot / "Shaders";

    if(!m_shaderLibrary.Initialize(shaderRoot)){
        Log::Error("Failed to initialize shader library.");
        return false;
    }

    ShaderDescription compositeVertexDescription{};
    compositeVertexDescription.path = "Composite.hlsl";
    compositeVertexDescription.entryPoint = "VSMain";
    compositeVertexDescription.stage = ShaderStage::Vertex;

    Shader* compositeVertexShader = m_shaderLibrary.Load(compositeVertexDescription);

    if(!compositeVertexShader){
        Log::Error("Failed to load composite vertex shader.");
        return false;
    }

    ShaderDescription compositePixelDescription{};
    compositePixelDescription.path = "Composite.hlsl";
    compositePixelDescription.entryPoint = "PSMain";
    compositePixelDescription.stage = ShaderStage::Pixel;

    Shader* compositePixelShader = m_shaderLibrary.Load(compositePixelDescription);

    if(!compositePixelShader){
        Log::Error("Failed to load composite pixel shader.");
        return false;
    }

    PipelineDescription compositeDescription{};
    compositeDescription.vertexShader = compositeVertexShader;
    compositeDescription.pixelShader = compositePixelShader;
    compositeDescription.vertexLayout = nullptr;
    compositeDescription.renderTargets.count = 1;
    compositeDescription.renderTargets.formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    
    compositeDescription.rootParameters.push_back({RootParameterType::Constants, 0, 3, D3D12_SHADER_VISIBILITY_PIXEL});
    compositeDescription.rootParameters.push_back({RootParameterType::ConstantBuffer, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL});

    if(!m_compositePipeline.Initialize(device, compositeDescription)){
        Log::Error("Failed to init composite test pipeline.");
        return false;
    }

    if(!m_resourceUploader.Initialize(device)){
        Log::Error("Failed to init resource uploader.");
        return false;
    }

    if(!m_uploadBuffer.Initialize(device, 1024 * 1024)){
        Log::Error("Failed to init upload buffer.");
        return false;
    }

    if(!m_resourceDescriptorHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, true)){
        Log::Error("Failed to init resource descriptor heap.");
        return false;
    }

    if(!m_rtvDescriptorHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 16, false)){
        Log::Error("Failed to init rtv descriptor heap.");
        return false;
    }

    if(!m_dsvDescriptorHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 16, false)){
        Log::Error("Failed to init dsv descriptor heap.");
        return false;
    }

    if(!m_gBuffer.Initialize(device, m_width, m_height, m_rtvDescriptorHeap, m_dsvDescriptorHeap, m_resourceDescriptorHeap)){
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

    if(!m_gBuffer.Resize(m_graphicsDevice.GetDevice(), width, height, m_rtvDescriptorHeap, m_dsvDescriptorHeap, m_resourceDescriptorHeap)){
        Log::Error("Failed to resize G-Buffer.");
        return;
    }

    m_width = width;
    m_height = height;
}

void Renderer::Draw(ID3D12GraphicsCommandList* commandList, const RenderObject& object, D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress)
{
    if(!object.mesh || !object.material || !object.material->pipeline || !object.material->texture){
        return;
    }

    ObjectConstants objectConstants{};
    objectConstants.model = object.transform.GetModelMatrix();
    objectConstants.normalMatrix = objectConstants.model.Inverse().Transpose();

    D3D12_GPU_VIRTUAL_ADDRESS objectConstantsAddress = m_uploadBuffer.Allocate(&objectConstants, sizeof(ObjectConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(objectConstantsAddress == 0){
        Log::Error("Failed to allocate object constants.");
        return;
    }

    object.material->pipeline->Bind(commandList);

    commandList->SetGraphicsRoot32BitConstant(0, object.material->texture->GetDescriptorIndex(), 0);

    commandList->SetGraphicsRootConstantBufferView(1, cameraConstantsAddress);
    commandList->SetGraphicsRootConstantBufferView(2, objectConstantsAddress);

    object.mesh->Bind(commandList);

    commandList->DrawInstanced(static_cast<UINT>(object.mesh->GetVertexCount()), 1, 0, 0);
}


// --------------
// Deferred Passes
// --------------
void Renderer::BeginGeometryPass(ID3D12GraphicsCommandList* commandList)
{
    RenderTarget& albedo = m_gBuffer.GetAlbedo();
    RenderTarget& normal = m_gBuffer.GetNormal();
    DepthBuffer& depth = m_gBuffer.GetDepth();

    albedo.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    normal.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    depth.Transition(commandList, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    const float albedoClearColor[] =
    {
        1.0f, 0.0f, 1.0f, 1.0f
    };

    const float normalClearColor[] =
    {
        0.0f, 1.0f, 0.0f, 1.0f
    };

    D3D12_CPU_DESCRIPTOR_HANDLE renderTargets[] = {albedo.GetRTV(), normal.GetRTV()};

    commandList->ClearRenderTargetView(renderTargets[0], albedoClearColor, 0, nullptr);
    commandList->ClearRenderTargetView(renderTargets[1], normalClearColor, 0, nullptr);
    commandList->ClearDepthStencilView(depth.GetDSV(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    D3D12_CPU_DESCRIPTOR_HANDLE dsv = depth.GetDSV();

    commandList->OMSetRenderTargets(2, renderTargets, FALSE, &dsv);
}

void Renderer::EndGeometryPass(ID3D12GraphicsCommandList* commandList)
{
    m_gBuffer.GetAlbedo().Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_gBuffer.GetNormal().Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_gBuffer.GetDepth().Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void Renderer::ExecuteCompositePass(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV, D3D12_GPU_VIRTUAL_ADDRESS reconstructionConstantsAddress)
{
    commandList->OMSetRenderTargets(1, &backBufferRTV, FALSE, nullptr);
    
    m_compositePipeline.Bind(commandList);

    unsigned int gBufferIndices[] =
    {
        m_gBuffer.GetAlbedo().GetSRVIndex(),
        m_gBuffer.GetNormal().GetSRVIndex(),
        m_gBuffer.GetDepth().GetSRVIndex()
    };

    commandList->SetGraphicsRoot32BitConstants(0, 3, gBufferIndices, 0);
    commandList->SetGraphicsRootConstantBufferView(1, reconstructionConstantsAddress);

    commandList->DrawInstanced(3, 1, 0, 0);
}


// --------------
// Rendering
// --------------

void Renderer::Render(const RenderObject& object, const Camera& camera)
{
    m_uploadBuffer.Reset();

    CameraConstants cameraConstants{};
    cameraConstants.view = camera.GetViewMatrix();
    cameraConstants.projection = camera.GetProjectionMatrix();

    D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress = m_uploadBuffer.Allocate(&cameraConstants, sizeof(CameraConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(cameraConstantsAddress == 0){
        Log::Error("Failed to allocate camera constants.");
        return;
    }

    ReconstructionConstants reconstructionConstants{};
    const Matrix4 viewProjection = cameraConstants.projection * cameraConstants.view;
    reconstructionConstants.inverseViewProjection = viewProjection.Inverse();

    D3D12_GPU_VIRTUAL_ADDRESS reconstructionConstantsAddress = m_uploadBuffer.Allocate(&reconstructionConstants, sizeof(ReconstructionConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(reconstructionConstantsAddress == 0){
        Log::Error("Failed to allocate reconstruction constants.");
        return;
    }

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

    ID3D12DescriptorHeap* descriptorHeaps[] = {m_resourceDescriptorHeap.GetHeap()};
    commandList->SetDescriptorHeaps(1, descriptorHeaps);

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_swapChain.GetCurrentRTV();

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

    BeginGeometryPass(commandList);
    Draw(commandList, object, cameraConstantsAddress);
    EndGeometryPass(commandList);
    ExecuteCompositePass(commandList, rtvHandle, reconstructionConstantsAddress);

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
    ShaderDescription vertexShaderDescription{};
    vertexShaderDescription.path = "Triangle.hlsl";
    vertexShaderDescription.entryPoint = "VSMain";
    vertexShaderDescription.stage = ShaderStage::Vertex;

    Shader* vertexShader = m_shaderLibrary.Load(vertexShaderDescription);

    if(!vertexShader){
        Log::Error("Failed to load demo vertex shader.");
        return false;
    }

    ShaderDescription pixelShaderDescription{};
    pixelShaderDescription.path = "Triangle.hlsl";
    pixelShaderDescription.entryPoint = "PSMain";
    pixelShaderDescription.stage = ShaderStage::Pixel;

    Shader* pixelShader = m_shaderLibrary.Load(pixelShaderDescription);

    if(!pixelShader){
        Log::Error("Failed to load demo pixel shader.");
        return false;
    }

    bool result = scene.Initialize(m_graphicsDevice.GetDevice(), m_resourceUploader, m_resourceDescriptorHeap, vertexShader, pixelShader);

    if(!result){
        return false;
    }

    scene.SetAspectRatio(static_cast<float>(m_width) / static_cast<float>(m_height));

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
    
    m_uploadBuffer.Shutdown();
    m_resourceDescriptorHeap.Shutdown();
    m_rtvDescriptorHeap.Shutdown();
    m_dsvDescriptorHeap.Shutdown();

    m_shaderLibrary.Shutdown();

    m_graphicsDevice.Shutdown();
}