#include "PathTraceReferencePass.h"

#include "Core/Log.h"

#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/RenderTarget.h"

#include "Renderer/Shader/ShaderLibrary.h"

#include "Renderer/RayTracing/RayTracingPipelineDescription.h"
#include "Renderer/RayTracing/RayTracingSceneResources.h"

#include <algorithm>
#include <iterator>
#include <optional>
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

    const char* ReferenceLibraryPath(PathTraceLibraryVariant variant)
    {
        switch(variant)
        {
        case PathTraceLibraryVariant::SM69:
            return "PathTraceReferenceSM69.hlsl";
        case PathTraceLibraryVariant::SM69SER:
            return "PathTraceReferenceSERSM69.hlsl";
        case PathTraceLibraryVariant::SM66:
            break;
        }

        return "PathTraceReference.hlsl";
    }

    const char* ReSTIRDILibraryPath(PathTraceLibraryVariant variant)
    {
        switch(variant)
        {
        case PathTraceLibraryVariant::SM69:
            return "PathTraceReSTIRDISM69.hlsl";
        case PathTraceLibraryVariant::SM69SER:
            return "PathTraceReSTIRDISERSM69.hlsl";
        case PathTraceLibraryVariant::SM66:
            break;
        }

        return "PathTraceReSTIRDI.hlsl";
    }

    // One shared library; each primary constructor is its own ray-generation export. The RTXPT.3 ReSTIR DI
    // primary uses the RTXDI-specialized compilation of that library. The library variant (SER S6B) picks the
    // wrappers of both libraries; exports, bindings and root signatures are identical in every variant.
    RayTracingPipelineDescription BuildPathTracePipelineDescription(
        PathTracePrimary primary,
        PathTraceLibraryVariant variant)
    {
        RayTracingPipelineDescription pipelineDescription{};
        pipelineDescription.shaderPath = ReferenceLibraryPath(variant);

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
            pipelineDescription.shaderPath = ReSTIRDILibraryPath(variant);

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

bool PathTraceReferencePass::LibrarySelectionFixed() const
{
    // Libraries and pipelines are keyed by primary, not by variant, so the choice is fixed once anything has
    // been compiled, submitted or created from it.
    const bool anyPipeline = std::find(std::begin(m_pipelineReady), std::end(m_pipelineReady), true) !=
        std::end(m_pipelineReady);

    return m_initialized || anyPipeline || !m_libraries.empty() || !m_failedLibraries.empty();
}

bool PathTraceReferencePass::SelectLibraryVariant(PathTraceLibraryVariant variant)
{
    if(variant == m_libraryVariant)
    {
        return true;
    }

    if(LibrarySelectionFixed())
    {
        return false;
    }

    m_libraryVariant = variant;
    return true;
}

std::string PathTraceReferencePass::TroubleshootingHint() const
{
    if(m_libraryVariant == PathTraceLibraryVariant::SM66)
    {
        return {};
    }

    return std::string(" (library variant ") + PathTraceLibraryVariantName(m_libraryVariant) +
        "; set ENGINE_PATH_TRACE_LIBRARIES=sm66 to use the conventional SM 6.6 libraries)";
}

bool PathTraceReferencePass::Initialize(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height,
    std::string& failureReason)
{
    ReleaseResources();

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

    // Compiled libraries survive, including those accepted from the async worker, so a later Initialize
    // never recompiles an unchanged library. A missing shader (for example a historical cooked bundle
    // without this library) fails here.
    auto& referencePipeline = m_pipelines[static_cast<size_t>(PathTracePrimary::Camera)];
    const RayTracingPipelineDescription description =
        BuildPathTracePipelineDescription(PathTracePrimary::Camera, m_libraryVariant);
    const ShaderLibraryCompileResult* library = AcquireLibrary(shaderLibrary, description.shaderPath);

    if(!library ||
        !referencePipeline.Initialize(graphicsDevice.GetRayTracingDevice(), library->bytecode, description))
    {
        failureReason = "The PathTraceReference DXR library is unavailable or failed to compile" +
            TroubleshootingHint() + ".";
        ReleaseResources();
        return false;
    }

    m_pipelineReady[static_cast<size_t>(PathTracePrimary::Camera)] = true;

    if(!CreateTextures(graphicsDevice.GetDevice(), descriptorHeap, width, height))
    {
        failureReason = "Path tracing accumulation resources or descriptors could not be allocated.";
        ReleaseResources();
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
    const RayTracingPipelineDescription description =
        BuildPathTracePipelineDescription(primary, m_libraryVariant);
    const std::string libraryName = description.shaderPath.generic_string();
    const ShaderLibraryCompileResult* library = AcquireLibrary(shaderLibrary, description.shaderPath);

    if(!library)
    {
        m_pipelineFailure[index] = "The " + libraryName + " DXR library is unavailable or failed to compile" +
            TroubleshootingHint() + ".";
    }
    else if(!LibraryNamesExport(library->bytecode, description.rayGenerationShader))
    {
        m_pipelineFailure[index] = "The " + libraryName + " DXR library has no entry point for this primary "
            "(a shader bundle from before RTXPT.2 R3).";
    }
    else if(!m_pipelines[index].Initialize(graphicsDevice.GetRayTracingDevice(), library->bytecode, description))
    {
        m_pipelineFailure[index] = "The " + libraryName + " DXR pipeline could not be created for this primary" +
            TroubleshootingHint() + ".";
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

ShaderLibraryPreparation PathTraceReferencePass::PrepareLibrary(
    ShaderLibrary& shaderLibrary,
    ShaderLibraryCompileWorker& worker,
    PathTracePrimary primary)
{
    const std::filesystem::path path =
        BuildPathTracePipelineDescription(primary, m_libraryVariant).shaderPath;
    const std::string libraryName = path.generic_string();
    ShaderLibraryPreparation preparation{};

    switch(worker.GetState(path))
    {
    case ShaderLibraryCompileState::Queued:
        preparation.state = ShaderLibraryPreparationState::Queued;
        return preparation;
    case ShaderLibraryCompileState::Compiling:
        preparation.state = ShaderLibraryPreparationState::Compiling;
        return preparation;
    case ShaderLibraryCompileState::Completed:
    case ShaderLibraryCompileState::None:
        break;
    }

    std::optional<ShaderLibraryCompileOutcome> completed = worker.TakeCompleted(path);
    const auto failed = std::find_if(m_failedLibraries.begin(), m_failedLibraries.end(),
        [&](const FailedLibrary& failedLibrary)
        {
            return failedLibrary.path == path;
        });

    // A result is advisory until accepted here: one compiled from sources that have since changed, whether
    // it succeeded or failed, is discarded and compiled again.
    if(completed && !ShaderLibraryCompileWorker::IsCurrent(*completed))
    {
        Log::Info("Discarded a stale " + libraryName + " compilation; its sources changed.");
        return SubmitLibrary(shaderLibrary, worker, path);
    }

    if(completed && !completed->succeeded)
    {
        Log::Error("The " + libraryName + " DXR library failed to compile" + TroubleshootingHint() + ":\n" +
            completed->diagnostics);

        if(failed != m_failedLibraries.end())
        {
            m_failedLibraries.erase(failed);
        }

        preparation.state = ShaderLibraryPreparationState::Failed;
        preparation.diagnostic = "The " + libraryName + " DXR library failed to compile" + TroubleshootingHint() +
            ":\n" + completed->diagnostics;
        m_failedLibraries.push_back(FailedLibrary{path, std::move(*completed)});
        return preparation;
    }

    if(completed)
    {
        if(failed != m_failedLibraries.end())
        {
            m_failedLibraries.erase(failed);
        }

        CompiledLibrary* entry = nullptr;

        for(CompiledLibrary& compiled : m_libraries)
        {
            if(compiled.path == path)
            {
                entry = &compiled;
                break;
            }
        }

        if(!entry)
        {
            entry = &m_libraries.emplace_back(CompiledLibrary{path, {}});
        }

        entry->library = std::move(completed->library);

        // A new revision may fix what made a pipeline fail. Pipelines already created keep their state
        // objects, as they do under synchronous reuse.
        for(size_t index = 0; index < PrimaryCount; ++index)
        {
            const PathTracePrimary builtPrimary = static_cast<PathTracePrimary>(index);

            if(BuildPathTracePipelineDescription(builtPrimary, m_libraryVariant).shaderPath == path)
            {
                m_pipelineFailed[index] = false;
                m_pipelineFailure[index].clear();
            }
        }

        Log::Info("Accepted the asynchronously compiled " + libraryName + " DXR library.");
        preparation.state = ShaderLibraryPreparationState::Ready;
        preparation.acceptedNewRevision = true;
        return preparation;
    }

    // An unchanged failure is reported again without recompiling; a changed one is retried.
    if(failed != m_failedLibraries.end())
    {
        if(ShaderLibraryCompileWorker::IsCurrent(failed->outcome))
        {
            preparation.state = ShaderLibraryPreparationState::Failed;
            preparation.diagnostic = "The " + libraryName + " DXR library failed to compile" + TroubleshootingHint() +
                ":\n" + failed->outcome.diagnostics;
            return preparation;
        }

        m_failedLibraries.erase(failed);
        return SubmitLibrary(shaderLibrary, worker, path);
    }

    for(const CompiledLibrary& compiled : m_libraries)
    {
        if(compiled.path == path && shaderLibrary.IsLibraryCurrent(path, compiled.library))
        {
            preparation.state = ShaderLibraryPreparationState::Ready;
            return preparation;
        }
    }

    return SubmitLibrary(shaderLibrary, worker, path);
}

ShaderLibraryPreparation PathTraceReferencePass::SubmitLibrary(
    ShaderLibrary& shaderLibrary,
    ShaderLibraryCompileWorker& worker,
    const std::filesystem::path& path)
{
    ShaderLibraryPreparation preparation{};
    ShaderLibraryCompileRequest request{};

    if(!shaderLibrary.CreateLibraryCompileRequest(path, request) || !worker.Submit(request))
    {
        preparation.state = ShaderLibraryPreparationState::Failed;
        preparation.diagnostic = "The " + path.generic_string() + " DXR library could not be queued for compilation.";
        return preparation;
    }

    preparation.state = ShaderLibraryPreparationState::Queued;
    return preparation;
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
    if(entry && shaderLibrary.IsLibraryCurrent(path, entry->library))
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
    ReleaseResources();
    m_libraries.clear();
    m_failedLibraries.clear();
}

void PathTraceReferencePass::ReleaseResources()
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

    m_width = 0;
    m_height = 0;
    m_initialized = false;
}

const char* PathTraceLibraryVariantName(PathTraceLibraryVariant variant)
{
    switch(variant)
    {
    case PathTraceLibraryVariant::SM69:
        return "SM 6.9";
    case PathTraceLibraryVariant::SM69SER:
        return "SM 6.9 + SER";
    case PathTraceLibraryVariant::SM66:
        break;
    }

    return "SM 6.6";
}

namespace
{
    std::string LowerCase(std::string_view text)
    {
        std::string lower(text);

        for(char& character : lower)
        {
            if(character >= 'A' && character <= 'Z')
            {
                character = static_cast<char>(character - 'A' + 'a');
            }
        }

        return lower;
    }

    // The first SM 6.9 capability the device lacks, or null when it has them all.
    const char* MissingShaderModel69Capability(const PathTraceLibraryCapabilities& capabilities)
    {
        if(!capabilities.rayTracing)
        {
            return "DXR unavailable";
        }

        if(!capabilities.shaderModel69)
        {
            return "Shader Model 6.9 unsupported";
        }

        return nullptr;
    }

    // The first SER capability the device lacks, or null when it has them all.
    const char* MissingSERCapability(const PathTraceLibraryCapabilities& capabilities)
    {
        if(const char* missing = MissingShaderModel69Capability(capabilities))
        {
            return missing;
        }

        if(!capabilities.rayTracingTier12)
        {
            return "DXR tier 1.2 unsupported";
        }

        if(!capabilities.serActuallyReorders)
        {
            return "shader execution reordering does not actually reorder on this device";
        }

        return nullptr;
    }
}

PathTraceLibrarySelection SelectPathTraceLibraryVariant(
    std::string_view libraryOverride,
    const PathTraceLibraryCapabilities& capabilities)
{
    std::string requested = LowerCase(libraryOverride);
    PathTraceLibrarySelection selection{};

    if(capabilities.packagedRuntime)
    {
        selection.reason = "packaged runtime (only the SM 6.6 libraries are cooked)";
        selection.warning = !requested.empty() && requested != "auto" && requested != "sm66";

        if(selection.warning)
        {
            selection.reason += "; ENGINE_PATH_TRACE_LIBRARIES=" + std::string(libraryOverride) + " ignored";
        }

        return selection;
    }

    std::string prefix;

    if(!requested.empty() && requested != "auto" && requested != "sm66" && requested != "sm69" && requested != "ser")
    {
        prefix = "invalid ENGINE_PATH_TRACE_LIBRARIES value '" + std::string(libraryOverride) + "', using auto; ";
        selection.warning = true;
        requested = "auto";
    }

    if(requested == "sm66")
    {
        selection.reason = prefix + "ENGINE_PATH_TRACE_LIBRARIES=sm66";
        return selection;
    }

    if(requested == "sm69")
    {
        if(const char* missing = MissingShaderModel69Capability(capabilities))
        {
            selection.reason = prefix + "ENGINE_PATH_TRACE_LIBRARIES=sm69 unavailable (" + missing +
                "); falling back to SM 6.6";
            selection.warning = true;
            return selection;
        }

        selection.variant = PathTraceLibraryVariant::SM69;
        selection.reason = prefix + "ENGINE_PATH_TRACE_LIBRARIES=sm69 (diagnostic, no SER)";
        return selection;
    }

    const char* missingSER = MissingSERCapability(capabilities);

    if(requested == "ser")
    {
        if(missingSER)
        {
            selection.reason = prefix + "ENGINE_PATH_TRACE_LIBRARIES=ser unavailable (" + missingSER +
                "); falling back to SM 6.6";
            selection.warning = true;
            return selection;
        }

        selection.variant = PathTraceLibraryVariant::SM69SER;
        selection.reason = prefix + "ENGINE_PATH_TRACE_LIBRARIES=ser";
        return selection;
    }

    if(missingSER)
    {
        selection.reason = prefix + "automatic: SER unavailable (" + missingSER + ")";
        return selection;
    }

    selection.variant = PathTraceLibraryVariant::SM69SER;
    selection.reason = prefix + "automatic: Shader Model 6.9, DXR tier 1.2 and reordering supported";
    return selection;
}
