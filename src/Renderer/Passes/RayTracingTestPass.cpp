#include "RayTracingTestPass.h"

#include "Core/Log.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/Mesh.h"
#include "Renderer/RayTracing/RayTracingAccelerationStructure.h"
#include "Scene/Scene.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/RenderTarget.h"

#include <vector>

bool RayTracingTestPass::Initialize(
    GraphicsDevice& graphicsDevice,
    ID3D12GraphicsCommandList4* commandList,
    ShaderLibrary& shaderLibrary,
    DescriptorHeap& descriptorHeap,
    const Mesh& mesh,
    const Scene& scene,
    unsigned int width,
    unsigned int height)
{
    if(!graphicsDevice.SupportsRayTracing() || !commandList){
        return false;
    }

    RayTracingBottomLevelDescription bottomLevelDescription{};

    RayTracingGeometryDescription geometryDescription{};
    geometryDescription.mesh = &mesh;
    geometryDescription.opaque = true;

    bottomLevelDescription.geometries.push_back(geometryDescription);
    bottomLevelDescription.buildPreference = RayTracingAccelerationStructureBuildPreference::FastTrace;

    if(!m_blas.Initialize(graphicsDevice.GetRayTracingDevice(), commandList, bottomLevelDescription))
    {
        return false;
    }

    RayTracingTopLevelDescription topLevelDescription{};

    const std::vector<RenderObject>& renderObjects = scene.GetRenderObjects();

    topLevelDescription.instances.reserve(renderObjects.size());

    for(unsigned int i = 0; i < renderObjects.size(); ++i)
    {
        RayTracingInstanceDescription instanceDescription{};

        instanceDescription.transform = renderObjects[i].transform.GetModelMatrix();
        instanceDescription.instanceID = i;
        instanceDescription.mask = 0xFF;
        instanceDescription.bottomLevelIndex = 0;

        topLevelDescription.instances.push_back(instanceDescription);
    }

    topLevelDescription.buildPreference = RayTracingAccelerationStructureBuildPreference::FastTrace;

    const std::vector<const BottomLevelAccelerationStructure*> bottomLevelStructures = {&m_blas};

    if(!m_tlas.Initialize(graphicsDevice.GetRayTracingDevice(), commandList, topLevelDescription, bottomLevelStructures))
    {
        return false;
    }

    RayTracingPipelineDescription pipelineDescription{};
    pipelineDescription.shaderPath = "RayTracingTest.hlsl";
    pipelineDescription.rayGenerationShader = L"RayGeneration";
    pipelineDescription.missShaders = {L"Miss"};
    pipelineDescription.hitGroups ={{L"HitGroup", L"ClosestHit", L""}};
    pipelineDescription.maxPayloadSize = sizeof(float) * 3;
    pipelineDescription.maxAttributeSize = sizeof(float) * 2;
    pipelineDescription.maxRecursionDepth = 1;

    if(!m_pipeline.Initialize(graphicsDevice.GetRayTracingDevice(), shaderLibrary, pipelineDescription))
    {
        return false;
    }

    if(!m_output.Initialize(graphicsDevice.GetDevice(), width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        Log::Error("Failed to initialize ray tracing output texture.");
        return false;
    }

    if(!m_output.CreateUAV(graphicsDevice.GetDevice(), descriptorHeap))
    {
        Log::Error("Failed to create ray tracing output UAV.");
        return false;
    }

    return true;
}

void RayTracingTestPass::Execute(
    ID3D12GraphicsCommandList4* commandList,
    DescriptorHeap& descriptorHeap,
    D3D12_GPU_VIRTUAL_ADDRESS reconstructionConstantsAddress,
    RenderTarget& sceneColor,
    unsigned int width,
    unsigned int height)
{
    if(!commandList){
        return;
    }

    m_output.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    commandList->SetPipelineState1(m_pipeline.GetStateObject());
    commandList->SetComputeRootSignature(m_pipeline.GetGlobalRootSignature());
    commandList->SetComputeRootShaderResourceView(0, m_tlas.GetGPUAddress());

    const D3D12_GPU_DESCRIPTOR_HANDLE outputUAV = descriptorHeap.GetGPUHandle(m_output.GetUAVIndex());

    commandList->SetComputeRootDescriptorTable(1, outputUAV);
    commandList->SetComputeRootConstantBufferView(2, reconstructionConstantsAddress);

    D3D12_DISPATCH_RAYS_DESC dispatchDescription = m_pipeline.CreateDispatchDescription(width, height);

    commandList->DispatchRays(&dispatchDescription);

    m_output.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);

    sceneColor.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST);

    commandList->CopyResource(sceneColor.GetResource(), m_output.GetResource());
}

void RayTracingTestPass::Shutdown()
{
    m_pipeline.Shutdown();
    m_tlas.Shutdown();
    m_blas.Shutdown();
    m_output.Shutdown();
}