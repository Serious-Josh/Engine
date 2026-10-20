#include "PathTraceReferencePass.h"

#include "Core/Log.h"

#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/RenderTarget.h"

#include "Renderer/Shader/ShaderLibrary.h"

#include "Renderer/RayTracing/RayTracingPipelineDescription.h"
#include "Renderer/RayTracing/RayTracingSceneResources.h"

#include <algorithm>
#include <string>
#include <vector>

namespace
{
    // True when the export name occurs in the DXIL container (its function table stores every entry point's
    // name, mangled around it). Absence is conclusive; presence is confirmed by state-object creation.
    bool LibraryNamesExport(const std::vector<char>& bytecode, const std::wstring& exportName)
    {
        // Export names are ASCII identifiers.
        std::string name;

        for(wchar_t character : exportName)
        {
            name.push_back(static_cast<char>(character));
        }

        return !name.empty() &&
            std::search(bytecode.begin(), bytecode.end(), name.begin(), name.end()) != bytecode.end();
    }

    // One shared library; each primary constructor is its own ray-generation export. The RTXPT.3 ReSTIR DI
    // primary uses the RTXDI-specialized compilation of that library.
    RayTracingPipelineDescription BuildPathTracePipelineDescription(PathTracePrimary primary)
    {
        RayTracingPipelineDescription pipelineDescription{};
        pipelineDescription.shaderPath = "PathTraceReference.hlsl";

        switch(primary)
        {
        case PathTracePrimary::Camera:
            pipelineDescription.rayGenerationShader = L"PathTraceRayGeneration";
            break;
        case PathTracePrimary::PixelCenter:
            pipelineDescription.rayGenerationShader = L"PathTracePixelCenterRayGeneration";
            break;
        case PathTracePrimary::Raster:
            pipelineDescription.rayGenerationShader = L"PathTraceRasterPrimaryRayGeneration";
            break;
        case PathTracePrimary::RasterReSTIRDI:
            // RTXPT.3: the RTXDI-specialized compilation of the same source (PathTraceReSTIRDI.hlsl).
            pipelineDescription.shaderPath = "PathTraceReSTIRDI.hlsl";
            pipelineDescription.rayGenerationShader = L"PathTraceRasterPrimaryReSTIRDIRayGeneration";
            break;
        }

        pipelineDescription.missShaders = {L"PathTraceMiss", L"PathTraceVisibilityMiss"};
        pipelineDescription.hitGroups = {{L"PathTraceHitGroup", L"PathTraceClosestHit", L""}};

        pipelineDescription.rootParameters = {{RootParameterType::ShaderResource, 0},
                                              {RootParameterType::DescriptorTable, 0, 0, D3D12_SHADER_VISIBILITY_ALL,
                                               DescriptorRangeType::UnorderedAccess, 1},
                                              {RootParameterType::DescriptorTable, 1, 0, D3D12_SHADER_VISIBILITY_ALL,
                                               DescriptorRangeType::UnorderedAccess, 1},
                                              {RootParameterType::DescriptorTable, 2, 0, D3D12_SHADER_VISIBILITY_ALL,
                                               DescriptorRangeType::UnorderedAccess, 1},
                                              {RootParameterType::ConstantBuffer, 0},
                                              {RootParameterType::ConstantBuffer, 1}};

        // RTXPT.3: RTXDI constants (b2), in the ReSTIR DI variant's own pipeline only; the other pipelines'
        // root signatures are unchanged.
        if(primary == PathTracePrimary::RasterReSTIRDI)
        {
            pipelineDescription.rootParameters.push_back({RootParameterType::ConstantBuffer, 2});
        }

        // Hit distance, geometric normal, shading normal, UV, material index.
        pipelineDescription.maxPayloadSize = 40;
        pipelineDescription.maxAttributeSize = 8;
        // Raygen traces every segment iteratively; closest hit and miss never trace.
        pipelineDescription.maxRecursionDepth = 1;
        return pipelineDescription;
    }
}

bool PathTraceReferencePass::Initialize(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height,
    std::string& failureReason)
{
    Shutdown();

    if(!graphicsDevice.SupportsRayTracing() || !graphicsDevice.GetRayTracingDevice())
    {
        failureReason = "DXR ray tracing is not supported by this device.";
        return false;
    }

    if(width == 0 || height == 0)
    {
        failureReason = "The scene extent is empty.";
        return false;
    }

    // A missing shader (for example a historical cooked bundle without this library) fails here.
    auto& referencePipeline = m_pipelines[static_cast<size_t>(PathTracePrimary::Camera)];
    const RayTracingPipelineDescription description = BuildPathTracePipelineDescription(PathTracePrimary::Camera);
    const ShaderLibraryCompileResult* library = AcquireLibrary(shaderLibrary, description.shaderPath);

    if(!library ||
        !referencePipeline.Initialize(graphicsDevice.GetRayTracingDevice(), library->bytecode, description))
    {
        failureReason = "The PathTraceReference DXR library is unavailable or failed to compile.";
        Shutdown();
        return false;
    }

    m_pipelineReady[static_cast<size_t>(PathTracePrimary::Camera)] = true;

    if(!CreateTextures(graphicsDevice.GetDevice(), descriptorHeap, width, height))
    {
        failureReason = "Path tracing accumulation resources or descriptors could not be allocated.";
        Shutdown();
        return false;
    }

    m_initialized = true;
    return true;
}

bool PathTraceReferencePass::EnsurePrimaryPipeline(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    PathTracePrimary primary,
    std::string& failureReason)
{
    const size_t index = static_cast<size_t>(primary);

    if(!m_initialized)
    {
        failureReason = "The path tracer is not initialized.";
        return false;
    }

    if(m_pipelineReady[index])
    {
        return true;
    }

    if(m_pipelineFailed[index])
    {
        failureReason = m_pipelineFailure[index];
        return false;
    }

    // A library cooked before RTXPT.2 R3 lacks this export, and a state object naming a missing export is a
    // D3D12 error. The export's name must therefore appear in the library's DXIL function table first.
    const RayTracingPipelineDescription description = BuildPathTracePipelineDescription(primary);
    const std::string libraryName = description.shaderPath.generic_string();
    const ShaderLibraryCompileResult* library = AcquireLibrary(shaderLibrary, description.shaderPath);

    if(!library)
    {
        m_pipelineFailure[index] = "The " + libraryName + " DXR library is unavailable or failed to compile.";
    }
    else if(!LibraryNamesExport(library->bytecode, description.rayGenerationShader))
    {
        m_pipelineFailure[index] = "The " + libraryName + " DXR library has no entry point for this primary "
            "(a shader bundle from before RTXPT.2 R3).";
    }
    else if(!m_pipelines[index].Initialize(graphicsDevice.GetRayTracingDevice(), library->bytecode, description))
    {
        m_pipelineFailure[index] = "The " + libraryName + " DXR pipeline could not be created for this primary.";
    }
    else
    {
        m_pipelineReady[index] = true;
        return true;
    }

    m_pipelineFailed[index] = true;
    failureReason = m_pipelineFailure[index];
    return false;
}

const ShaderLibraryCompileResult* PathTraceReferencePass::AcquireLibrary(
    ShaderLibrary& shaderLibrary,
    const std::filesystem::path& path)
{
    CompiledLibrary* entry = nullptr;

    for(CompiledLibrary& compiled : m_libraries)
    {
        if(compiled.path == path)
        {
            entry = &compiled;
            break;
        }
    }

    // Reused only while every source the compilation read is unchanged; otherwise it compiles again, exactly
    // as each pipeline used to.
    if(entry && shaderLibrary.IsLibraryCurrent(entry->library))
    {
        return &entry->library;
    }

    if(!entry)
    {
        entry = &m_libraries.emplace_back(CompiledLibrary{path, {}});
    }

    if(!shaderLibrary.CompileLibrary(path, entry->library))
    {
        entry->library = {};
        return nullptr;
    }

    return &entry->library;
}

bool PathTraceReferencePass::CreateTextures(
    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    if(!device || width == 0 || height == 0)
    {
        return false;
    }

    constexpr D3D12_RESOURCE_FLAGS unorderedAccess = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    constexpr D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    if(!m_accumulationTexture.Initialize(device, width, height, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT,
        unorderedAccess, initialState) ||
        !m_accumulationTexture.CreateSRV(device, descriptorHeap) ||
        !m_accumulationTexture.CreateUAV(device, descriptorHeap))
    {
        Log::Error("Failed to create the path tracing accumulation texture.");
        return false;
    }

    m_accumulationTexture.GetResource()->SetName(L"Path Trace Accumulation");

    if(!m_outputTexture.Initialize(device, width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
        unorderedAccess, initialState) ||
        !m_outputTexture.CreateSRV(device, descriptorHeap) ||
        !m_outputTexture.CreateUAV(device, descriptorHeap))
    {
        Log::Error("Failed to create the path tracing output texture.");
        return false;
    }

    m_outputTexture.GetResource()->SetName(L"Path Trace Output");

    // Statistics survive resize: they are cumulative and only read as differences.
    if(!m_statisticsTexture.GetResource())
    {
        if(!m_statisticsTexture.Initialize(device, StatisticsWidth, 1, 1, 1, DXGI_FORMAT_R32_UINT,
            unorderedAccess, initialState) ||
            !m_statisticsTexture.CreateUAV(device, descriptorHeap))
        {
            Log::Error("Failed to create the path tracing statistics texture.");
            return false;
        }

        m_statisticsTexture.GetResource()->SetName(L"Path Trace Statistics");
    }

    m_width = width;
    m_height = height;
    return true;
}

bool PathTraceReferencePass::Resize(
    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    if(!m_initialized)
    {
        return true;
    }

    m_accumulationTexture.Shutdown();
    m_outputTexture.Shutdown();

    if(!CreateTextures(device, descriptorHeap, width, height))
    {
        Shutdown();
        return false;
    }

    return true;
}

void PathTraceReferencePass::Execute(
    PathTracePrimary primary,
    ID3D12GraphicsCommandList4* commandList,
    DescriptorHeap& descriptorHeap,
    const RayTracingSceneResources& rayTracingScene,
    D3D12_GPU_VIRTUAL_ADDRESS constantsAddress,
    D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstantsAddress,
    unsigned int width,
    unsigned int height,
    D3D12_GPU_VIRTUAL_ADDRESS rtxdiConstantsAddress)
{
    RayTracingPipeline& pipeline = m_pipelines[static_cast<size_t>(primary)];
    const bool usesRtxdi = primary == PathTracePrimary::RasterReSTIRDI;

    if(!m_initialized || !commandList || width != m_width || height != m_height ||
        !m_pipelineReady[static_cast<size_t>(primary)] || (usesRtxdi && rtxdiConstantsAddress == 0))
    {
        return;
    }

    m_accumulationTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_outputTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_statisticsTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    pipeline.Bind(commandList);

    commandList->SetComputeRootShaderResourceView(0, rayTracingScene.GetTopLevel().GetGPUAddress());
    commandList->SetComputeRootDescriptorTable(1, descriptorHeap.GetGPUHandle(m_accumulationTexture.GetUAVIndex()));
    commandList->SetComputeRootDescriptorTable(2, descriptorHeap.GetGPUHandle(m_outputTexture.GetUAVIndex()));
    commandList->SetComputeRootDescriptorTable(3, descriptorHeap.GetGPUHandle(m_statisticsTexture.GetUAVIndex()));
    commandList->SetComputeRootConstantBufferView(4, constantsAddress);
    commandList->SetComputeRootConstantBufferView(5, sceneLightConstantsAddress);

    if(usesRtxdi)
    {
        commandList->SetComputeRootConstantBufferView(6, rtxdiConstantsAddress);
    }

    pipeline.Dispatch(commandList, width, height);

    // The next frame reads this frame's running mean; order the UAV accesses explicitly.
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = m_accumulationTexture.GetResource();
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[1].UAV.pResource = m_statisticsTexture.GetResource();
    commandList->ResourceBarrier(2, barriers);
}

void PathTraceReferencePass::CopyToSceneColor(ID3D12GraphicsCommandList* commandList, RenderTarget& sceneColor)
{
    if(!m_initialized || !commandList)
    {
        return;
    }

    m_outputTexture.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    sceneColor.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyResource(sceneColor.GetResource(), m_outputTexture.GetResource());

    // Tone mapping transitions scene colour to a shader resource itself; leave it as a render target
    // so the state matches what the legacy deferred pass leaves behind.
    sceneColor.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_outputTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

std::uint64_t PathTraceReferencePass::GetResourceBytes() const
{
    if(!m_initialized)
    {
        return 0;
    }

    const std::uint64_t pixels = static_cast<std::uint64_t>(m_width) * m_height;
    return pixels * (16 + 8) + StatisticsWidth * 4;
}

void PathTraceReferencePass::Shutdown()
{
    m_accumulationTexture.Shutdown();
    m_outputTexture.Shutdown();
    m_statisticsTexture.Shutdown();
    for(size_t index = 0; index < PrimaryCount; ++index)
    {
        m_pipelines[index].Shutdown();
        m_pipelineReady[index] = false;
        m_pipelineFailed[index] = false;
        m_pipelineFailure[index].clear();
    }

    m_libraries.clear();

    m_width = 0;
    m_height = 0;
    m_initialized = false;
}
