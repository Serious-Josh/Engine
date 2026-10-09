#include "Core/Log.h"
#include "Renderer.h"
#include "Renderer/Mesh.h"
#include "Renderer/Passes/EquirectangularToCubemapPass.h"
#include "Renderer/Passes/EnvironmentMipPass.h"
#include "Renderer/Passes/DiffuseIrradiancePass.h"
#include "Renderer/Passes/GGXMultiscatterIrradiancePass.h"
#include "Renderer/Texture.h"
#include "Material.h"
#include "Scene/Camera.h"
#include "ShaderData.h"
#include "Scene/DirectionalLight.h"
#include "Scene/Scene.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <string>
#include <d3d12.h>
#include <algorithm>
#include <cctype>
#include <iterator>
#include <utility>
#include <cstdlib>
#include <climits>


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

    if(!m_gpuTimestampProfiler.Initialize(m_graphicsDevice.GetDevice(), m_commandContext.GetCommandQueue())){
        Log::Error("Failed to init GPU timestamp profiler.");
        return false;
    }

    ID3D12Device* device = m_graphicsDevice.GetDevice();

    m_assetRoot = assetRoot;

    const std::filesystem::path shaderRoot = m_assetRoot / "Shaders";

    if(!DiscoverEnvironments()){
        return false;
    }

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

    if(!m_rtvDescriptorHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 256, false)){
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

    if(!m_ggxAverageAlbedoLUT.Initialize(device, 512, 1, 1, 1, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET)){
        Log::Error("Failed to init GGX average albedo LUT.");
        return false;
    }

    if(!m_ggxAverageAlbedoLUT.CreateSRV(device, m_resourceDescriptorHeap)){
        Log::Error("Failed to create GGX average albedo LUT SRV.");
        return false;
    }

    if(!m_ggxAverageAlbedoPass.Initialize(device, m_shaderLibrary, m_rtvDescriptorHeap, m_ggxAverageAlbedoLUT)){
        Log::Error("Failed to init GGX average albedo pass.");
        return false;
    }

    if(!m_rayTracedShadowPass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_width, m_height)){
        Log::Error("Faield to init ray traced shadow pass.");
        return false;
    }

    if(!m_rayTracedReflectionPass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_width, m_height)){
        Log::Error("Failed to init ray traced reflection pass.");
        return false;
    }

    if(!m_rayTracedGIPass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_width, m_height)){
        Log::Error("Failed to init ray traced gi pass.");
        return false;
    }

    if(!m_rayTracedAOPass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_width, m_height))
    {
        Log::Error("Failed to init ray traced AO pass.");
        return false;
    }

    if(!m_aoTemporalPass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_width, m_height))
    {
        Log::Error("Failed to init AO temporal pass.");
        return false;
    }

    if(!m_aoDenoisePass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_width, m_height))
    {
        Log::Error("Failed to init ray traced ao denoise pass.");
        return false;
    }

    if(!m_rayTracedReflectionTemporalPass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_width, m_height)){
        Log::Error("Failed to init ray traced reflection temporal pass.");
        return false;
    }

    if(!m_rayTracedReflectionDenoisePass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_width, m_height)){
        Log::Error("Failed to init ray traced reflection denoise pass.");
        return false;
    }

    if(!m_reflectionContributionLimitPass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_width, m_height))
    {
        Log::Error("Failed to initialize reflection contribution limiter.");
        return false;
    }

    if(!m_nrdReflectionPreparePass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_width, m_height))
    {
        Log::Error("Failed to initialize NRD reflection preparation pass.");
        return false;
    }

    if(!m_nrdReflectionResolvePass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_width, m_height))
    {
        Log::Error("Failed to initialize NRD reflection resolve pass.");
        return false;
    }

    if(!m_nrdDiffuseResolvePass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_width, m_height))
    {
        Log::Error("Failed to initialize NRD diffuse resolve pass.");
        return false;
    }

    if(!m_nrdReflectionDenoiser.Initialize(device, m_commandContext.GetCommandQueue(), m_resourceDescriptorHeap, m_width, m_height, 2)){
        Log::Error("Failed to init NRD reflection denoiser.");
        return false;
    }

    Log::Info("NRD REBLUR initialized.");
    Log::Info("NRD persistent memory: " + std::to_string(m_nrdReflectionDenoiser.GetPersistentMemoryUsageInMb()) + " MiB");
    Log::Info("NRD transient memory: " + std::to_string(m_nrdReflectionDenoiser.GetAliasableMemoryUsageInMb()) + " MiB");
    Log::Info("NRD total memory: " + std::to_string(m_nrdReflectionDenoiser.GetTotalMemoryUsageInMb()) + " MiB");

    if(!m_shadowDenoisePass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_width, m_height)){
        Log::Error("Failed to init shadow denoise pass.");
        return false;
    }

    if(m_availableEnvironments.empty()){
        Log::Error("No HDR environments were discovered.");
        return false;
    }

    m_currentEnvironmentIndex = 0;

    const EnvironmentAsset& defaultEnvironment = m_availableEnvironments[m_currentEnvironmentIndex];
    const std::filesystem::path environmentPath = m_assetRoot / defaultEnvironment.relativePath;

    if(!m_environment.Initialize(
        device,
        environmentPath,
        m_resourceUploader,
        m_resourceDescriptorHeap))
    {
        Log::Error("Failed to initialize environment: " + defaultEnvironment.displayName);

        m_environment.Shutdown();
        return false;
    }

    Log::Info("Initial environment: " + defaultEnvironment.displayName);

    if(!m_commandContext.Begin(0))
    {
        Log::Error("Failed to begin environment-independent LUT generation.");
        return false;
    }

    ID3D12GraphicsCommandList* lutCommandList = m_commandContext.GetCommandList();

    ID3D12DescriptorHeap* descriptorHeaps[] =
    {
        m_resourceDescriptorHeap.GetHeap()
    };

    lutCommandList->SetDescriptorHeaps(1, descriptorHeaps);

    m_brdfIntegrationPass.Execute(lutCommandList, m_brdfIntegrationLUT);

    m_ggxAverageAlbedoPass.Execute(lutCommandList, m_ggxAverageAlbedoLUT);

    if(!m_commandContext.End())
    {
        Log::Error("Failed to end environment-independent LUT generation.");
        return false;
    }

    const bool lutSubmitted = m_commandContext.Execute(0);

    // Execute can fail to signal after submitting work; still attempt to drain it.
    if(!m_commandContext.WaitForGPU()){
        Log::Error("Failed to confirm GPU completion after LUT generation.");
        return false;
    }
    if(!lutSubmitted){
        Log::Error("Failed to execute or signal environment-independent LUT generation.");
        return false;
    }

    if(!PreprocessEnvironment(m_environment))
    {
        Log::Error("Failed to preprocess environment.");
        return false;
    }

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

    if(!m_selectionMask.Initialize(device, m_width, m_height, DXGI_FORMAT_R8_UNORM, m_rtvDescriptorHeap, m_resourceDescriptorHeap)){
        Log::Error("Failed to init selection mask.");
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


// Resize and geometry drawing
bool Renderer::Resize(unsigned int width, unsigned int height)
{
    if(width == 0 || height == 0){
        return true;
    }

    if(!m_commandContext.WaitForGPU()){
        Log::Error("Failed to confirm GPU completion before resize.");
        return false;
    }

    if(!m_swapChain.Resize(width, height)){
        Log::Error("Failed to resize swap chain.");
        return false;
    }

    if(!m_gBuffer.Resize(m_graphicsDevice.GetDevice(), width, height, m_rtvDescriptorHeap, m_dsvDescriptorHeap, m_resourceDescriptorHeap)){
        Log::Error("Failed to resize G-Buffer.");
        return false;
    }

    if(!m_rayTracedShadowPass.Resize(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, width, height)){
        Log::Error("Failed to resize ray traced shadow pass.");
        return false;
    }

    if(!m_rayTracedReflectionPass.Resize(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, width, height)){
        Log::Error("Failed to resize ray traced reflection pass.");
        return false;
    }

    if(!m_rayTracedGIPass.Resize(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, width, height)){
        Log::Error("Failed to resize ray traced GI pass.");
        return false;
    }

    if(!m_rayTracedAOPass.Resize(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, width, height))
    {
        Log::Error("Failed to resize ray traced AO pass.");
        return false;
    }

    if(!m_aoTemporalPass.Resize(m_graphicsDevice, m_resourceDescriptorHeap, width, height))
    {
        Log::Error("Failed to resize AO temporal pass.");
        return false;
    }

    if(!m_aoDenoisePass.Resize(m_graphicsDevice, m_resourceDescriptorHeap, width, height))
    {
        Log::Error("Failed to resize AO denoise pass.");
        return false;
    }

    if(!m_rayTracedReflectionTemporalPass.Resize(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, width, height))
    {
        Log::Error("Failed to resize ray traced reflection temporal pass.");
        return false;
    }

    if(!m_reflectionContributionLimitPass.Resize(m_graphicsDevice, m_resourceDescriptorHeap, width, height))
    {
        Log::Error("Failed to resize reflection contribution limiter.");
        return false;
    }

    if(!m_nrdReflectionPreparePass.Resize(m_graphicsDevice, m_resourceDescriptorHeap, width, height))
    {
        Log::Error("Failed to resize NRD reflection preparation pass.");
        return false;
    }

    if(!m_nrdReflectionResolvePass.Resize(m_graphicsDevice, m_resourceDescriptorHeap, width, height))
    {
        Log::Error("Failed to resize NRD reflection resolve pass.");
        return false;
    }

    if(!m_nrdDiffuseResolvePass.Resize(m_graphicsDevice, m_resourceDescriptorHeap, width, height))
    {
        Log::Error("Failed to resize NRD diffuse resolve pass.");
        return false;
    }

    if(!m_nrdReflectionDenoiser.Resize(m_resourceDescriptorHeap, width, height)){
        Log::Error("Failed to resize NRD reflection denoiser.");
        return false;
    }

    m_hasPreviousNrdCamera = false;

    if(!m_rayTracedReflectionDenoisePass.Resize(m_graphicsDevice, m_resourceDescriptorHeap, width, height)){
        Log::Error("Failed to resize ray traced reflection denoise pass.");
        return false;
    }

    if(!m_shadowDenoisePass.Resize(m_graphicsDevice, m_resourceDescriptorHeap, width, height)){
        Log::Error("Failed to resize shadow denoise pass.");
        return false;
    }

    m_sceneColor.Shutdown();

    if(!m_sceneColor.Initialize(m_graphicsDevice.GetDevice(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, m_rtvDescriptorHeap, m_resourceDescriptorHeap)){
        Log::Error("Failed to resize HDR scene color.");
        return false;
    }

    m_selectionMask.Shutdown();

    if(!m_selectionMask.Initialize(m_graphicsDevice.GetDevice(), width, height, DXGI_FORMAT_R8_UNORM, m_rtvDescriptorHeap, m_resourceDescriptorHeap)){
        Log::Error("Failed to resize selection mask.");
        return false;
    }

    m_width = width;
    m_height = height;
    return true;
}

void Renderer::Draw(ID3D12GraphicsCommandList* commandList, const RenderObject& object, D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress)
{
    if(!object.mesh || !object.material || !object.material->pipeline || !object.material->texture){
        return;
    }

    ObjectConstants objectConstants{};
    objectConstants.model = object.transform.GetModelMatrix();
    objectConstants.previousModel = object.previousTransform.GetModelMatrix();
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
    materialConstants.normalTextureIndex = object.material->normalTexture ? object.material->normalTexture->GetDescriptorIndex() : UINT_MAX;;
    materialConstants.baseColor[0] = object.material->baseColor[0];
    materialConstants.baseColor[1] = object.material->baseColor[1];
    materialConstants.baseColor[2] = object.material->baseColor[2];

    commandList->SetGraphicsRoot32BitConstants(0, 8, &materialConstants, 0);

    commandList->SetGraphicsRootConstantBufferView(1, cameraConstantsAddress);
    commandList->SetGraphicsRootConstantBufferView(2, objectConstantsAddress);

    object.mesh->Bind(commandList);

    object.material->texture->Transition(
        commandList,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if(object.material->normalTexture)
    {
        object.material->normalTexture->Transition(
            commandList,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    if(object.mesh->IsIndexed()){
        const uint64_t indexCount = static_cast<uint64_t>(object.mesh->GetIndexCount());

        commandList->DrawIndexedInstanced(static_cast<UINT>(object.mesh->GetIndexCount()), 1, 0, 0, 0);

        ++m_currentFrameStatistics.geometryDrawCalls;
        m_currentFrameStatistics.submittedTriangles += indexCount / 3;
    }
    else{
        const uint64_t vertexCount = static_cast<uint64_t>(object.mesh->GetVertexCount());

        commandList->DrawInstanced(static_cast<UINT>(object.mesh->GetVertexCount()), 1, 0, 0);

        ++m_currentFrameStatistics.geometryDrawCalls;
        m_currentFrameStatistics.submittedTriangles += vertexCount / 3;
    }
}

void Renderer::DrawSelectionMask(ID3D12GraphicsCommandList* commandList, const RenderObject& object, D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress)
{
    if(!object.mesh){
        return;
    }

    ObjectConstants objectConstants{};
    objectConstants.model = object.transform.GetModelMatrix();
    objectConstants.previousModel = object.previousTransform.GetModelMatrix();
    objectConstants.normalMatrix = objectConstants.model.Inverse().Transpose();

    const D3D12_GPU_VIRTUAL_ADDRESS objectConstantsAddress = m_uploadBuffer.Allocate(&objectConstants, sizeof(ObjectConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(objectConstantsAddress == 0){
        Log::Error("Failed to allocate selection mask object constants.");
        return;
    }

    m_selectionMaskPass.Bind(commandList);

    commandList->SetGraphicsRootConstantBufferView(0, cameraConstantsAddress);
    commandList->SetGraphicsRootConstantBufferView(1, objectConstantsAddress);

    object.mesh->Bind(commandList);

    if(object.mesh->IsIndexed()){
        commandList->DrawIndexedInstanced(static_cast<UINT>(object.mesh->GetIndexCount()), 1, 0, 0, 0);
    }
    else{
        commandList->DrawInstanced(static_cast<UINT>(object.mesh->GetVertexCount()), 1, 0, 0);
    }
}


// --------------
// Geometry pass
// --------------
void Renderer::BeginGeometryPass(ID3D12GraphicsCommandList* commandList)
{
    RenderTarget& albedo = m_gBuffer.GetMaterial();
    RenderTarget& normal = m_gBuffer.GetSurface();
    RenderTarget& motion = m_gBuffer.GetMotion();
    RenderTarget& geometricNormal = m_gBuffer.GetGeometricNormal();
    DepthBuffer& depth = m_gBuffer.GetDepth();

    albedo.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    normal.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    motion.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    geometricNormal.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    depth.Transition(commandList, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    const float albedoClearColor[] =
    {
        1.0f, 0.0f, 1.0f, 1.0f
    };

    const float normalClearColor[] =
    {
        0.0f, 1.0f, 0.0f, 1.0f
    };

    const float motionClearColor[] =
    {
        0.0f, 0.0f, 0.0f, 0.0f
    };

    const float geometricNormalClearColor[] = {0.0f, 1.0f, 0.0f, 0.0f};

    D3D12_CPU_DESCRIPTOR_HANDLE renderTargets[] = {albedo.GetRTV(), normal.GetRTV(), motion.GetRTV(), geometricNormal.GetRTV()};

    commandList->ClearRenderTargetView(renderTargets[0], albedoClearColor, 0, nullptr);
    commandList->ClearRenderTargetView(renderTargets[1], normalClearColor, 0, nullptr);
    commandList->ClearRenderTargetView(renderTargets[2], motionClearColor, 0, nullptr);
    commandList->ClearRenderTargetView(renderTargets[3], geometricNormalClearColor, 0, nullptr);

    commandList->ClearDepthStencilView(depth.GetDSV(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    D3D12_CPU_DESCRIPTOR_HANDLE dsv = depth.GetDSV();

    commandList->OMSetRenderTargets(4, renderTargets, FALSE, &dsv);
}

void Renderer::EndGeometryPass(ID3D12GraphicsCommandList* commandList)
{
    constexpr D3D12_RESOURCE_STATES shaderReadState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    m_gBuffer.GetMaterial().Transition(commandList, shaderReadState);
    m_gBuffer.GetSurface().Transition(commandList, shaderReadState);
    m_gBuffer.GetMotion().Transition(commandList, shaderReadState);
    m_gBuffer.GetGeometricNormal().Transition(commandList, shaderReadState);
    m_gBuffer.GetDepth().Transition(commandList, shaderReadState);
}

// Scene resources and history

bool Renderer::InitializeSceneResources(const Scene& scene, const std::vector<Material*>& materials)
{
    if(!InitializeMaterials(materials)){
        Log::Error("Failed to init scene materials.");
        return false;
    }

    if(!InitializeRayTracingScene(scene)){
        Log::Error("Failed to init ray tracing scene.");
        // Retain resources until shutdown confirms completion of any submitted build.
        return false;
    }

    m_selectionMaskPass.Shutdown();

    const RenderObject* firstRenderableObject = nullptr;

    for(const RenderObject& object : scene.GetRenderObjects()){
        if(object.mesh){
            firstRenderableObject = &object;
            break;
        }
    }

    if(firstRenderableObject){
        if(!m_selectionMaskPass.Initialize(m_graphicsDevice.GetDevice(), m_shaderLibrary, firstRenderableObject->mesh->GetVertexLayout())){
            Log::Error("Failed to init selection mask pass.");
            return false;
        }
    }

    ResetSceneHistory();

    return true;
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

bool Renderer::InitializeRayTracingScene(const Scene& scene)
{
    if(!m_graphicsDevice.SupportsRayTracing()){
        Log::Error("Cannot initialize ray tracing scene: ray tracing is not supported.");
        return false;
    }

    if(!m_commandContext.Begin(0)){
        Log::Error("Failed to begin acceleration structure construction.");
        return false;
    }

    if(!m_rayTracingScene.Initialize(m_graphicsDevice.GetRayTracingDevice(), m_commandContext.GetRayTracingCommandList(), m_resourceUploader, m_resourceDescriptorHeap, scene)){
        Log::Error("Failed to init ray tracing scene resources.");
        return false;
    }

    if(!m_commandContext.End()){
        return false;
    }

    const bool buildSubmitted = m_commandContext.Execute(0);

    // A failed submission fence signal does not mean the build was discarded.
    if(!m_commandContext.WaitForGPU()){
        Log::Error("Failed to confirm acceleration structure build completion.");
        return false;
    }
    if(!buildSubmitted){
        Log::Error("Failed to execute or signal acceleration structure construction.");
        return false;
    }

    return true;
}

void Renderer::ReleaseSceneResources()
{
    m_rayTracingScene.Shutdown();
    m_materialBuffer.Shutdown();

    m_selectionMaskPass.Shutdown();

    ResetSceneHistory();
}

void Renderer::ResetSceneHistory()
{
    m_rayTracedShadowPass.ResetHistory();
    m_rayTracedReflectionTemporalPass.ResetHistory();
    m_aoTemporalPass.ResetHistory();

    m_hasPreviousViewProjection = false;
    m_hasPreviousShadowLightDirection = false;
    m_hasPreviousNrdCamera = false;

    m_nrdExecutedLastFrame = false;
    m_nativeReflectionExecutedLastFrame = false;
    m_aoExecutedLastFrame = false;

    m_shadowSampleIndex = 0;
    m_reflectionSampleIndex = 0;
    m_nrdFrameIndex = 0;
}

// --------------
// Rendering
// --------------

bool Renderer::Render(const Scene& scene, const RendererOptions& options)
{
    const unsigned int frameIndex = m_swapChain.GetCurrentBackBufferIndex();
    const ToneMapDebugView debugView = m_debugUI.GetToneMapDebugView();
    const bool runAO = !options.enableRTGI ||
        debugView == ToneMapDebugView::AORaw ||
        debugView == ToneMapDebugView::AOTemporal ||
        debugView == ToneMapDebugView::AOFiltered ||
        debugView == ToneMapDebugView::HDRIDiffuseAmbientOnly;

    if(!m_commandContext.Begin(frameIndex)){
        Log::Error("failed to begin command recording.");
        return false;
    }

    if(m_gpuTimestampProfiler.CollectCompleted(m_commandContext.GetCompletedFenceValue())){
        const GpuTimestampProfiler::Results& gpuResults = m_gpuTimestampProfiler.GetResults();

        for(unsigned int i = 0; i < GpuTimestampProfiler::FrameCount; ++i){
            PendingRendererStatistics& pending = m_pendingStatistics[i];

            if(!pending.valid){
                continue;
            }

            if(pending.statistics.frameSerial != gpuResults.frameSerial){
                continue;
            }

            m_statistics = pending.statistics;
            m_statistics.gpuTimings = gpuResults;

            pending.valid = false;
            break;
        }
    }
    
    ID3D12GraphicsCommandList* commandList = m_commandContext.GetCommandList();
    m_gpuTimestampProfiler.BeginFrame(commandList, frameIndex);

    m_currentFrameStatistics = {};
    m_currentFrameStatistics.frameSerial = m_gpuProfilerFrameSerial;
    m_currentFrameStatistics.renderObjectCount = static_cast<uint64_t>(scene.GetRenderObjects().size());
    m_currentFrameStatistics.tlasInstanceCount = static_cast<uint64_t>(m_rayTracingScene.GetInstanceCount());

    m_uploadBuffer.Reset(frameIndex);

    const bool tlasNeedsUpdate = !scene.GetChangedTransforms().empty();

    m_debugUI.SetTLASUpdated(tlasNeedsUpdate);

    if(tlasNeedsUpdate){
        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::TlasUpdate);
        const bool updated = m_rayTracingScene.Update(m_commandContext.GetRayTracingCommandList(), frameIndex, scene);
        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::TlasUpdate);
        if(!updated)
        {
            Log::Error("Failed to update ray tracing scene.");
            return false;
        }
    }

    const Camera& camera = scene.GetCamera();
    const DirectionalLight& light = scene.GetDirectionalLight();

    const Matrix4 currentView = camera.GetViewMatrix();
    const Matrix4 currentProjection = camera.GetProjectionMatrix();
    const Matrix4 currentViewProjection = currentProjection * currentView;

    CameraConstants cameraConstants{};
    cameraConstants.viewProjection = currentViewProjection;
    cameraConstants.previousViewProjection = m_hasPreviousViewProjection ? m_previousViewProjection : currentViewProjection;
    cameraConstants.view = currentView;
    cameraConstants.previousView = m_hasPreviousViewProjection ? m_previousView : currentView;

    const Matrix4 previousNrdView = m_hasPreviousNrdCamera ? m_previousNrdView : currentView;

    const Matrix4 previousNrdProjection = m_hasPreviousNrdCamera ? m_previousNrdProjection : currentProjection;

    D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress = m_uploadBuffer.Allocate(&cameraConstants, sizeof(CameraConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(cameraConstantsAddress == 0){
        Log::Error("Failed to allocate camera constants.");
        return false;
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
        return false;
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
        return false;
    }

    const Vector3 currentLightDirection = light.direction.Normalized();

    if(m_hasPreviousShadowLightDirection){
        const float directionDifference = (currentLightDirection - m_previousShadowLightDirection).Length();

        if(directionDifference > 0.0001f){
            m_rayTracedShadowPass.ResetHistory();
        }
    }

    m_previousShadowLightDirection = currentLightDirection;
    m_hasPreviousShadowLightDirection = true;

    ShadowConstants shadowConstants{};
    shadowConstants.inverseViewProjection = reconstructionConstants.inverseViewProjection;

    if(m_hasPreviousViewProjection){
        shadowConstants.previousViewProjection = m_previousViewProjection;
        shadowConstants.previousInverseViewProjection = m_previousViewProjection.Inverse();
    }
    else{
        shadowConstants.previousViewProjection = cameraConstants.viewProjection;
        shadowConstants.previousInverseViewProjection = reconstructionConstants.inverseViewProjection;
    }

    shadowConstants.lightDirection[0] = lightDirection.x;
    shadowConstants.lightDirection[1] = lightDirection.y;
    shadowConstants.lightDirection[2] = lightDirection.z;
    shadowConstants.depthIndex = m_gBuffer.GetDepth().GetSRVIndex();
    shadowConstants.surfaceIndex = m_gBuffer.GetSurface().GetSRVIndex();
    shadowConstants.geometricNormalIndex = m_gBuffer.GetGeometricNormal().GetSRVIndex();
    shadowConstants.lightAngularRadius = light.angularRadius;
    shadowConstants.frameIndex = m_shadowSampleIndex++;
    shadowConstants.historyIndex = m_rayTracedShadowPass.GetVisibilityTexture().GetDescriptorIndex();
    shadowConstants.previousDepthIndex = m_rayTracedShadowPass.GetPreviousDepthIndex();
    shadowConstants.outputDepthIndex = m_rayTracedShadowPass.GetOutputDepthIndex();
    shadowConstants.previousHistoryLengthIndex =m_rayTracedShadowPass.GetPreviousHistoryLengthIndex();
    shadowConstants.outputHistoryLengthIndex = m_rayTracedShadowPass.GetOutputHistoryLengthIndex();
    shadowConstants.accumulatedFrameCount = m_rayTracedShadowPass.GetAccumulatedFrameCount();

    ReflectionConstants reflectionConstants{};

    reflectionConstants.inverseViewProjection = reconstructionConstants.inverseViewProjection;
    reflectionConstants.cameraPosition[0] = cameraPosition.x;
    reflectionConstants.cameraPosition[1] = cameraPosition.y;
    reflectionConstants.cameraPosition[2] = cameraPosition.z;
    reflectionConstants.depthIndex = m_gBuffer.GetDepth().GetSRVIndex();
    reflectionConstants.surfaceIndex = m_gBuffer.GetSurface().GetSRVIndex();
    reflectionConstants.geometricNormalIndex = m_gBuffer.GetGeometricNormal().GetSRVIndex();
    reflectionConstants.instanceDataIndex = m_rayTracingScene.GetInstanceDataDescriptorIndex();
    reflectionConstants.materialBufferIndex = m_materialBuffer.GetDescriptorIndex();
    reflectionConstants.environmentIndex = m_environment.GetSourceTexture().GetDescriptorIndex();
    reflectionConstants.brdfIntegrationLUTIndex = m_brdfIntegrationLUT.GetDescriptorIndex();
    reflectionConstants.ggxAverageAlbedoLUTIndex = m_ggxAverageAlbedoLUT.GetDescriptorIndex();
    reflectionConstants.frameIndex = m_reflectionSampleIndex++;
    reflectionConstants.environmentCubemapIndex = m_environment.GetCubemap().GetDescriptorIndex();
    reflectionConstants.diffuseIrradianceIndex = m_environment.GetDiffuseIrradiance().GetDescriptorIndex();
    reflectionConstants.ggxMultiscatterIrradianceIndex = m_environment.GetGGXMultiscatterIrradiance().GetDescriptorIndex();
    reflectionConstants.materialIndex = m_gBuffer.GetMaterial().GetSRVIndex();
    reflectionConstants.importanceConditionalCDFIndex = m_environment.GetImportanceConditionalCDF().GetDescriptorIndex();
    reflectionConstants.importanceMarginalCDFIndex = m_environment.GetImportanceMarginalCDF().GetDescriptorIndex();
    reflectionConstants.importanceWidth = m_environment.GetImportanceWidth();
    reflectionConstants.importanceHeight = m_environment.GetImportanceHeight();
    reflectionConstants.importanceTotalWeight = m_environment.GetImportanceTotalWeight();

    m_previousViewProjection = cameraConstants.viewProjection;
    m_previousView = cameraConstants.view;
    m_hasPreviousViewProjection = true;

    D3D12_GPU_VIRTUAL_ADDRESS shadowConstantsAddress = m_uploadBuffer.Allocate(&shadowConstants, sizeof(ShadowConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(shadowConstantsAddress == 0){
        Log::Error("Failed to allocate ray traced shadow constants.");
        return false;
    }

    D3D12_GPU_VIRTUAL_ADDRESS reflectionConstantsAddress = m_uploadBuffer.Allocate(&reflectionConstants, sizeof(ReflectionConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(reflectionConstantsAddress == 0){
        Log::Error("Failed to allocate ray traced reflection constants.");
        return false;
    }

    GIConstants giConstants{};
    giConstants.inverseViewProjection = reconstructionConstants.inverseViewProjection;
    giConstants.depthIndex = m_gBuffer.GetDepth().GetSRVIndex();
    giConstants.surfaceIndex = m_gBuffer.GetSurface().GetSRVIndex();
    giConstants.geometricNormalIndex = m_gBuffer.GetGeometricNormal().GetSRVIndex();
    giConstants.frameIndex = reflectionConstants.frameIndex;
    giConstants.instanceDataIndex = reflectionConstants.instanceDataIndex;
    giConstants.materialBufferIndex = reflectionConstants.materialBufferIndex;
    giConstants.environmentIndex = reflectionConstants.environmentIndex;
    giConstants.diffuseIrradianceIndex = m_environment.GetDiffuseIrradiance().GetDescriptorIndex();
    giConstants.diffuseIrradianceArrayIndex = m_environment.GetDiffuseIrradianceArrayIndex();

    const D3D12_GPU_VIRTUAL_ADDRESS giConstantsAddress = m_uploadBuffer.Allocate( &giConstants, sizeof(GIConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(giConstantsAddress == 0){
        Log::Error("Failed to allocate ray traced GI constants.");
        return false;
    }

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

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::Geometry);

    BeginGeometryPass(commandList);

    for(const RenderObject& object : scene.GetRenderObjects()){
        Draw(commandList, object, cameraConstantsAddress);
    }

    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::Geometry);
    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::SelectionMask);

    m_selectionMask.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);

    const float maskClearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
    commandList->ClearRenderTargetView(m_selectionMask.GetRTV(), maskClearColor, 0, nullptr);

    const auto& renderObjects = scene.GetRenderObjects();

    if(options.selectedObject < renderObjects.size())
    {
        const RenderObject& selectedObject = renderObjects[options.selectedObject];

        if(selectedObject.mesh)
        {
            DepthBuffer& depth = m_gBuffer.GetDepth();

            depth.Transition(commandList, D3D12_RESOURCE_STATE_DEPTH_READ);

            const D3D12_CPU_DESCRIPTOR_HANDLE maskRTV = m_selectionMask.GetRTV();
            const D3D12_CPU_DESCRIPTOR_HANDLE depthDSV = depth.GetReadOnlyDSV();

            commandList->OMSetRenderTargets(1, &maskRTV, FALSE, &depthDSV);

            DrawSelectionMask(commandList, selectedObject, cameraConstantsAddress);
        }
    }

    m_selectionMask.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::SelectionMask);

    EndGeometryPass(commandList);

    if(runAO)
    {
        AOConstants aoConstants{};

        aoConstants.inverseViewProjection = reconstructionConstants.inverseViewProjection;
        aoConstants.depthIndex = m_gBuffer.GetDepth().GetSRVIndex();
        aoConstants.surfaceIndex = m_gBuffer.GetSurface().GetSRVIndex();
        aoConstants.frameIndex = reflectionConstants.frameIndex;

        aoConstants.maxDistance = 1.5f;
        aoConstants.originBias = 0.01f;
        aoConstants.rayTMin = 0.001f;

        const D3D12_GPU_VIRTUAL_ADDRESS aoConstantsAddress = m_uploadBuffer.Allocate(&aoConstants, sizeof(AOConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(aoConstantsAddress == 0){
            Log::Error("Failed to allocate ray traced AO constants.");
            return false;
        }

        if(!m_aoExecutedLastFrame)
            m_aoTemporalPass.ResetHistory();

        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::RayTracedAO);
        m_rayTracedAOPass.Execute(
            m_commandContext.GetRayTracingCommandList(),
            m_resourceDescriptorHeap,
            m_rayTracingScene,
            aoConstantsAddress,
            m_width,
            m_height);
        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::RayTracedAO);

        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::AOTemporal);
        m_aoTemporalPass.Execute(
            commandList,
            m_rayTracedAOPass.GetRawAOTexture(),
            m_gBuffer,
            m_uploadBuffer,
            reconstructionConstants.inverseViewProjection,
            shadowConstants.previousViewProjection,
            shadowConstants.previousInverseViewProjection,
            m_width,
            m_height);
        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::AOTemporal);

        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::AODenoise);
        m_aoDenoisePass.Execute(
            commandList,
            m_aoTemporalPass.GetAccumulatedAOTexture(),
            m_gBuffer,
            m_uploadBuffer,
            reconstructionConstants.inverseViewProjection,
            m_width,
            m_height);
        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::AODenoise);
    }

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::RayTracedShadows);

    m_rayTracedShadowPass.Execute(
        m_commandContext.GetRayTracingCommandList(),
        m_resourceDescriptorHeap,
        m_rayTracingScene,
        shadowConstantsAddress,
        m_width,
        m_height
    );

    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::RayTracedShadows);

    constexpr D3D12_RESOURCE_STATES shaderReadState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    m_environment.GetSourceTexture().Transition(commandList, shaderReadState);
    m_environment.GetCubemap().Transition(commandList, shaderReadState);
    m_environment.GetDiffuseIrradiance().Transition(commandList, shaderReadState);
    m_environment.GetGGXMultiscatterIrradiance().Transition(commandList, shaderReadState);
    m_brdfIntegrationLUT.Transition(commandList, shaderReadState);
    m_ggxAverageAlbedoLUT.Transition(commandList, shaderReadState);

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::RayTracedReflections);

    m_rayTracedReflectionPass.Execute(
        m_commandContext.GetRayTracingCommandList(),
        m_resourceDescriptorHeap,
        m_rayTracingScene,
        reflectionConstantsAddress,
        lightConstantsAddress,
        m_width,
        m_height
    );

    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::RayTracedReflections);

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::RayTracedGI);
    m_rayTracedGIPass.Execute(
        m_commandContext.GetRayTracingCommandList(),
        m_resourceDescriptorHeap,
        m_rayTracingScene,
        giConstantsAddress,
        lightConstantsAddress,
        m_width,
        m_height);
    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::RayTracedGI);

    constexpr float nrdDenoisingRange = 1000.0f;
    const bool limiterEnabled = m_debugUI.IsReflectionContributionLimiterEnabled();
    const ReflectionDenoiserMode reflectionMode = m_debugUI.GetReflectionDenoiserMode();
    bool runNrd = reflectionMode == ReflectionDenoiserMode::Nrd;
    // Native production can still inspect fresh NRD diagnostics without changing its lighting input.
    switch(debugView)
    {
        case ToneMapDebugView::NrdViewZ:
        case ToneMapDebugView::NrdNormal:
        case ToneMapDebugView::NrdRoughness:
        case ToneMapDebugView::NrdMotion:
        case ToneMapDebugView::NrdSpecularRadiance:
        case ToneMapDebugView::NrdHitDistance:
        case ToneMapDebugView::NrdResolvedReflection:
        case ToneMapDebugView::GIDenoised:
            runNrd = true; break;
        default: break;
    }
    runNrd = runNrd || options.enableRTGI;
    const bool runNative = reflectionMode == ReflectionDenoiserMode::Native;
    const bool resetNrdHistory = !m_hasPreviousNrdCamera || !m_nrdExecutedLastFrame ||
        limiterEnabled != m_previousContributionLimiterEnabled;

    if(runNrd)
    {
        m_nrdReflectionDenoiser.NewFrame();

        if(!m_nrdReflectionDenoiser.SetFrameSettings(
            currentView,
            currentProjection,
            previousNrdView,
            previousNrdProjection,
            m_nrdFrameIndex,
            nrdDenoisingRange,
            resetNrdHistory)){
            Log::Error("Failed to set NRD reflection frame settings.");
            return false;
        }

        Texture* nrdInput = &m_rayTracedReflectionPass.GetRawReflectionTexture();
        if(limiterEnabled)
        {
            m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::ReflectionContributionLimiter);

            const bool limited = m_reflectionContributionLimitPass.Execute(commandList, *nrdInput, m_uploadBuffer, m_width, m_height);
            m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::ReflectionContributionLimiter);
            if(!limited)
            {
                Log::Error("Failed to execute reflection contribution limiter.");
                return false;
            }

            nrdInput = &m_reflectionContributionLimitPass.GetOutputTexture();
        }

        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdPrepare);

        m_nrdReflectionPreparePass.Execute(
            commandList,
            m_gBuffer,
            *nrdInput,
            m_rayTracedReflectionPass.GetSpecularTrackingDistanceTexture(),
            m_rayTracedGIPass.GetRawGITexture(),
            m_rayTracedGIPass.GetHitDistanceTexture(),
            m_uploadBuffer,
            reconstructionConstants.inverseViewProjection,
            currentView,
            m_materialBuffer.GetDescriptorIndex(),
            m_width,
            m_height,
            nrdDenoisingRange);

        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdPrepare);

        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdReblur);

        m_nrdReflectionDenoiser.Denoise(
            commandList,
            m_nrdReflectionPreparePass.GetMotionTexture(),
            m_nrdReflectionPreparePass.GetNormalRoughnessTexture(),
            m_nrdReflectionPreparePass.GetViewZTexture(),
            m_nrdReflectionPreparePass.GetSpecRadianceHitDistTexture(),
            m_nrdReflectionPreparePass.GetDiffRadianceHitDistTexture());

        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdReblur);

        commandList->SetDescriptorHeaps(
            1,
            descriptorHeaps);

        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdResolve);

        m_nrdReflectionResolvePass.Execute(
            commandList,
            m_gBuffer,
            m_nrdReflectionDenoiser.GetOutputTexture(),
            m_uploadBuffer,
            reconstructionConstants.inverseViewProjection,
            currentView,
            m_materialBuffer.GetDescriptorIndex(),
            m_width,
            m_height);

        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdResolve);

        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdDiffuseResolve);
        m_nrdDiffuseResolvePass.Execute(
            commandList,
            m_nrdReflectionDenoiser.GetDiffuseOutputTexture(),
            m_nrdReflectionPreparePass.GetViewZTexture(),
            m_uploadBuffer,
            m_width,
            m_height,
            nrdDenoisingRange);
        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdDiffuseResolve);
    }

    if(runNative)
    {
        if(!m_nativeReflectionExecutedLastFrame)
            m_rayTracedReflectionTemporalPass.ResetHistory();
        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::ReflectionTemporal);
        m_rayTracedReflectionTemporalPass.Execute(
            commandList,
            m_rayTracedReflectionPass.GetRawReflectionTexture(),
            m_gBuffer,
            m_uploadBuffer,
            reflectionConstants.inverseViewProjection,
            shadowConstants.previousViewProjection,
            shadowConstants.previousInverseViewProjection,
            m_width,
            m_height);
        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::ReflectionTemporal);

        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::ReflectionDenoise);
        m_rayTracedReflectionDenoisePass.Execute(
            commandList,
            m_rayTracedReflectionTemporalPass.GetReflectionTexture(),
            m_rayTracedReflectionPass.GetHitDistanceTexture(),
            m_gBuffer,
            m_uploadBuffer,
            reflectionConstants.inverseViewProjection,
            m_width,
            m_height);
        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::ReflectionDenoise);
    }

    Texture& finalReflection = reflectionMode == ReflectionDenoiserMode::Nrd
        ? m_nrdReflectionResolvePass.GetOutputTexture()
        : m_rayTracedReflectionDenoisePass.GetFilteredReflectionTexture();

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::ShadowDenoise);

    m_shadowDenoisePass.Execute(commandList, m_rayTracedShadowPass.GetVisibilityTexture(), m_gBuffer, m_uploadBuffer, reconstructionConstants.inverseViewProjection, m_width, m_height);

    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::ShadowDenoise);

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::DeferredLighting);

    m_deferredLightingPass.Execute(
        commandList,
        m_gBuffer,
        m_materialBuffer,
        m_environment.GetSourceTexture(),
        m_environment.GetCubemap(),
        m_environment.GetDiffuseIrradiance(),
        m_brdfIntegrationLUT,
        m_ggxAverageAlbedoLUT,
        m_environment.GetGGXMultiscatterIrradiance(),
        m_shadowDenoisePass.GetFilteredVisibilityTexture(),
        finalReflection,
        m_nrdDiffuseResolvePass.GetOutputTexture(),
        m_aoDenoisePass.GetFilteredAOTexture(),
        options.enableRTGI,
        m_sceneColor,
        reconstructionConstantsAddress,
        lightConstantsAddress,
        static_cast<unsigned int>(debugView)
    );

    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::DeferredLighting);

    Texture* debugTexture = nullptr;
    RenderTarget* debugRenderTarget = nullptr;
    switch(debugView)
    {
        case ToneMapDebugView::GBufferWorldNormal:
            debugRenderTarget = &m_gBuffer.GetSurface(); break;
        case ToneMapDebugView::GBufferAlbedo:
            debugRenderTarget = &m_gBuffer.GetMaterial(); break;
        case ToneMapDebugView::GBufferMotion:
            debugRenderTarget = &m_gBuffer.GetMotion(); break;
        case ToneMapDebugView::SpecularTrackingDistance:
            debugTexture = &m_rayTracedReflectionPass.GetSpecularTrackingDistanceTexture(); break;
        case ToneMapDebugView::NrdViewZ:
            debugTexture = &m_nrdReflectionPreparePass.GetViewZTexture(); break;
        case ToneMapDebugView::NrdNormal:
        case ToneMapDebugView::NrdRoughness:
            debugTexture = &m_nrdReflectionPreparePass.GetNormalRoughnessTexture(); break;
        case ToneMapDebugView::NrdMotion:
            debugTexture = &m_nrdReflectionPreparePass.GetMotionTexture(); break;
        case ToneMapDebugView::NrdSpecularRadiance:
        case ToneMapDebugView::NrdHitDistance:
            debugTexture = &m_nrdReflectionPreparePass.GetSpecRadianceHitDistTexture(); break;
        case ToneMapDebugView::NrdResolvedReflection:
            debugTexture = &m_nrdReflectionResolvePass.GetOutputTexture(); break;
        case ToneMapDebugView::SelectionMask:
            debugRenderTarget = &m_selectionMask; break;
        case ToneMapDebugView::GIRaw:
            debugTexture = &m_rayTracedGIPass.GetRawGITexture(); break;
        case ToneMapDebugView::GIHitDistance:
            debugTexture = &m_rayTracedGIPass.GetHitDistanceTexture(); break;
        case ToneMapDebugView::GIDenoised:
            debugTexture = &m_nrdDiffuseResolvePass.GetOutputTexture(); break;
        case ToneMapDebugView::AORaw:
            debugTexture = &m_rayTracedAOPass.GetRawAOTexture(); break;
        case ToneMapDebugView::AOTemporal:
            debugTexture = &m_aoTemporalPass.GetAccumulatedAOTexture(); break;
        case ToneMapDebugView::AOFiltered:
            debugTexture = &m_aoDenoisePass.GetFilteredAOTexture(); break;
        default: break;
    }

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::ToneMap);

    m_toneMapPass.Execute(
        commandList,
        m_sceneColor,
        rtvHandle,
        debugTexture,
        debugView,
        nrdDenoisingRange,
        debugRenderTarget,
        &m_selectionMask,
        options.selectionOutlineColor,
        debugView == ToneMapDebugView::None ? options.selectionOutlineThickness : 0,
        m_gBuffer.GetDepth().GetSRVIndex()
    );

    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::ToneMap);

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::DebugUI);
    m_debugUI.EndFrame(commandList);
    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::DebugUI);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

    commandList->ResourceBarrier(1, &barrier);

    m_gpuTimestampProfiler.EndFrameAndResolve(commandList, frameIndex);

    if(!m_commandContext.End()){
        Log::Error("Failed to end command recording.");
        return false;
    }

    if(!m_commandContext.Execute(frameIndex)){
        Log::Error("Failed to submit command list or signal its fence; GPU work may already be in flight.");
        return false;
    }

    const uint64_t submissionFence = m_commandContext.GetFrameFenceValue(frameIndex);

    m_gpuTimestampProfiler.MarkSubmitted(frameIndex, submissionFence, m_gpuProfilerFrameSerial);
    m_aoExecutedLastFrame = runAO;

    m_pendingStatistics[frameIndex].statistics = m_currentFrameStatistics;
    m_pendingStatistics[frameIndex].valid = true;

    ++m_gpuProfilerFrameSerial;

    if(!m_swapChain.Present()){
        Log::Error("Failed to present swap chain.");
        return false;
    }

    m_previousNrdView = currentView;
    m_previousNrdProjection = currentProjection;
    m_hasPreviousNrdCamera = true;
    m_nrdExecutedLastFrame = runNrd;
    m_nativeReflectionExecutedLastFrame = runNative;
    if(runNrd)
    {
        m_previousContributionLimiterEnabled = limiterEnabled;
        ++m_nrdFrameIndex;
    }

    return true;
}


// --------------
// Shutdown
// --------------

void Renderer::Shutdown()
{
    Log::Info("Renderer shutdown: waiting for GPU...");
    if(!m_commandContext.WaitForGPU()){
        Log::Error("Renderer shutdown: GPU completion is unconfirmed; aborting without resource teardown.");
        std::abort();
    }
    Log::Info("Renderer shutdown: GPU finished.");

    Log::Info("Renderer shutdown: shutting down DebugUI...");
    m_debugUI.Shutdown();
    Log::Info("Renderer shutdown: DebugUI finished.");

    Log::Info("Renderer shutdown: releasing renderer resources...");
    m_swapChain.Shutdown();
    m_gpuTimestampProfiler.Shutdown();
    m_commandContext.Shutdown();
    m_resourceUploader.Shutdown();

    m_gBuffer.Shutdown();
    m_deferredLightingPass.Shutdown();
    m_toneMapPass.Shutdown();
    m_brdfIntegrationPass.Shutdown();
    m_ggxAverageAlbedoPass.Shutdown();
    m_rayTracedShadowPass.Shutdown();
    m_rayTracedReflectionPass.Shutdown();
    m_rayTracedGIPass.Shutdown();
    m_rayTracedAOPass.Shutdown();
    m_aoDenoisePass.Shutdown();
    m_aoTemporalPass.Shutdown();
    m_rayTracedReflectionTemporalPass.Shutdown();
    m_rayTracedReflectionDenoisePass.Shutdown();
    m_reflectionContributionLimitPass.Shutdown();
    m_nrdReflectionPreparePass.Shutdown();
    m_nrdReflectionDenoiser.Shutdown();
    m_nrdReflectionResolvePass.Shutdown();
    m_nrdDiffuseResolvePass.Shutdown();
    m_shadowDenoisePass.Shutdown();
    m_selectionMaskPass.Shutdown();

    m_uploadBuffer.Shutdown();
    m_environment.Shutdown();
    m_sceneColor.Shutdown();
    m_selectionMask.Shutdown();

    m_brdfIntegrationLUT.Shutdown();
    m_ggxAverageAlbedoLUT.Shutdown();

    m_resourceDescriptorHeap.Shutdown();
    m_rtvDescriptorHeap.Shutdown();
    m_dsvDescriptorHeap.Shutdown();

    m_shaderLibrary.Shutdown();
    Log::Info("Renderer shutdown: resources released.");

    m_graphicsDevice.Shutdown();
}

bool Renderer::WaitForGPU()
{
    return m_commandContext.WaitForGPU();
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

DebugUI& Renderer::GetDebugUI()
{
    return m_debugUI;
}

// Runtime environment discovery and preprocessing

bool Renderer::DiscoverEnvironments()
{
    m_availableEnvironments.clear();

    const std::filesystem::path environmentDirectory = m_assetRoot / "Textures" / "Environment";

    std::error_code error;

    if(!std::filesystem::exists(environmentDirectory, error) || error){
        Log::Error("Failed to discover environment directory.");
        return false;
    }

    std::filesystem::directory_iterator iterator(environmentDirectory, error);
    const std::filesystem::directory_iterator end;

    if(error){
        Log::Error("Failed iterating through environment directory.");
        return false;
    }

    for(; iterator != end; iterator.increment(error)){
        if(error){
            Log::Error("Error iterating through directory.");
            m_availableEnvironments.clear();
            return false;
        }

        const auto& entry = *iterator;

        if(!entry.is_regular_file(error)){
            if(error){
                m_availableEnvironments.clear();
                return false;
            }

            continue;
        }

        std::string extension = entry.path().extension().string();

        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c){return static_cast<char>(std::tolower(c));});

        if(extension != ".hdr"){
            continue;
        }

        EnvironmentAsset asset;
        asset.displayName = entry.path().stem().string();
        asset.relativePath = std::filesystem::relative(entry.path(), m_assetRoot, error);

        if(error){
            m_availableEnvironments.clear();
            return false;
        }

        m_availableEnvironments.push_back(std::move(asset));
    }

    std::sort(m_availableEnvironments.begin(), m_availableEnvironments.end(), [](const EnvironmentAsset& a, const EnvironmentAsset& b){return a.displayName < b.displayName;});

    return true;
}

bool Renderer::SetEnvironment(size_t index)
{
    if(index >= m_availableEnvironments.size())
    {
        Log::Error("Invalid environment index.");
        return false;
    }

    if(index == m_currentEnvironmentIndex)
    {
        return true;
    }

    if(!m_commandContext.WaitForGPU()){
        Log::Error("Failed to confirm GPU completion before environment switch.");
        return false;
    }

    m_environment.Shutdown();

    const EnvironmentAsset& asset = m_availableEnvironments[index];

    const std::filesystem::path environmentPath = m_assetRoot / asset.relativePath;

    if(!m_environment.Initialize(
        m_graphicsDevice.GetDevice(),
        environmentPath,
        m_resourceUploader,
        m_resourceDescriptorHeap))
    {
        Log::Error("Failed to initialize environment: " + asset.displayName);
        m_environment.Shutdown();
        return false;
    }

    if(!PreprocessEnvironment(m_environment))
    {
        Log::Error("Failed to preprocess environment: " + asset.displayName);

        // Defer resource release to shutdown: preprocessing may have submitted work.
        return false;
    }

    m_currentEnvironmentIndex = index;

    ResetSceneHistory();

    Log::Info("Environment changed to: " + asset.displayName);

    return true;
}

bool Renderer::PreprocessEnvironment(EnvironmentMap& environment)
{
    EnvironmentMipPass environmentMipPass;
    EquirectangularToCubemapPass equirectangularToCubemapPass;
    DiffuseIrradiancePass diffuseIrradiancePass;
    GGXMultiscatterIrradiancePass ggxMultiscatterIrradiancePass;

    auto cleanup = [&]()
    {
        ggxMultiscatterIrradiancePass.Shutdown();
        diffuseIrradiancePass.Shutdown();
        equirectangularToCubemapPass.Shutdown();
        environmentMipPass.Shutdown();
    };

    ID3D12Device* device = m_graphicsDevice.GetDevice();

    if(!environmentMipPass.Initialize(
        device,
        m_shaderLibrary,
        m_rtvDescriptorHeap,
        environment.GetSourceTexture()))
    {
        Log::Error("Failed to initialize environment mip pass.");
        cleanup();
        return false;
    }

    if(!equirectangularToCubemapPass.Initialize(
        device,
        m_shaderLibrary,
        m_rtvDescriptorHeap,
        environment.GetCubemap()))
    {
        Log::Error("Failed to initialize equirectangular-to-cubemap pass.");
        cleanup();
        return false;
    }

    if(!diffuseIrradiancePass.Initialize(
        device,
        m_shaderLibrary,
        m_rtvDescriptorHeap,
        environment.GetDiffuseIrradiance()))
    {
        Log::Error("Failed to initialize diffuse irradiance pass.");
        cleanup();
        return false;
    }

    if(!ggxMultiscatterIrradiancePass.Initialize(
        device,
        m_shaderLibrary,
        m_rtvDescriptorHeap,
        environment.GetGGXMultiscatterIrradiance()))
    {
        Log::Error("Failed to initialize GGX multiscatter irradiance pass.");
        cleanup();
        return false;
    }

    if(!m_commandContext.Begin(0))
    {
        Log::Error("Failed to begin environment preprocessing.");
        cleanup();
        return false;
    }

    ID3D12GraphicsCommandList* commandList =
        m_commandContext.GetCommandList();

    ID3D12DescriptorHeap* descriptorHeaps[] =
    {
        m_resourceDescriptorHeap.GetHeap()
    };

    commandList->SetDescriptorHeaps(1, descriptorHeaps);

    environmentMipPass.Execute(
        commandList,
        environment.GetSourceTexture());

    diffuseIrradiancePass.Execute(
        commandList,
        environment.GetSourceTexture(),
        environment.GetDiffuseIrradiance());

    equirectangularToCubemapPass.Execute(
        commandList,
        environment.GetSourceTexture(),
        environment.GetCubemap());

    ggxMultiscatterIrradiancePass.Execute(
        commandList,
        environment.GetSourceTexture(),
        m_brdfIntegrationLUT,
        environment.GetGGXMultiscatterIrradiance());

    if(!m_commandContext.End())
    {
        Log::Error("Failed to end environment preprocessing.");
        cleanup();
        return false;
    }

    const bool submitted = m_commandContext.Execute(0);
    
    if(!m_commandContext.WaitForGPU()){
        Log::Error("Failed to confirm environment preprocessing completion; retaining temporary GPU resources until process exit.");
        // These passes own raw D3D12 pointers. Do not release them or recycle their
        // descriptors while submitted work may still reference them.
        return false;
    }

    cleanup();

    if(!submitted)
    {
        Log::Error("Failed to execute environment preprocessing.");
        return false;
    }

    return true;
}

const RendererStatistics& Renderer::GetStatistics() const
{
    return m_statistics;
}
