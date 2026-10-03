#include "Core/Log.h"
#include "Renderer.h"
#include "Renderer/Texture.h"
#include "Material.h"
#include "Scene/Camera.h"
#include "ShaderData.h"
#include "Scene/DirectionalLight.h"
#include "Scene/Scene.h"
#include "ImageLoader.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <string>
#include <cstring>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <algorithm>


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

    if(!m_deferredLightingPass.Initialize(device, m_shaderLibrary)){
        Log::Error("Failed to initialize deferred lighting pass.");
        return false;
    }

    if(!m_toneMapPass.Initialize(device, m_shaderLibrary)){
        Log::Error("Failed to initialize tone map pass.");
        return false;
    }

    if(!m_resourceUploader.Initialize(device)){
        Log::Error("Failed to init resource uploader.");
        return false;
    }

    if(!m_uploadBuffer.Initialize(device, 1024 * 1024, 2)){
        Log::Error("Failed to init upload buffer.");
        return false;
    }

    if(!m_resourceDescriptorHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 256, true)){
        Log::Error("Failed to init resource descriptor heap.");
        return false;
    }

    if(!m_rtvDescriptorHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 128, false)){
        Log::Error("Failed to init rtv descriptor heap.");
        return false;
    }

    if(!m_brdfIntegrationLUT.Initialize(device, 512, 512, 1, 1, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET)){
        Log::Error("Failed to init BRDF integration LUT.");
        return false;
    }

    if(!m_brdfIntegrationLUT.CreateSRV(device, m_resourceDescriptorHeap)){
        Log::Error("Failed to create BRDF integration LUT SRV.");
        return false;
    }

    if(!m_brdfIntegrationPass.Initialize(device, m_shaderLibrary, m_rtvDescriptorHeap, m_brdfIntegrationLUT)){
        Log::Error("Failed to init BRDF integration pass.");
        return false;
    }

    const std::filesystem::path environmentPath =
    assetRoot / "Textures" / "Environment" / "TestEnvironment.hdr";

    if(!m_environment.Initialize(
        device,
        environmentPath,
        m_resourceUploader,
        m_resourceDescriptorHeap))
    {
        Log::Error("Failed to initialize environment map.");
        return false;
    }

    if(!m_environmentMipPass.Initialize(
        device,
        m_shaderLibrary,
        m_rtvDescriptorHeap,
        m_environment.GetSourceTexture()))
    {
        Log::Error(
            "Failed to initialize environment mip pass."
        );
        return false;
    }

    if(!m_equirectangularToCubemapPass.Initialize(
        device,
        m_shaderLibrary,
        m_rtvDescriptorHeap,
        m_environment.GetCubemap()))
    {
        Log::Error(
            "Failed to initialize equirectangular-to-cubemap pass."
        );
        return false;
    }

    if(!m_diffuseIrradiancePass.Initialize(device, m_shaderLibrary, m_rtvDescriptorHeap, m_environment.GetDiffuseIrradiance())){
        Log::Error("Failed to init diffuse irradiance pass.");
        return false;
    }

    if(!m_commandContext.Begin(0)){
        Log::Error(
            "Failed to begin environment preprocessing."
        );
        return false;
    }

    ID3D12GraphicsCommandList* preprocessingCommandList =
        m_commandContext.GetCommandList();

    ID3D12DescriptorHeap* descriptorHeaps[] =
    {
        m_resourceDescriptorHeap.GetHeap()
    };

    preprocessingCommandList->SetDescriptorHeaps(
        1,
        descriptorHeaps
    );

    m_environmentMipPass.Execute(
        preprocessingCommandList,
        m_environment.GetSourceTexture()
    );

    m_diffuseIrradiancePass.Execute(preprocessingCommandList, m_environment.GetSourceTexture(), m_environment.GetCubemap());

    m_equirectangularToCubemapPass.Execute(
        preprocessingCommandList,
        m_environment.GetSourceTexture(),
        m_environment.GetCubemap()
    );

    m_brdfIntegrationPass.Execute(preprocessingCommandList, m_brdfIntegrationLUT);

    if(!m_commandContext.End()){
        Log::Error(
            "Failed to end environment preprocessing."
        );
        return false;
    }

    if(!m_commandContext.Execute(0)){
        Log::Error(
            "Failed to execute environment preprocessing."
        );
        return false;
    }

    m_commandContext.WaitForGPU();

    if(!m_sceneColor.Initialize(device, m_width, m_height, DXGI_FORMAT_R16G16B16A16_FLOAT, m_rtvDescriptorHeap, m_resourceDescriptorHeap)){
        Log::Error("Failed to init HDR scene color");
        return false;
    }

    if(!m_dsvDescriptorHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 16, false)){
        Log::Error("Failed to init dsv descriptor heap.");
        return false;
    }

    if(!m_debugUI.Initialize(static_cast<HWND>(windowHandle), device, m_commandContext.GetCommandQueue(), m_resourceDescriptorHeap)){
        Log::Error("Failed to init debug UI.");
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

    m_sceneColor.Shutdown();

    if(!m_sceneColor.Initialize(m_graphicsDevice.GetDevice(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, m_rtvDescriptorHeap, m_resourceDescriptorHeap)){
        Log::Error("Failed to resize HDR scene color.");
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

    MaterialConstants materialConstants{};
    materialConstants.textureIndex = object.material->texture->GetDescriptorIndex();
    materialConstants.roughness = object.material->roughness;
    materialConstants.materialIndex = object.material->gpuIndex;
    materialConstants.baseColor[0] = object.material->baseColor[0];
    materialConstants.baseColor[1] = object.material->baseColor[1];
    materialConstants.baseColor[2] = object.material->baseColor[2];

    commandList->SetGraphicsRoot32BitConstants(0, 3, &materialConstants, 0);

    commandList->SetGraphicsRootConstantBufferView(1, cameraConstantsAddress);
    commandList->SetGraphicsRootConstantBufferView(2, objectConstantsAddress);

    object.mesh->Bind(commandList);

    if(object.mesh->IsIndexed()){
        commandList->DrawIndexedInstanced(static_cast<UINT>(object.mesh->GetIndexCount()), 1, 0, 0, 0);
    }
    else{
        commandList->DrawInstanced(static_cast<UINT>(object.mesh->GetVertexCount()), 1, 0, 0);
    }
}


// --------------
// Deferred Passes
// --------------
void Renderer::BeginGeometryPass(ID3D12GraphicsCommandList* commandList)
{
    RenderTarget& albedo = m_gBuffer.GetMaterial();
    RenderTarget& normal = m_gBuffer.GetSurface();
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
    m_gBuffer.GetMaterial().Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_gBuffer.GetSurface().Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_gBuffer.GetDepth().Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

bool Renderer::InitializeMaterials(const std::vector<Material*>& materials)
{
    if(materials.empty()){
        Log::Error("Cannot init an empty material buffer.");
        return false;
    }

    std::vector<GPUMaterial> gpuMaterials;
    gpuMaterials.reserve(materials.size());

    for(size_t i = 0; i < materials.size(); ++i){
        if(!materials[i]){
            Log::Error("Cannot init null material.");
            return false;
        }

        materials[i]->gpuIndex = static_cast<unsigned int>(i);
        gpuMaterials.push_back(materials[i]->GetGPUData());
    }

    if(!m_materialBuffer.Initialize(m_graphicsDevice.GetDevice(), m_resourceUploader, gpuMaterials.data(), sizeof(GPUMaterial), gpuMaterials.size(), m_resourceDescriptorHeap)){
        Log::Error("Failed to init material buffer.");
        return false;
    }

    return true;
}


// --------------
// Rendering
// --------------

void Renderer::Render(const Scene& scene)
{
    m_debugUI.BeginFrame();

    const unsigned int frameIndex = m_swapChain.GetCurrentBackBufferIndex();

    if(!m_commandContext.Begin(frameIndex)){
        Log::Error("failed to begin command recording.");
        return;
    }

    m_uploadBuffer.Reset(frameIndex);

    const Camera& camera = scene.GetCamera();
    const DirectionalLight& light = scene.GetDirectionalLight();

    CameraConstants cameraConstants{};
    cameraConstants.viewProjection = camera.GetProjectionMatrix() * camera.GetViewMatrix();

    D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress = m_uploadBuffer.Allocate(&cameraConstants, sizeof(CameraConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(cameraConstantsAddress == 0){
        Log::Error("Failed to allocate camera constants.");
        return;
    }

    ReconstructionConstants reconstructionConstants{};
    reconstructionConstants.inverseViewProjection = cameraConstants.viewProjection.Inverse();

    const Vector3& cameraPosition = camera.GetPosition();

    reconstructionConstants.cameraPosition[0] = cameraPosition.x;
    reconstructionConstants.cameraPosition[1] = cameraPosition.y;
    reconstructionConstants.cameraPosition[2] = cameraPosition.z;
    reconstructionConstants.padding = 0.0f;

    D3D12_GPU_VIRTUAL_ADDRESS reconstructionConstantsAddress = m_uploadBuffer.Allocate(&reconstructionConstants, sizeof(ReconstructionConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(reconstructionConstantsAddress == 0){
        Log::Error("Failed to allocate reconstruction constants.");
        return;
    }

    DirectionalLightConstants lightConstants{};

    const Vector3 lightDirection = light.direction.Normalized();
    lightConstants.direction[0] = lightDirection.x;
    lightConstants.direction[1] = lightDirection.y;
    lightConstants.direction[2] = lightDirection.z;
    lightConstants.intensity = light.intensity;

    lightConstants.color[0] = light.color.x;
    lightConstants.color[1] = light.color.y;
    lightConstants.color[2] = light.color.z;
    lightConstants.ambientStrength = light.ambientStrength;

    D3D12_GPU_VIRTUAL_ADDRESS lightConstantsAddress = m_uploadBuffer.Allocate(&lightConstants, sizeof(DirectionalLightConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(lightConstantsAddress == 0){
        Log::Error("Failed to allocate directional light constants.");
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
    
    for(const RenderObject& object : scene.GetRenderObjects()){
        Draw(commandList, object, cameraConstantsAddress);
    }

    EndGeometryPass(commandList);


    m_deferredLightingPass.Execute(commandList, m_gBuffer, m_materialBuffer, m_environment.GetSourceTexture(), m_environment.GetCubemap(), m_environment.GetDiffuseIrradiance(), m_brdfIntegrationLUT, m_sceneColor, reconstructionConstantsAddress, lightConstantsAddress);
    m_toneMapPass.Execute(commandList, m_sceneColor, rtvHandle);

    m_debugUI.EndFrame(commandList);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

    commandList->ResourceBarrier(1, &barrier);

    if(!m_commandContext.End()){
        Log::Error("Failed to end command recording.");
        return;
    }

    if(!m_commandContext.Execute(frameIndex)){
        Log::Error("Failed to execute command list.");
        return;
    }

    if(!m_swapChain.Present()){
        Log::Error("Failed to present swap chain.");
    }
}


// --------------
// Shutdown
// --------------

void Renderer::Shutdown()
{
    Log::Info("Renderer shutdown: waiting for GPU...");
    m_commandContext.WaitForGPU();
    Log::Info("Renderer shutdown: GPU finished.");

    Log::Info("Renderer shutdown: shutting down DebugUI...");
    m_debugUI.Shutdown();
    Log::Info("Renderer shutdown: DebugUI finished.");

    Log::Info("Renderer shutdown: releasing renderer resources...");
    m_swapChain.Shutdown();
    m_commandContext.Shutdown();
    m_resourceUploader.Shutdown();

    m_gBuffer.Shutdown();
    m_deferredLightingPass.Shutdown();
    m_toneMapPass.Shutdown();
    m_environmentMipPass.Shutdown();
    m_equirectangularToCubemapPass.Shutdown();
    m_diffuseIrradiancePass.Shutdown();
    m_brdfIntegrationPass.Shutdown();
    
    m_uploadBuffer.Shutdown();
    m_materialBuffer.Shutdown();
    m_environment.Shutdown();
    m_sceneColor.Shutdown();

    m_brdfIntegrationLUT.Shutdown();

    m_resourceDescriptorHeap.Shutdown();
    m_rtvDescriptorHeap.Shutdown();
    m_dsvDescriptorHeap.Shutdown();

    m_shaderLibrary.Shutdown();
    Log::Info("Renderer shutdown: resources released.");

    m_graphicsDevice.Shutdown();
}

void Renderer::WaitForGPU()
{
    m_commandContext.WaitForGPU();
}

ID3D12Device* Renderer::GetDevice()
{
    return m_graphicsDevice.GetDevice();
}

ResourceUploader& Renderer::GetResourceUploader()
{
    return m_resourceUploader;
}

DescriptorHeap& Renderer::GetResourceDescriptorHeap()
{
    return m_resourceDescriptorHeap;
}

ShaderLibrary& Renderer::GetShaderLibrary()
{
    return m_shaderLibrary;
}