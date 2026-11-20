#include "PathTraceReferencePass.h"
#include "Debug/MemoryRegistry.h"
#include "Renderer/Shader/DxilLibraryCache.h"
#include "Debug/TransitionTiming.h"

#include "Core/Log.h"

#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/RenderTarget.h"

#include "Renderer/Shader/ShaderLibrary.h"

#include "Renderer/RayTracing/RayTracingPipelineDescription.h"
#include "Renderer/RayTracing/RayTracingSceneResources.h"

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace
{
    // RayQuery 2L diagnostics (Debug builds, ENGINE_DIAG_PIX_EVENTS set): names a path-trace dispatch as a PIX event
    // region, so profilers (Nsight GPU Trace regimes) attribute metrics to it. Metadata 0 is the unversioned PIX format
    // (a null-terminated wide string), as MarkDredStage uses. No-ops otherwise.
    void BeginPathTraceEvent([[maybe_unused]] ID3D12GraphicsCommandList* commandList, [[maybe_unused]] const wchar_t* name)
    {
#ifndef NDEBUG
        static const bool enabled = std::getenv("ENGINE_DIAG_PIX_EVENTS") != nullptr;

        if(enabled && commandList)
        {
            commandList->BeginEvent(0, name, static_cast<UINT>((std::wcslen(name) + 1) * sizeof(wchar_t)));
        }
#endif
    }

    void EndPathTraceEvent([[maybe_unused]] ID3D12GraphicsCommandList* commandList)
    {
#ifndef NDEBUG
        static const bool enabled = std::getenv("ENGINE_DIAG_PIX_EVENTS") != nullptr;

        if(enabled && commandList)
        {
            commandList->EndEvent();
        }
#endif
    }

    // RayQuery 2M E3: logs, once per distinct combination, which implementation ran the ReSTIR DI direct pass (the
    // production compute shader, or the DXR comparison backend), so the backend is identifiable in every log.
    void LogDirectPassBackend(const std::string& backend, bool extract, std::uint32_t psrChain)
    {
        static std::vector<std::string> logged;
        const std::string line = "ReSTIR DI direct pass: " + backend + (extract ? ", extraction" : "") + ", PSR chain " +
            std::to_string(psrChain);

        if(std::find(logged.begin(), logged.end(), line) != logged.end())
        {
            return;
        }

        logged.push_back(line);
        Log::Info(line + ".");
    }

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

    // SHARC.4D: the cache libraries of each variant (the reference library with PATH_TRACE_RADIANCE_CACHE_LIBRARY).
    const char* D6RadianceCacheLibraryPath(PathTraceLibraryVariant variant, PathTraceRadianceCacheLibrary library)
    {
        static constexpr const char* paths[][3] = {
            {"PathTraceReferenceRadianceCacheDeferred.hlsl", "PathTraceReferenceRadianceCacheDeferredSM69.hlsl",
                "PathTraceReferenceRadianceCacheDeferredSERSM69.hlsl"},
            {"PathTraceReferenceRadianceCacheDeferredLate.hlsl", "PathTraceReferenceRadianceCacheDeferredLateSM69.hlsl",
                "PathTraceReferenceRadianceCacheDeferredLateSERSM69.hlsl"},
            {"PathTraceReferenceRadianceCacheQuery.hlsl", "PathTraceReferenceRadianceCacheQuerySM69.hlsl",
                "PathTraceReferenceRadianceCacheQuerySERSM69.hlsl"},
            {"PathTraceReferenceRadianceCacheTraining.hlsl", "PathTraceReferenceRadianceCacheTrainingSM69.hlsl",
                "PathTraceReferenceRadianceCacheTrainingSERSM69.hlsl"},
            {"PathTraceReferenceRadianceCacheQueryTermination.hlsl",
                "PathTraceReferenceRadianceCacheQueryTerminationSM69.hlsl",
                "PathTraceReferenceRadianceCacheQueryTerminationSERSM69.hlsl"},
            {"PathTraceReferenceRadianceCacheRender.hlsl", "PathTraceReferenceRadianceCacheRenderSM69.hlsl",
                "PathTraceReferenceRadianceCacheRenderSERSM69.hlsl"},
            {"PathTraceReferenceRadianceCacheRender.hlsl", "PathTraceReferenceRadianceCacheRenderSM69.hlsl",
                "PathTraceReferenceRadianceCacheRenderSERSM69.hlsl"},
            {"PathTraceReferenceRadianceCacheRender.hlsl", "PathTraceReferenceRadianceCacheRenderSM69.hlsl",
                "PathTraceReferenceRadianceCacheRenderSERSM69.hlsl"},
            {"PathTraceReferenceRadianceCacheRender.hlsl", "PathTraceReferenceRadianceCacheRenderSM69.hlsl",
                "PathTraceReferenceRadianceCacheRenderSERSM69.hlsl"},
            {"PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistance.hlsl",
                "PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistanceSM69.hlsl",
                "PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistanceSERSM69.hlsl"},
            {"PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistance.hlsl",
                "PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistanceSM69.hlsl",
                "PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistanceSERSM69.hlsl"},
            {"PathTraceReferencePsrSharcTraining.hlsl", "PathTraceReferencePsrSharcTrainingSM69.hlsl", "PathTraceReferencePsrSharcTrainingSERSM69.hlsl"},
            {"PathTraceReferencePsrSharcRender.hlsl", "PathTraceReferencePsrSharcRenderSM69.hlsl", "PathTraceReferencePsrSharcRenderSERSM69.hlsl"},
            {"PathTraceReferencePsrSharcRender.hlsl", "PathTraceReferencePsrSharcRenderSM69.hlsl", "PathTraceReferencePsrSharcRenderSERSM69.hlsl"},
            {"PathTraceReferencePsrSharcRenderRawDiffuseHitDistance.hlsl", "PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSM69.hlsl", "PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSERSM69.hlsl"}};
        static_assert(std::size(paths) == static_cast<size_t>(PathTraceRadianceCacheLibrary::Count));
        size_t column = 0;

        switch(variant)
        {
        case PathTraceLibraryVariant::SM69:
            column = 1;
            break;
        case PathTraceLibraryVariant::SM69SER:
            column = 2;
            break;
        case PathTraceLibraryVariant::SM66:
            break;
        }

        return paths[static_cast<size_t>(library)][column];
    }

    // NVIDIA SHARC v1.8.3: its variant tree (SharcNv/, compiled with -enable-16bit-types). The training slot holds the
    // update library (with cache resampling, or without it for the depth-matched diagnostic); the four render slots share
    // one library and the two raw-distance slots another, exactly as the D6 cache's. The D6 cache's diagnostic and PSR
    // libraries have no NVIDIA counterpart (nullptr).
    const char* NvidiaRadianceCacheLibraryPath(PathTraceLibraryVariant variant, PathTraceRadianceCacheLibrary library,
        bool depthMatched)
    {
        static constexpr const char* training[] = {"SharcNv/PathTraceSharcNvTraining.hlsl",
            "SharcNv/PathTraceSharcNvTrainingSM69.hlsl", "SharcNv/PathTraceSharcNvTrainingSERSM69.hlsl"};
        static constexpr const char* trainingDepthMatched[] = {"SharcNv/PathTraceSharcNvTrainingDepthMatched.hlsl",
            "SharcNv/PathTraceSharcNvTrainingDepthMatchedSM69.hlsl", "SharcNv/PathTraceSharcNvTrainingDepthMatchedSERSM69.hlsl"};
        static constexpr const char* render[] = {"SharcNv/PathTraceSharcNvRender.hlsl", "SharcNv/PathTraceSharcNvRenderSM69.hlsl",
            "SharcNv/PathTraceSharcNvRenderSERSM69.hlsl"};
        static constexpr const char* renderRaw[] = {"SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistance.hlsl",
            "SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistanceSM69.hlsl",
            "SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistanceSERSM69.hlsl"};
        // PSR.2: the same libraries with the PSR G-buffer (PsrChainLibraryPath adds the two-bounce PsrK2/ prefix).
        static constexpr const char* psrTraining[] = {"SharcNv/PathTraceSharcNvPsrTraining.hlsl",
            "SharcNv/PathTraceSharcNvPsrTrainingSM69.hlsl", "SharcNv/PathTraceSharcNvPsrTrainingSERSM69.hlsl"};
        static constexpr const char* psrTrainingDepthMatched[] = {"SharcNv/PathTraceSharcNvPsrTrainingDepthMatched.hlsl",
            "SharcNv/PathTraceSharcNvPsrTrainingDepthMatchedSM69.hlsl",
            "SharcNv/PathTraceSharcNvPsrTrainingDepthMatchedSERSM69.hlsl"};
        static constexpr const char* psrRender[] = {"SharcNv/PathTraceSharcNvPsrRender.hlsl",
            "SharcNv/PathTraceSharcNvPsrRenderSM69.hlsl", "SharcNv/PathTraceSharcNvPsrRenderSERSM69.hlsl"};
        static constexpr const char* psrRenderRaw[] = {"SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistance.hlsl",
            "SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSM69.hlsl",
            "SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSERSM69.hlsl"};
        size_t column = 0;

        if(variant == PathTraceLibraryVariant::SM69)
        {
            column = 1;
        }
        else if(variant == PathTraceLibraryVariant::SM69SER)
        {
            column = 2;
        }

        switch(library)
        {
        case PathTraceRadianceCacheLibrary::Training:
            return depthMatched ? trainingDepthMatched[column] : training[column];
        case PathTraceRadianceCacheLibrary::Render:
        case PathTraceRadianceCacheLibrary::RenderExtraction:
        case PathTraceRadianceCacheLibrary::RenderSplitIndirect:
        case PathTraceRadianceCacheLibrary::RenderSplitIndirectExtraction:
            return render[column];
        case PathTraceRadianceCacheLibrary::RenderExtractionRawDiffuseHitDistance:
        case PathTraceRadianceCacheLibrary::RenderSplitIndirectExtractionRawDiffuseHitDistance:
            return renderRaw[column];
        case PathTraceRadianceCacheLibrary::PsrTraining:
            return depthMatched ? psrTrainingDepthMatched[column] : psrTraining[column];
        case PathTraceRadianceCacheLibrary::PsrRenderSplitIndirect:
        case PathTraceRadianceCacheLibrary::PsrRenderSplitIndirectExtraction:
            return psrRender[column];
        case PathTraceRadianceCacheLibrary::PsrRenderSplitIndirectExtractionRawDiffuseHitDistance:
            return psrRenderRaw[column];
        default:
            return nullptr;
        }
    }

    const char* RadianceCacheLibraryPath(PathTraceLibraryVariant variant, PathTraceRadianceCacheLibrary library,
        PathTraceRadianceCacheImplementation implementation = PathTraceRadianceCacheImplementation::EngineD6)
    {
        switch(implementation)
        {
        case PathTraceRadianceCacheImplementation::Nvidia:
            return NvidiaRadianceCacheLibraryPath(variant, library, false);
        case PathTraceRadianceCacheImplementation::NvidiaDepthMatched:
            return NvidiaRadianceCacheLibraryPath(variant, library, true);
        case PathTraceRadianceCacheImplementation::EngineD6:
        case PathTraceRadianceCacheImplementation::Count:
            break;
        }

        return D6RadianceCacheLibraryPath(variant, library);
    }

    bool IsPsrCacheLibrary(PathTraceRadianceCacheLibrary library)
    {
        return library >= PathTraceRadianceCacheLibrary::PsrTraining && library < PathTraceRadianceCacheLibrary::Count;
    }

    PathTraceRadianceCacheLibrary PsrRenderLibrary(bool extract, bool raw)
    {
        return !extract ? PathTraceRadianceCacheLibrary::PsrRenderSplitIndirect : raw ?
            PathTraceRadianceCacheLibrary::PsrRenderSplitIndirectExtractionRawDiffuseHitDistance :
            PathTraceRadianceCacheLibrary::PsrRenderSplitIndirectExtraction;
    }

    // NRD.RELAX.2B: the reference library built with PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY. Only its extraction
    // exports are used; the production and legacy extraction pipelines never come from it.
    const char* RawDiffuseHitDistanceLibraryPath(PathTraceLibraryVariant variant)
    {
        switch(variant)
        {
        case PathTraceLibraryVariant::SM69:
            return "PathTraceReferenceRawDiffuseHitDistanceSM69.hlsl";
        case PathTraceLibraryVariant::SM69SER:
            return "PathTraceReferenceRawDiffuseHitDistanceSERSM69.hlsl";
        case PathTraceLibraryVariant::SM66:
            break;
        }

        return "PathTraceReferenceRawDiffuseHitDistance.hlsl";
    }

    // PSR.1a: the reference library built with PATH_TRACE_PSR_LIBRARY. The production libraries never contain its code.
    const char* PrimarySurfaceReplacementLibraryPath(PathTraceLibraryVariant variant)
    {
        switch(variant)
        {
        case PathTraceLibraryVariant::SM69:
            return "PathTraceReferencePsrSM69.hlsl";
        case PathTraceLibraryVariant::SM69SER:
            return "PathTraceReferencePsrSERSM69.hlsl";
        case PathTraceLibraryVariant::SM66:
            break;
        }

        return "PathTraceReferencePsr.hlsl";
    }

    // PSR.2 Stage 3A: the indirect pass's library with PATH_TRACE_PSR_GBUFFER (G-buffer storage). The direct pass is a
    // compute shader (DirectPassDescription).
    const char* PrimarySurfaceReplacementGBufferLibraryPath(PathTraceLibraryVariant variant)
    {
        switch(variant)
        {
        case PathTraceLibraryVariant::SM69:
            return "PathTraceReferencePsrGBufferSM69.hlsl";
        case PathTraceLibraryVariant::SM69SER:
            return "PathTraceReferencePsrGBufferSERSM69.hlsl";
        case PathTraceLibraryVariant::SM66:
            break;
        }

        return "PathTraceReferencePsrGBuffer.hlsl";
    }

    // PSR.2 Stage 3B: the indirect extraction libraries with PATH_TRACE_PSR_GBUFFER. The extraction indirect export is
    // in the PSR.2 reference library; the raw-distance one has a library of its own.
    const char* PrimarySurfaceReplacementGBufferExtractionLibraryPath(bool rawDiffuseHitDistance,
        PathTraceLibraryVariant variant)
    {
        if(!rawDiffuseHitDistance)
        {
            return PrimarySurfaceReplacementGBufferLibraryPath(variant);
        }

        switch(variant)
        {
        case PathTraceLibraryVariant::SM69:
            return "PathTraceReferenceRawDiffuseHitDistancePsrGBufferSM69.hlsl";
        case PathTraceLibraryVariant::SM69SER:
            return "PathTraceReferenceRawDiffuseHitDistancePsrGBufferSERSM69.hlsl";
        case PathTraceLibraryVariant::SM66:
            break;
        }

        return "PathTraceReferenceRawDiffuseHitDistancePsrGBuffer.hlsl";
    }

    bool IsRawDiffuseHitDistanceLibrary(PathTraceRadianceCacheLibrary library)
    {
        return library == PathTraceRadianceCacheLibrary::RenderExtractionRawDiffuseHitDistance ||
            library == PathTraceRadianceCacheLibrary::RenderSplitIndirectExtractionRawDiffuseHitDistance ||
            library == PathTraceRadianceCacheLibrary::PsrRenderSplitIndirectExtractionRawDiffuseHitDistance;
    }


    // The library a primary's DXR pipelines come from: the reference library for every primary (RTXPT.3 ReSTIR DI's
    // indirect pass is in it; its direct pass is a compute shader, DirectPassDescription).
    std::filesystem::path PathTraceLibraryPath(PathTracePrimary, PathTraceLibraryVariant variant)
    {
        return ReferenceLibraryPath(variant);
    }

    // One shared library; each single-pipeline primary constructor is its own ray-generation export. RTXPT.3 ReSTIR
    // DI is the split pipeline pair instead (BuildReSTIRDISplitPipelineDescription). The library variant (SER S6B)
    // picks the wrappers of both libraries; exports, bindings and root signatures are identical in every variant.
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
            // Never built as one pipeline: BuildReSTIRDISplitPipelineDescription builds its two.
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

        // Hit distance, geometric normal, shading normal, UV, material index.
        pipelineDescription.maxPayloadSize = 40;
        pipelineDescription.maxAttributeSize = 8;
        // Raygen traces every segment iteratively; closest hit and miss never trace.
        pipelineDescription.maxRecursionDepth = 1;
        return pipelineDescription;
    }

    // RTXPT.3 ReSTIR DI: one float3 per pixel and sample (PathTraceSplitDirectRadianceAddress).
    constexpr std::uint64_t SplitDirectRadianceStride = 12;

    // The direct pass is an export of the ReSTIR DI library with the Raster Primary root signature plus the RTXDI
    // constants (b2); the indirect pass is an export of the reference library, without RTXDI, with the Raster
    // Primary root signature.
    RayTracingPipelineDescription BuildReSTIRDISplitPipelineDescription(
        bool directPass,
        PathTraceLibraryVariant variant)
    {
        if(directPass)
        {
            RayTracingPipelineDescription description =
                BuildPathTracePipelineDescription(PathTracePrimary::Raster, variant);
            // The direct pass's root signature and hit groups; DirectPassDescription supplies its compute shader.
            description.shaderPath.clear();
            description.rayGenerationShader = ShaderCompiler::ComputeLibraryEntryPoint;
            description.rootParameters.push_back({RootParameterType::ConstantBuffer, 2});
            return description;
        }

        RayTracingPipelineDescription description = BuildPathTracePipelineDescription(PathTracePrimary::Raster, variant);
        description.rayGenerationShader = L"PathTraceReSTIRDISplitIndirectRayGeneration";
        return description;
    }

    // SHARC.4D: the Raster Primary pipeline with a learning library and the cache binding (b3) appended.
    RayTracingPipelineDescription BuildRadianceCachePipelineDescription(PathTraceLibraryVariant variant,
        PathTraceRadianceCacheLibrary library,
        PathTraceRadianceCacheImplementation implementation = PathTraceRadianceCacheImplementation::EngineD6)
    {
        RayTracingPipelineDescription description = BuildPathTracePipelineDescription(PathTracePrimary::Raster, variant);
        const char* path = RadianceCacheLibraryPath(variant, library, implementation);
        description.shaderPath = path ? path : "";
        const bool nvidia = implementation != PathTraceRadianceCacheImplementation::EngineD6;

        if(library == PathTraceRadianceCacheLibrary::Training || library == PathTraceRadianceCacheLibrary::PsrTraining)
        {
            description.rayGenerationShader = nvidia ? L"PathTraceSharcNvTrainingRayGeneration" :
                L"PathTraceRadianceCacheTrainingRayGeneration";
        }
        else if(library == PathTraceRadianceCacheLibrary::RenderExtraction ||
            library == PathTraceRadianceCacheLibrary::RenderExtractionRawDiffuseHitDistance)
        {
            description.rayGenerationShader = L"PathTraceRasterPrimaryExtractRayGeneration";
        }
        else if(library == PathTraceRadianceCacheLibrary::RenderSplitIndirect ||
            library == PathTraceRadianceCacheLibrary::PsrRenderSplitIndirect)
        {
            description.rayGenerationShader = L"PathTraceReSTIRDISplitIndirectRayGeneration";
        }
        else if(library == PathTraceRadianceCacheLibrary::RenderSplitIndirectExtraction ||
            library == PathTraceRadianceCacheLibrary::RenderSplitIndirectExtractionRawDiffuseHitDistance ||
            library == PathTraceRadianceCacheLibrary::PsrRenderSplitIndirectExtraction ||
            library == PathTraceRadianceCacheLibrary::PsrRenderSplitIndirectExtractionRawDiffuseHitDistance)
        {
            description.rayGenerationShader = L"PathTraceReSTIRDISplitIndirectExtractRayGeneration";
        }

        description.rootParameters.push_back({RootParameterType::ConstantBuffer, 3});
        if(IsPsrCacheLibrary(library))
        {
            description.rootParameters.push_back({RootParameterType::ConstantBuffer, 4});
            description.rootParameters.push_back({RootParameterType::ShaderResource, 2});
            description.rasterMaterialSampler = true;
        }
        return description;
    }

    // RTXPT D.1B.2: each extraction pipeline is its production counterpart with the extraction ray generation; the
    // direct pass's comes from its own library (ReSTIRDIExtractionLibraryPath). They bind nothing more: the outputs
    // are reached through the descriptor heap.
    RayTracingPipelineDescription BuildRasterExtractionPipelineDescription(PathTraceLibraryVariant variant)
    {
        RayTracingPipelineDescription description = BuildPathTracePipelineDescription(PathTracePrimary::Raster, variant);
        description.rayGenerationShader = L"PathTraceRasterPrimaryExtractRayGeneration";
        return description;
    }

    // PSR.1a: the Raster Primary description with the PSR library and, for the record pass, its record ray generation.
    // Same root signature and bindings; the records are reached through the descriptor heap.
    RayTracingPipelineDescription BuildPrimarySurfaceReplacementPipelineDescription(bool recordPass,
        PathTraceLibraryVariant variant)
    {
        RayTracingPipelineDescription description = BuildPathTracePipelineDescription(PathTracePrimary::Raster, variant);
        description.shaderPath = PrimarySurfaceReplacementLibraryPath(variant);

        if(recordPass)
        {
            description.rayGenerationShader = L"PathTracePsrRayGeneration";
        }

        return description;
    }

    // PSR.1b: the record pass's description with the guide ray generation, its guide hit group and miss (payload
    // PathTracePsrGuidePayload, 48 bytes), the guide constants (b3) and the instance transforms (t1, root SRV).
    RayTracingPipelineDescription BuildPrimarySurfaceReplacementGuidePipelineDescription(PathTraceLibraryVariant variant)
    {
        RayTracingPipelineDescription description = BuildPrimarySurfaceReplacementPipelineDescription(true, variant);
        description.rayGenerationShader = L"PathTracePsrGuideRayGeneration";
        description.missShaders = {L"PathTracePsrGuideMiss", L"PathTraceVisibilityMiss"};
        description.hitGroups = {{L"PathTracePsrGuideHitGroup", L"PathTracePsrGuideClosestHit", L""}};
        description.rootParameters.push_back({RootParameterType::ConstantBuffer, 3});
        description.rootParameters.push_back({RootParameterType::ShaderResource, 1});
        description.maxPayloadSize = 48;
        return description;
    }

    // PSR.1b: PATH_TRACE_PSR_GUIDE_STRIDE.
    constexpr std::uint64_t PrimarySurfaceReplacementGuideStride = 32;

    // PSR.1a-P: PATH_TRACE_PSR_RECORD_STRIDE.
    constexpr std::uint64_t PrimarySurfaceReplacementRecordStride = 40;

    RayTracingPipelineDescription BuildReSTIRDISplitExtractionPipelineDescription(
        bool directPass,
        PathTraceLibraryVariant variant)
    {
        RayTracingPipelineDescription description = BuildReSTIRDISplitPipelineDescription(directPass, variant);

        if(directPass)
        {
            description.rayGenerationShader = ShaderCompiler::ComputeLibraryEntryPoint;
        }
        else
        {
            description.rayGenerationShader = L"PathTraceReSTIRDISplitIndirectExtractRayGeneration";
        }

        return description;
    }

    // RayQuery 2M E3: the production ReSTIR DI direct pass's compute shader for a PSR chain (0: PSR off) and NRD signal
    // extraction. DirectLighting/ is compiled as cs_6_6 (ShaderLibraryProfile::Compute66).
    std::filesystem::path DirectLightingComputePath(bool extraction, std::uint32_t psrChain)
    {
        std::string name = "ReSTIRDIDirect";

        if(psrChain >= 2u)
        {
            name += "PsrK2";
        }
        else if(psrChain == 1u)
        {
            name += "Psr";
        }

        if(extraction)
        {
            name += "Extract";
        }

        return std::filesystem::path("DirectLighting") / (name + ".hlsl");
    }

    // RayQuery 2M E3: the compute direct pass traces its one radiance ray (the two-bounce PSR chain retrace) as these DXR
    // pipelines do only while they have one hit group running PathTraceClosestHit without an any-hit shader and the
    // radiance miss PathTraceMiss (PathTraceTraceRadianceInline). A description that breaks this has no compute pass.
    bool ComputeRetraceContractHolds(const RayTracingPipelineDescription& description)
    {
        return description.hitGroups.size() == 1 && description.hitGroups[0].closestHitShader == L"PathTraceClosestHit" &&
            description.hitGroups[0].anyHitShader.empty() && !description.missShaders.empty() &&
            description.missShaders[0] == L"PathTraceMiss";
    }

    // The entry check of a pipeline's library: its ray generation export (a compute shader has one fixed entry point, which
    // pipeline creation validates).
    bool LibraryHasEntryPoint(const std::vector<char>& bytecode, const RayTracingPipelineDescription& description)
    {
        return description.computeShader || LibraryNamesExport(bytecode, description.rayGenerationShader);
    }

    // Whether the library is itself a RayQuery twin (they live under GI6/).
    bool RayQueryTwinOf(const std::filesystem::path& path)
    {
        const std::filesystem::path first = *path.begin();
        return first == "GI6";
    }

    // The RayQuery twin InitializeVisibilityPipelines pairs with a pipeline's library, if any: GI.6 pairs the reference
    // libraries except training.
    std::optional<std::filesystem::path> RayQueryTwinLibraryPath(const std::filesystem::path& path)
    {
        const std::string name = path.filename().string();

        // RayQuery 2M E3: the compute direct pass already traces inline RayQuery; it has no twin.
        if(!path.empty() && *path.begin() == "DirectLighting")
        {
            return std::nullopt;
        }

        // NVIDIA SHARC's render libraries (SharcNv/) pair like the reference render libraries; its update libraries do not.
        if((name.starts_with("PathTraceReference") || name.starts_with("PathTraceSharcNv")) &&
            name.find("Training") == std::string::npos)
        {
            return std::filesystem::path("GI6") / path;
        }

        return std::nullopt;
    }

    // ER.1 Phase 4: everything a DXR pipeline is built from besides the library bytes (its path, exports, root signature
    // and pipeline configuration), so two descriptions share a prepared pipeline only when they would build the same one.
    std::string PipelineDescriptionKey(const RayTracingPipelineDescription& description)
    {
        std::string key = description.shaderPath.generic_string();
        const auto add = [&](const std::wstring& text)
        {
            key += '|';
            key += std::filesystem::path(text).string();
        };

        add(description.rayGenerationShader);

        for(const std::wstring& missShader : description.missShaders)
        {
            add(missShader);
        }

        for(const RayTracingHitGroupDescription& hitGroup : description.hitGroups)
        {
            add(hitGroup.name);
            add(hitGroup.closestHitShader);
            add(hitGroup.anyHitShader);
        }

        for(const RootParameterDescription& parameter : description.rootParameters)
        {
            key += "|r" + std::to_string(static_cast<int>(parameter.type)) + ',' + std::to_string(parameter.shaderRegister) +
                ',' + std::to_string(parameter.constantCount) + ',' + std::to_string(static_cast<int>(parameter.visibility)) +
                ',' + std::to_string(static_cast<int>(parameter.descriptorRangeType)) + ',' +
                std::to_string(parameter.descriptorCount);
        }

        key += "|p" + std::to_string(description.maxPayloadSize) + ',' + std::to_string(description.maxAttributeSize) + ',' +
            std::to_string(description.maxRecursionDepth);

        if(description.rasterMaterialSampler)
        {
            key += "|s2";
        }

        if(description.computeShader)
        {
            key += "|cs";
        }

        return key;
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
        !InitializeVisibilityPipelines(graphicsDevice, shaderLibrary, referencePipeline, library->bytecode, description))
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
    TransitionTiming::Scope er1Scope("EnsurePrimaryPipeline", 0.5);
    const size_t index = static_cast<size_t>(primary);

    if(!m_initialized)
    {
        failureReason = "The path tracer is not initialized.";
        return false;
    }

    // RTXPT.3: ReSTIR DI is the split pipeline pair, not a pipeline of its own.
    if(primary == PathTracePrimary::RasterReSTIRDI)
    {
        return EnsureReSTIRDISplitPipelines(graphicsDevice, shaderLibrary, failureReason);
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
    else if(!LibraryHasEntryPoint(library->bytecode, description))
    {
        m_pipelineFailure[index] = "The " + libraryName + " DXR library has no entry point for this primary "
            "(a shader bundle from before RTXPT.2 R3).";
    }
    else if(!InitializeVisibilityPipelines(graphicsDevice, shaderLibrary, m_pipelines[index], library->bytecode, description))
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
    return PrepareLibrary(shaderLibrary, worker, PathTraceLibraryPath(primary, m_libraryVariant));
}

ShaderLibraryPreparation PathTraceReferencePass::PreparePackagedLibrary(
    ShaderLibrary& shaderLibrary,
    const std::filesystem::path& path,
    bool optional)
{
    const std::string libraryName = path.generic_string();
    ShaderLibraryPreparation preparation{};

    for(const CompiledLibrary& compiled : m_libraries)
    {
        if(compiled.path == path && !compiled.library.bytecode.empty())
        {
            preparation.state = ShaderLibraryPreparationState::Ready;
            return preparation;
        }
    }

    // Cooked bytes never change, so a failure is final for the session; it is reported once.
    const auto failed = m_packagedLibraryFailures.find(path);

    if(failed != m_packagedLibraryFailures.end())
    {
        preparation.state = ShaderLibraryPreparationState::Failed;
        preparation.diagnostic = failed->second;
        return preparation;
    }

    ShaderLibraryCompileResult library;
    std::string problem;

    if(!shaderLibrary.AcquirePackagedLibrary(path, library, problem))
    {
        preparation.state = ShaderLibraryPreparationState::Failed;
        preparation.diagnostic = "The " + libraryName + " DXR library " + problem + ".";
        m_packagedLibraryFailures.emplace(path, preparation.diagnostic);
        const bool missingTwin = optional && !shaderLibrary.HasPackagedLibrary(path);

        if(missingTwin)
        {
            ReportMissingPackagedTwin(path);
        }
        else
        {
            Log::Error(preparation.diagnostic);
        }

        return preparation;
    }

    m_libraries.push_back(CompiledLibrary{path, std::move(library)});
    m_libraries.back().revision = ++m_libraryRevisionCounter;
    preparation.state = ShaderLibraryPreparationState::Ready;
    return preparation;
}

void PathTraceReferencePass::ReportMissingPackagedTwin(const std::filesystem::path& path)
{
    // One report per twin, however many pipelines pair with it.
    if(m_reportedMissingTwins.insert(path.generic_string()).second)
    {
        Log::Warning(std::string("GI.6") + " RayQuery variant is not in this game's shaders; the "
            "original TraceRay pipeline is used: " + path.generic_string());
    }
}

ShaderLibraryPreparation PathTraceReferencePass::PrepareLibrary(
    ShaderLibrary& shaderLibrary,
    ShaderLibraryCompileWorker& worker,
    const std::filesystem::path& path)
{
    TransitionTiming::Scope er1Scope("PrepareLibrary", 0.5);

    // PS.5B.1: packaged games have no compiler; their cooked DXIL is the library, admitted without the worker.
    if(shaderLibrary.IsPackaged())
    {
        return PreparePackagedLibrary(shaderLibrary, path, RayQueryTwinOf(path));
    }
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

    if(completed)
    {
        DxilLibraryCache::Report(path, completed->library);
    }

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
        entry->revision = ++m_libraryRevisionCounter;
        entry->snapshot.reset();

        // A new revision may fix what made a pipeline fail. Pipelines already created keep their state
        // objects, as they do under synchronous reuse.
        for(size_t index = 0; index < PrimaryCount; ++index)
        {
            const PathTracePrimary builtPrimary = static_cast<PathTracePrimary>(index);

            if(PathTraceLibraryPath(builtPrimary, m_libraryVariant) == path)
            {
                m_pipelineFailed[index] = false;
                m_pipelineFailure[index].clear();
            }
        }

        // The ReSTIR DI split pipelines come from both libraries, so either may fix them.
        m_splitPipelinesFailed = false;
        m_splitPipelineFailure.clear();

        // RTXPT D.1B.2: the extraction pipelines come from the same libraries.
        m_extractRasterFailed = false;
        m_extractRasterFailure.clear();
        m_extractSplitFailed = false;
        m_extractSplitFailure.clear();
        m_extractRasterRawFailed = false;
        m_extractRasterRawFailure.clear();
        m_extractSplitRawFailed = false;
        m_extractSplitRawFailure.clear();

        // SHARC.4D: the learning libraries include the reference library (NVIDIA SHARC's too).
        for(auto& set : m_radianceCacheSets)
        {
            for(RadianceCacheLearningPipeline& learning : set)
            {
                learning.failed = false;
                learning.failure.clear();
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

std::vector<PathTraceLibraryRequirement> PathTraceReferencePass::GetRequiredLibraries(
    const PathTraceLibraryNeeds& needs) const
{
    std::vector<PathTraceLibraryRequirement> libraries;
    if(needs.psrChain != 0)
    {
        for(const auto& pipeline : GetRequiredPipelines(needs))
        {
            const auto& path = pipeline.description.shaderPath;
            if(std::none_of(libraries.begin(), libraries.end(), [&](const auto& entry) { return entry.path == path; }))
                libraries.push_back({path, pipeline.optional});
        }
        return libraries;
    }


    // Each pipeline library, followed by the RayQuery twin InitializeVisibilityPipelines pairs with it.
    const auto add = [&](const std::filesystem::path& path)
    {
        const auto listed = [&](const std::filesystem::path& candidate)
        {
            return std::any_of(libraries.begin(), libraries.end(),
                [&](const PathTraceLibraryRequirement& library)
                {
                    return library.path == candidate;
                });
        };

        if(!listed(path))
        {
            libraries.push_back(PathTraceLibraryRequirement{path, false});
        }

        const std::optional<std::filesystem::path> twinPath = RayQueryTwinLibraryPath(path);

        if(twinPath && !listed(*twinPath))
        {
            libraries.push_back(PathTraceLibraryRequirement{*twinPath, true});
        }
    };

    // Initialize creates the Camera pipeline for every primary; Raster Primary, the ReSTIR DI indirect pass and the
    // Raster Primary and split indirect extraction exports come from the same library.
    add(ReferenceLibraryPath(m_libraryVariant));

    // EnsureReSTIRDISplitPipelines creates the direct pass and, before the primary visibility selector is enabled,
    // the extraction direct pass (RayQuery 2M E3: their compute shaders in production).
    if(needs.primary == PathTracePrimary::RasterReSTIRDI)
    {
        add(DirectPassDescription(false, 0u).shaderPath);
        add(DirectPassDescription(true, 0u).shaderPath);
    }

    if(needs.rawDiffuseHitDistance)
    {
        add(RawDiffuseHitDistanceLibraryPath(m_libraryVariant));
    }

    if(needs.radianceCache)
    {
        const char* training = RadianceCacheLibraryPath(m_libraryVariant, PathTraceRadianceCacheLibrary::Training,
            needs.radianceCacheImplementation);
        const char* render = RadianceCacheLibraryPath(m_libraryVariant, needs.radianceCacheRender,
            needs.radianceCacheImplementation);

        if(training && render)
        {
            add(training);
            add(render);
        }
    }

    return libraries;
}

std::vector<PathTraceLibraryRequirement> PathTraceReferencePass::GetPackagedLibraries()
{
    // A pass that has created nothing selects SM66, the only variant packaged games use.
    const auto pass = std::make_unique<PathTraceReferencePass>();
    std::vector<PathTraceLibraryRequirement> libraries;

    // PS.5C: the ordinary path with every denoiser's extraction, and each PSR chain (single, two-bounce) with and
    // without extraction and the raw diffuse hit distance, as PSR prepares exactly the set its frame needs.
    // PS.5D: each of those with SHARC, whose ReSTIR DI render library follows the signal (Renderer::SharcRenderLibrary).
    // NVIDIA SHARC (the production cache) is cooked as compiled DXIL; its shader source is never packaged.
    constexpr PathTraceRadianceCacheLibrary sharcRender[] = {PathTraceRadianceCacheLibrary::RenderSplitIndirect,
        PathTraceRadianceCacheLibrary::RenderSplitIndirectExtraction,
        PathTraceRadianceCacheLibrary::RenderSplitIndirectExtractionRawDiffuseHitDistance};

    for(const std::uint32_t psrChain : {0u, 1u, 2u})
    for(const bool sharc : {false, true})
    {
        for(const int signal : {0, 1, 2})
        {
            PathTraceLibraryNeeds needs{};
            needs.primary = PathTracePrimary::RasterReSTIRDI;
            needs.psrChain = psrChain;
            needs.extraction = signal >= 1;
            needs.rawDiffuseHitDistance = signal == 2;
            needs.radianceCache = sharc;
            needs.radianceCacheRender = sharcRender[signal];
            needs.radianceCacheImplementation = PathTraceRadianceCacheImplementation::Nvidia;

            for(PathTraceLibraryRequirement& library : pass->GetRequiredLibraries(needs))
            {
                if(std::none_of(libraries.begin(), libraries.end(),
                    [&](const PathTraceLibraryRequirement& listed) { return listed.path == library.path; }))
                {
                    libraries.push_back(std::move(library));
                }
            }
        }
    }

    return libraries;
}

std::vector<PathTraceReferencePass::PipelineRequirement> PathTraceReferencePass::GetRequiredPipelines(
    const PathTraceLibraryNeeds& needs) const
{
    std::vector<PipelineRequirement> pipelines;

    // Each pipeline, followed by the RayQuery twin InitializeVisibilityPipelines pairs with it.
    const auto add = [&](const RayTracingPipelineDescription& description)
    {
        pipelines.push_back(PipelineRequirement{description, false});
        const std::optional<std::filesystem::path> twinPath = RayQueryTwinLibraryPath(description.shaderPath);

        if(twinPath)
        {
            RayTracingPipelineDescription twin = description;
            twin.shaderPath = *twinPath;
            pipelines.push_back(PipelineRequirement{std::move(twin), true});
        }
    };
    if(needs.psrChain != 0)
    {
        const auto chainPath = [&](const std::filesystem::path& path)
        {
            return needs.psrChain >= 2u ?
                (m_psrChainDirectoryForTesting.empty() ? std::filesystem::path("PsrK2") :
                    m_psrChainDirectoryForTesting) / path : path;
        };
        const auto lighting = [&](bool direct, bool extraction, bool raw)
        {
            // RayQuery 2M E3: the direct pass's own description (its compute shader in production).
            if(direct)
            {
                add(DirectPassDescription(extraction, needs.psrChain));
                return;
            }

            auto description = extraction ? BuildReSTIRDISplitExtractionPipelineDescription(direct, m_libraryVariant) :
                BuildReSTIRDISplitPipelineDescription(direct, m_libraryVariant);
            description.shaderPath = chainPath(extraction ?
                PrimarySurfaceReplacementGBufferExtractionLibraryPath(raw, m_libraryVariant) :
                PrimarySurfaceReplacementGBufferLibraryPath(m_libraryVariant));
            description.rootParameters.push_back({RootParameterType::ConstantBuffer, 4});
            description.rootParameters.push_back({RootParameterType::ShaderResource, 2});
            description.rasterMaterialSampler = true;
            add(description);
        };
        lighting(true, false, false);
        lighting(false, false, false);
        if(needs.extraction)
        {
            lighting(true, true, false);
            lighting(false, true, false);
            if(needs.rawDiffuseHitDistance) lighting(false, true, true);
        }
        if(needs.radianceCache)
        {
            for(auto library : {PathTraceRadianceCacheLibrary::PsrTraining,
                PsrRenderLibrary(needs.extraction, needs.rawDiffuseHitDistance)})
            {
                if(!RadianceCacheLibraryPath(m_libraryVariant, library, needs.radianceCacheImplementation))
                {
                    continue;
                }

                auto description = BuildRadianceCachePipelineDescription(m_libraryVariant, library,
                    needs.radianceCacheImplementation);
                description.shaderPath = chainPath(description.shaderPath);
                add(description);
            }
        }
        return pipelines;
    }

    const bool split = needs.primary == PathTracePrimary::RasterReSTIRDI;

    // Initialize: the Camera pipeline, for every primary.
    add(BuildPathTracePipelineDescription(PathTracePrimary::Camera, m_libraryVariant));

    // EnsurePrimaryPipeline: a single-pipeline primary, or the ReSTIR DI split pair and its extraction direct pass.
    if(split)
    {
        // ReSTIR DI evaluates the Raster primary prerequisite before its own split pair.
        add(BuildPathTracePipelineDescription(PathTracePrimary::Raster, m_libraryVariant));
        add(DirectPassDescription(false, 0u));
        add(BuildReSTIRDISplitPipelineDescription(false, m_libraryVariant));
        add(DirectPassDescription(true, 0u));
    }
    else if(needs.primary != PathTracePrimary::Camera)
    {
        add(BuildPathTracePipelineDescription(needs.primary, m_libraryVariant));
    }

    // EnsureSignalExtraction: the production extraction pipelines, and the raw-distance ones (their own library).
    if(needs.extraction)
    {
        add(split ? BuildReSTIRDISplitExtractionPipelineDescription(false, m_libraryVariant) :
            BuildRasterExtractionPipelineDescription(m_libraryVariant));
    }

    if(needs.extraction && needs.rawDiffuseHitDistance)
    {
        RayTracingPipelineDescription raw = split ?
            BuildReSTIRDISplitExtractionPipelineDescription(false, m_libraryVariant) :
            BuildRasterExtractionPipelineDescription(m_libraryVariant);
        raw.shaderPath = RawDiffuseHitDistanceLibraryPath(m_libraryVariant);
        add(raw);
    }

    // EnsureRadianceCacheLearning: SHARC's sparse training and render pipelines.
    if(needs.radianceCache && RadianceCacheLibraryPath(m_libraryVariant, needs.radianceCacheRender,
        needs.radianceCacheImplementation))
    {
        add(BuildRadianceCachePipelineDescription(m_libraryVariant, PathTraceRadianceCacheLibrary::Training,
            needs.radianceCacheImplementation));
        add(BuildRadianceCachePipelineDescription(m_libraryVariant, needs.radianceCacheRender,
            needs.radianceCacheImplementation));
    }

    return pipelines;
}

std::uint64_t PathTraceReferencePass::LibraryRevision(const std::filesystem::path& path) const
{
    for(const CompiledLibrary& compiled : m_libraries)
    {
        if(compiled.path == path)
        {
            return compiled.revision;
        }
    }

    return 0;
}

bool PathTraceReferencePass::IsPipelineLive(const std::string& identity) const
{
    const auto live = [&](const RayTracingPipeline& pipeline)
    {
        return pipeline.IsInitialized() && pipeline.GetIdentity() == identity;
    };

    if(std::any_of(std::begin(m_pipelines), std::end(m_pipelines), live) || live(m_splitDirectPipeline) ||
        live(m_splitIndirectPipeline) || live(m_extractRasterPipeline) || live(m_extractSplitDirectPipeline) ||
        live(m_extractSplitIndirectPipeline) || live(m_extractRasterRawPipeline) || live(m_extractSplitIndirectRawPipeline))
    {
        return true;
    }

    for(const auto& set : m_radianceCacheSets)
    {
        for(const RadianceCacheLearningPipeline& learning : set)
        {
            if(live(learning.pipeline))
            {
                return true;
            }
        }
    }

    if(m_retainedPsrPipelines.contains(identity)) return true;
    for(const auto* pipeline : {&m_psrGBufferDirectPipeline, &m_psrGBufferIndirectPipeline,
        &m_psrGBufferExtractDirectPipeline, &m_psrGBufferExtractIndirectPipeline, &m_psrGBufferExtractIndirectRawPipeline})
        if(live(*pipeline)) return true;

    for(const auto* alternates : {&m_rayQueryPipelines, &m_psrRayQueryPipelines})
    {
        for(const auto& [original, alternate] : *alternates)
        {
            if(alternate && live(*alternate))
            {
                return true;
            }
        }
    }

    return false;
}

std::string PathTraceReferencePass::PipelineKey(const RayTracingPipelineDescription& description)
{
    return PipelineDescriptionKey(description);
}

ShaderLibraryPreparationState PathTraceReferencePass::ObserveLibrary(const ShaderLibraryCompileWorker* worker, bool packaged,
    const std::filesystem::path& path, std::string& detail) const
{
    for(const CompiledLibrary& compiled : m_libraries)
    {
        if(compiled.path == path && !compiled.library.bytecode.empty())
        {
            return ShaderLibraryPreparationState::Ready;
        }
    }

    const bool failed = std::any_of(m_failedLibraries.begin(), m_failedLibraries.end(), [&](const FailedLibrary& library)
    {
        return library.path == path;
    });

    if(failed || m_packagedLibraryFailures.contains(path))
    {
        detail = path.filename().string() + " failed";
        return ShaderLibraryPreparationState::Failed;
    }

    if(packaged || !worker)
    {
        detail = path.filename().string() + (packaged ? " not admitted yet" : " compiled on first use (no worker)");
        return ShaderLibraryPreparationState::Queued;
    }

    switch(worker->GetState(path))
    {
    case ShaderLibraryCompileState::Queued:
        detail = path.filename().string() + " queued";
        return ShaderLibraryPreparationState::Queued;
    case ShaderLibraryCompileState::Compiling:
        detail = path.filename().string() + " compiling";
        return ShaderLibraryPreparationState::Compiling;
    case ShaderLibraryCompileState::Completed:
        detail = path.filename().string() + " compiled, not accepted yet";
        return ShaderLibraryPreparationState::Compiling;
    case ShaderLibraryCompileState::None:
        break;
    }

    detail = path.filename().string() + " not requested yet";
    return ShaderLibraryPreparationState::Queued;
}

ShaderLibraryPreparationState PathTraceReferencePass::ObservePipeline(const RayTracingPipelineWorker* worker, ID3D12Device5* device,
    const PipelineRequirement& requirement, std::string& detail) const
{
    const RayTracingPipelineDescription& description = requirement.description;
    const CompiledLibrary* library = nullptr;

    for(const CompiledLibrary& compiled : m_libraries)
    {
        if(compiled.path == description.shaderPath)
        {
            library = &compiled;
            break;
        }
    }

    const std::string name = description.shaderPath.filename().string();

    if(!library || library->library.bytecode.empty())
    {
        detail = name + " library not accepted";
        return requirement.optional ? ShaderLibraryPreparationState::Ready : ShaderLibraryPreparationState::Queued;
    }

    const std::string key = PipelineDescriptionKey(description);

    if(IsPipelineLive(key + "#" + std::to_string(library->revision)))
    {
        return ShaderLibraryPreparationState::Ready;
    }

    const auto prepared = m_preparedPipelines.find(key);

    if(prepared != m_preparedPipelines.end() && prepared->second.revision == library->revision && prepared->second.device == device)
    {
        return ShaderLibraryPreparationState::Ready;
    }

    const auto failed = m_failedPipelines.find(key);

    if(failed != m_failedPipelines.end() && failed->second == library->revision)
    {
        detail = name + " pipeline failed";
        return requirement.optional ? ShaderLibraryPreparationState::Ready : ShaderLibraryPreparationState::Failed;
    }

    if(!worker)
    {
        detail = name + " pipeline created on first use (no worker)";
        return ShaderLibraryPreparationState::PreparingPipelines;
    }

    switch(worker->GetState(key + "#" + std::to_string(library->revision)))
    {
    case RayTracingPipelineJobState::Queued:
    case RayTracingPipelineJobState::Creating:
        detail = name + " pipeline creating";
        return ShaderLibraryPreparationState::PreparingPipelines;
    case RayTracingPipelineJobState::Completed:
        detail = name + " pipeline created, not adopted yet";
        return ShaderLibraryPreparationState::PreparingPipelines;
    case RayTracingPipelineJobState::None:
        break;
    }

    detail = name + " pipeline not requested yet";
    return ShaderLibraryPreparationState::PreparingPipelines;
}

ShaderLibraryPreparation PathTraceReferencePass::PreparePipelines(RayTracingPipelineWorker& worker, ID3D12Device5* device,
    const PathTraceLibraryNeeds& needs)
{
    TransitionTiming::Scope er1Scope("PreparePipelines", 0.5);
    ShaderLibraryPreparation combined{};
    combined.state = ShaderLibraryPreparationState::Ready;

    const auto worse = [&](ShaderLibraryPreparationState state, const std::string& diagnostic)
    {
        if(combined.state == ShaderLibraryPreparationState::Failed)
        {
            return;
        }

        if(state == ShaderLibraryPreparationState::Failed || combined.state == ShaderLibraryPreparationState::Ready)
        {
            combined.state = state;
            combined.diagnostic = diagnostic;
        }
    };

    for(const PipelineRequirement& requirement : GetRequiredPipelines(needs))
    {
        const RayTracingPipelineDescription& description = requirement.description;
        const std::string libraryName = description.shaderPath.generic_string();
        CompiledLibrary* library = nullptr;

        for(CompiledLibrary& compiled : m_libraries)
        {
            if(compiled.path == description.shaderPath)
            {
                library = &compiled;
                break;
            }
        }

        // A twin whose library failed keeps its pipeline on TraceRay, as InitializeVisibilityPipelines does.
        if(!library || library->library.bytecode.empty())
        {
            if(!requirement.optional)
            {
                worse(ShaderLibraryPreparationState::Failed, "The " + libraryName + " DXR library is unavailable.");
            }

            continue;
        }

        const std::string key = PipelineDescriptionKey(description);
        const std::string identity = key + "#" + std::to_string(library->revision);

        if(IsPipelineLive(identity))
        {
            continue;
        }

        const auto prepared = m_preparedPipelines.find(key);

        if(prepared != m_preparedPipelines.end() && prepared->second.revision == library->revision &&
            prepared->second.device == device)
        {
            continue;
        }

        const auto failed = m_failedPipelines.find(key);

        if(failed != m_failedPipelines.end() && failed->second == library->revision)
        {
            if(!requirement.optional)
            {
                worse(ShaderLibraryPreparationState::Failed, "The " + libraryName + " DXR pipeline could not be created.");
            }

            continue;
        }

        switch(worker.GetState(identity))
        {
        case RayTracingPipelineJobState::Queued:
        case RayTracingPipelineJobState::Creating:
            worse(ShaderLibraryPreparationState::PreparingPipelines, "");
            continue;
        case RayTracingPipelineJobState::Completed:
        {
            std::optional<RayTracingPipelineOutcome> outcome = worker.TakeCompleted(identity);

            // The identity carries the revision; the device must be this one too.
            if(outcome && outcome->succeeded && outcome->device == device)
            {
                m_preparedPipelines[key] = PreparedPipeline{library->revision, device, std::move(outcome->pipeline)};
                continue;
            }

            if(outcome && outcome->device != device)
            {
                ++m_pipelineStatistics.discarded;
                break;
            }

            ++m_pipelineStatistics.failures;
            m_failedPipelines[key] = library->revision;
            Log::Error("Background creation of the " + libraryName + " DXR pipeline failed" +
                (requirement.optional ? " (its TraceRay pipeline is kept)." : "."));

            if(!requirement.optional)
            {
                worse(ShaderLibraryPreparationState::Failed, "The " + libraryName + " DXR pipeline could not be created.");
            }

            continue;
        }
        case RayTracingPipelineJobState::None:
            break;
        }

        if(!library->snapshot)
        {
            library->snapshot = std::make_shared<const std::vector<char>>(library->library.bytecode);
        }

        RayTracingPipelineRequest request{};
        request.key = identity;
        request.device = device;
        request.library = library->snapshot;
        request.description = description;
        request.failForTesting = !m_pipelineFailureForTesting.empty() &&
            key.find(m_pipelineFailureForTesting) != std::string::npos;

        if(!worker.Submit(std::move(request)))
        {
            worse(ShaderLibraryPreparationState::Failed, "The " + libraryName + " DXR pipeline could not be queued.");
            continue;
        }

        ++m_pipelineStatistics.submitted;
        worse(ShaderLibraryPreparationState::PreparingPipelines, "");
    }

    return combined;
}

bool PathTraceReferencePass::CreatePipeline(GraphicsDevice& device, RayTracingPipeline& slot,
    const std::vector<char>& bytecode, const RayTracingPipelineDescription& description)
{
    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);

    const std::string key = PipelineDescriptionKey(description);
    const std::uint64_t revision = LibraryRevision(description.shaderPath);
    const std::string identity = key + "#" + std::to_string(revision);
    ID3D12Device5* const rayTracingDevice = device.GetRayTracingDevice();
    const auto retained = m_retainedPsrPipelines.find(identity);
    if(retained != m_retainedPsrPipelines.end())
    {
        slot.Swap(*retained->second);
        m_retainedPsrPipelines.erase(retained);
        ++m_pipelineStatistics.adopted;
        return true;
    }

    const auto prepared = m_preparedPipelines.find(key);

    if(prepared != m_preparedPipelines.end())
    {
        PreparedPipeline candidate = std::move(prepared->second);
        m_preparedPipelines.erase(prepared);

        // Adopted only at this library revision on this device; anything else is discarded (it was never used).
        if(candidate.revision == revision && candidate.device == rayTracingDevice && candidate.pipeline)
        {
            slot.Shutdown();
            slot.Swap(*candidate.pipeline);
            slot.SetIdentity(identity);
            ++m_pipelineStatistics.adopted;
            return true;
        }

        ++m_pipelineStatistics.discarded;
    }

    const auto failed = m_failedPipelines.find(key);

    if(failed != m_failedPipelines.end() && failed->second == revision)
    {
        return false;
    }

    ++m_pipelineStatistics.synchronous;

    if(!slot.Initialize(rayTracingDevice, bytecode, description))
    {
        return false;
    }

    slot.SetIdentity(identity);
    return true;
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

    // ER.1 Phase 2: a failure the worker reported for the current sources would fail again; it is not recompiled
    // here (PrepareLibrary retries it once the sources change).
    const bool failedCurrent = std::any_of(m_failedLibraries.begin(), m_failedLibraries.end(),
        [&](const FailedLibrary& failedLibrary)
        {
            return failedLibrary.path == path && ShaderLibraryCompileWorker::IsCurrent(failedLibrary.outcome);
        });

    if(failedCurrent)
    {
        return nullptr;
    }

    if(!entry)
    {
        entry = &m_libraries.emplace_back(CompiledLibrary{path, {}});
    }

    TransitionTiming::Scope er1Scope("Main-thread CompileLibrary " + path.generic_string(), 0.1,
        TransitionTiming::TransitionsAndStartup);
    if(!shaderLibrary.CompileLibrary(path, entry->library))
    {
        entry->library = {};
        entry->revision = ++m_libraryRevisionCounter;
        entry->snapshot.reset();
        return nullptr;
    }

    entry->revision = ++m_libraryRevisionCounter;
    entry->snapshot.reset();
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

    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);

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

void PathTraceReferencePass::ReleaseTextures()
{
    m_accumulationTexture.Shutdown();
    m_outputTexture.Shutdown();

    // Sized for the old extent; the next split frame, or extraction frame, allocates them again.
    ReleaseSplitDirectRadiance();
    m_diffuseTexture.Shutdown();
    m_specularTexture.Shutdown();
    m_historyPriorTexture.Shutdown();
    m_rawDiffuseHitDistanceTexture.Shutdown();
    // PSR.1a: sized for the old extent; the next replacement frame allocates it again (PSR.1b: and the guides).
    m_psrRecords.Shutdown();
    m_psrGuides.Shutdown();
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

    ReleaseTextures();

    if(!CreateTextures(device, descriptorHeap, width, height))
    {
        Shutdown();
        return false;
    }

    return true;
}

bool PathTraceReferencePass::Execute(
    PathTracePrimary primary,
    ID3D12GraphicsCommandList4* commandList,
    DescriptorHeap& descriptorHeap,
    const RayTracingSceneResources& rayTracingScene,
    D3D12_GPU_VIRTUAL_ADDRESS constantsAddress,
    D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstantsAddress,
    unsigned int width,
    unsigned int height,
    bool extract,
    D3D12_GPU_VIRTUAL_ADDRESS radianceCacheConstants,
    PathTraceRadianceCacheLibrary radianceCacheLibrary,
    bool rawDiffuseHitDistance,
    bool primarySurfaceReplacement,
    GpuTimestampProfiler* profiler,
    unsigned int frameIndex,
    D3D12_GPU_VIRTUAL_ADDRESS guideConstants,
    D3D12_GPU_VIRTUAL_ADDRESS guideTransforms)
{
    m_rawDiffuseHitDistanceWritten = false;
    m_splitDirectDiffuseWritten = false;
    m_psrWritten = false;
    m_psrGuidesWritten = false;

    // NRD.RELAX.2B: raw-distance extraction only with its output; otherwise the legacy extraction, which writes none.
#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
    m_sharcDiagnosticWritten = false;
#endif
    const bool raw = extract && rawDiffuseHitDistance && m_rawDiffuseHitDistanceTexture.GetResource();

    // SHARC.4D: a learning pipeline only for Raster Primary; otherwise the production one.
    RadianceCacheLearningPipeline& radianceCache = m_radianceCache[static_cast<size_t>(radianceCacheLibrary)];
    // SHARC.4E.1: an extraction frame uses a cache library only through its extraction ray generation, and
    // (NRD.RELAX.2B) only the build matching the frame's raw-distance request.
    const PathTraceRadianceCacheLibrary extractionLibrary = raw ?
        PathTraceRadianceCacheLibrary::RenderExtractionRawDiffuseHitDistance : PathTraceRadianceCacheLibrary::RenderExtraction;
    const bool extractingLibrary = radianceCacheLibrary == PathTraceRadianceCacheLibrary::RenderExtraction ||
        radianceCacheLibrary == PathTraceRadianceCacheLibrary::RenderExtractionRawDiffuseHitDistance;
    const bool learning = radianceCacheConstants != 0 && primary == PathTracePrimary::Raster &&
        extract == extractingLibrary && (!extract || radianceCacheLibrary == extractionLibrary) && radianceCache.ready &&
        (!extract || m_diffuseTexture.GetResource());
    const bool rawPipeline = raw && !learning && m_extractRasterRawReady;
    // PSR.1a: Raster Primary without extraction or a cache library only; otherwise the frame records as without it.
    // PSR.1a-P: with GI.6 selected the PSR pipelines need their own RayQuery twins, or PSR would render with TraceRay
    // secondary shadows where the production frame uses RayQuery; without them the frame records as without PSR.
    const bool replacementVisibility = !m_secondaryShadowRayQuery || (m_psrRayQueryPipelines[&m_psrRecordPipeline] &&
        m_psrRayQueryPipelines[&m_psrRasterPipeline]);
    const bool replacement = primarySurfaceReplacement && primary == PathTracePrimary::Raster && !extract && !learning &&
        radianceCacheConstants == 0 && m_psrPipelinesReady && replacementVisibility && m_psrRecords.GetResource() &&
        m_psrRecords.GetResource()->GetDesc().Width >=
        static_cast<std::uint64_t>(width) * height * PrimarySurfaceReplacementRecordStride;
    RayTracingPipeline& pipeline = VisibilityPipeline(learning ? radianceCache.pipeline :
        (extract ? (rawPipeline ? m_extractRasterRawPipeline : m_extractRasterPipeline) :
        (replacement ? m_psrRasterPipeline : m_pipelines[static_cast<size_t>(primary)])));
    const bool ready = learning || (extract ?
        primary == PathTracePrimary::Raster && (rawPipeline || m_extractRasterReady) && m_diffuseTexture.GetResource() :
        m_pipelineReady[static_cast<size_t>(primary)]);

    // ReSTIR DI records through ExecuteReSTIRDISplit; its own pipeline slot is never created.
    if(!m_initialized || !commandList || width != m_width || height != m_height ||
        primary == PathTracePrimary::RasterReSTIRDI || !ready)
    {
        return false;
    }

    m_accumulationTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_outputTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_statisticsTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if(extract)
    {
        m_diffuseTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_specularTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    const auto bindRoots = [&]()
    {
        commandList->SetComputeRootShaderResourceView(0, rayTracingScene.GetTopLevel().GetGPUAddress());
        commandList->SetComputeRootDescriptorTable(1, descriptorHeap.GetGPUHandle(m_accumulationTexture.GetUAVIndex()));
        commandList->SetComputeRootDescriptorTable(2, descriptorHeap.GetGPUHandle(m_outputTexture.GetUAVIndex()));
        commandList->SetComputeRootDescriptorTable(3, descriptorHeap.GetGPUHandle(m_statisticsTexture.GetUAVIndex()));
        commandList->SetComputeRootConstantBufferView(4, constantsAddress);
        commandList->SetComputeRootConstantBufferView(5, sceneLightConstantsAddress);
    };

    // PSR.1a: every pixel's record, ordered before the raster primary reads it. The record pass writes no image.
    if(replacement)
    {
        m_psrRecords.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if(profiler)
        {
            profiler->BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::PathTracePsrRecord);
        }

        // PSR.1b: the guide pass writes the same records, plus the guides.
        const bool guides = guideConstants != 0 && guideTransforms != 0 && m_psrGuidePipelineReady &&
            m_psrGuides.GetResource() &&
            m_psrGuides.GetResource()->GetDesc().Width >= static_cast<std::uint64_t>(width) * height *
            PrimarySurfaceReplacementGuideStride;
        RayTracingPipeline& recordPipeline = VisibilityPipeline(guides ? m_psrGuidePipeline : m_psrRecordPipeline);

        if(guides)
        {
            m_psrGuides.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        recordPipeline.Bind(commandList);
        bindRoots();

        if(guides)
        {
            commandList->SetComputeRootConstantBufferView(6, guideConstants);
            commandList->SetComputeRootShaderResourceView(7, guideTransforms);
        }

        recordPipeline.Dispatch(commandList, width, height);

        D3D12_RESOURCE_BARRIER recordBarriers[2]{};
        recordBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        recordBarriers[0].UAV.pResource = m_psrRecords.GetResource();
        recordBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        recordBarriers[1].UAV.pResource = m_psrGuides.GetResource();
        commandList->ResourceBarrier(guides ? 2 : 1, recordBarriers);
        m_psrGuidesWritten = guides;

        if(profiler)
        {
            profiler->EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::PathTracePsrRecord);
            profiler->BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::PathTracePsrRaster);
        }
    }

    pipeline.Bind(commandList);
    bindRoots();

    if(learning)
    {
        commandList->SetComputeRootConstantBufferView(6, radianceCacheConstants);
    }

    pipeline.Dispatch(commandList, width, height);
#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
    m_sharcDiagnosticWritten = learning && (radianceCacheLibrary == PathTraceRadianceCacheLibrary::Render ||
        radianceCacheLibrary == PathTraceRadianceCacheLibrary::RenderExtraction ||
        radianceCacheLibrary == PathTraceRadianceCacheLibrary::RenderExtractionRawDiffuseHitDistance);
#endif
    m_rawDiffuseHitDistanceWritten = learning ? IsRawDiffuseHitDistanceLibrary(radianceCacheLibrary) : rawPipeline;
    m_psrWritten = replacement;

    if(replacement && profiler)
    {
        profiler->EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::PathTracePsrRaster);
    }

    RecordOutputBarriers(commandList, extract);
    return true;
}

unsigned int PathTraceReferencePass::EnsurePrimarySurfaceReplacement(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    DescriptorHeap& descriptorHeap,
    std::string& failureReason)
{
    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);

    if(!m_initialized)
    {
        failureReason = "The path tracer is not initialized.";
        return UINT_MAX;
    }

    if(m_psrPipelinesFailed)
    {
        failureReason = m_psrFailure;
        return UINT_MAX;
    }

    if(!m_psrPipelinesReady)
    {
        RayTracingPipeline* pipelines[2] = {&m_psrRecordPipeline, &m_psrRasterPipeline};

        for(int pass = 0; pass < 2 && m_psrFailure.empty(); ++pass)
        {
            const RayTracingPipelineDescription description =
                BuildPrimarySurfaceReplacementPipelineDescription(pass == 0, m_libraryVariant);
            const std::string libraryName = description.shaderPath.generic_string();
            const ShaderLibraryCompileResult* library = AcquireLibrary(shaderLibrary, description.shaderPath);

            if(!library)
            {
                m_psrFailure = "The " + libraryName + " DXR library is unavailable or failed to compile" +
                    TroubleshootingHint() + ".";
            }
            else if(!LibraryHasEntryPoint(library->bytecode, description))
            {
                m_psrFailure = "The " + libraryName + " DXR library has no primary surface replacement entry point.";
            }
            else if(!InitializeVisibilityPipelines(graphicsDevice, shaderLibrary, *pipelines[pass], library->bytecode,
                description))
            {
                m_psrFailure = "The " + libraryName + " primary surface replacement DXR pipeline could not be created" +
                    TroubleshootingHint() + ".";
            }

            // PSR.1a-P: the GI.6 twin moves out of the shared readiness set, so an optional PSR library that fails to
            // build a twin cannot disable GI.6 for the production pipelines.
            const auto twin = m_rayQueryPipelines.find(pipelines[pass]);

            if(twin != m_rayQueryPipelines.end())
            {
                m_psrRayQueryPipelines[pipelines[pass]] = std::move(twin->second);
                m_rayQueryPipelines.erase(twin);
            }
        }

        if(!m_psrFailure.empty())
        {
            ReleasePrimarySurfaceReplacementPipelines();
            m_psrPipelinesFailed = true;
            failureReason = m_psrFailure;
            return UINT_MAX;
        }

        m_psrPipelinesReady = true;
    }

    if(!m_psrRecords.GetResource())
    {
        const std::uint64_t bytes = static_cast<std::uint64_t>(m_width) * m_height * PrimarySurfaceReplacementRecordStride;

        if(bytes > UINT_MAX || !m_psrRecords.InitializeGPUWritable(graphicsDevice.GetDevice(), static_cast<size_t>(bytes)) ||
            !m_psrRecords.CreateRawUAV(graphicsDevice.GetDevice(), descriptorHeap))
        {
            m_psrRecords.Shutdown();
            failureReason = "The primary surface replacement records (" + std::to_string(bytes) +
                " bytes) could not be allocated.";
            return UINT_MAX;
        }

        m_psrRecords.GetResource()->SetName(L"Path Trace PSR Records");
    }

    return m_psrRecords.GetUAVDescriptorIndex();
}

unsigned int PathTraceReferencePass::EnsurePrimarySurfaceReplacementGuides(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    DescriptorHeap& descriptorHeap,
    std::string& failureReason)
{
    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);

    if(!m_initialized)
    {
        failureReason = "The path tracer is not initialized.";
        return UINT_MAX;
    }

    if(m_psrGuidePipelineFailed)
    {
        failureReason = m_psrGuideFailure;
        return UINT_MAX;
    }

    if(!m_psrGuidePipelineReady)
    {
        const RayTracingPipelineDescription description =
            BuildPrimarySurfaceReplacementGuidePipelineDescription(m_libraryVariant);
        const std::string libraryName = description.shaderPath.generic_string();
        const ShaderLibraryCompileResult* library = AcquireLibrary(shaderLibrary, description.shaderPath);

        if(!library)
        {
            m_psrGuideFailure = "The " + libraryName + " DXR library is unavailable or failed to compile" +
                TroubleshootingHint() + ".";
        }
        else if(!LibraryHasEntryPoint(library->bytecode, description))
        {
            m_psrGuideFailure = "The " + libraryName + " DXR library has no primary surface replacement guide entry point.";
        }
        else if(!InitializeVisibilityPipelines(graphicsDevice, shaderLibrary, m_psrGuidePipeline, library->bytecode,
            description))
        {
            m_psrGuideFailure = "The " + libraryName + " primary surface replacement guide DXR pipeline could not be created" +
                TroubleshootingHint() + ".";
        }

        // As the PSR pipelines: the twin stays outside the shared GI.6 readiness set.
        const auto twin = m_rayQueryPipelines.find(&m_psrGuidePipeline);

        if(twin != m_rayQueryPipelines.end())
        {
            m_psrRayQueryPipelines[&m_psrGuidePipeline] = std::move(twin->second);
            m_rayQueryPipelines.erase(twin);
        }

        if(!m_psrGuideFailure.empty())
        {
            m_psrGuidePipeline.Shutdown();
            m_psrRayQueryPipelines.erase(&m_psrGuidePipeline);
            m_psrGuidePipelineFailed = true;
            failureReason = m_psrGuideFailure;
            return UINT_MAX;
        }

        m_psrGuidePipelineReady = true;
    }

    if(!m_psrGuides.GetResource())
    {
        const std::uint64_t bytes = static_cast<std::uint64_t>(m_width) * m_height * PrimarySurfaceReplacementGuideStride;

        if(bytes > UINT_MAX || !m_psrGuides.InitializeGPUWritable(graphicsDevice.GetDevice(), static_cast<size_t>(bytes)) ||
            !m_psrGuides.CreateRawUAV(graphicsDevice.GetDevice(), descriptorHeap))
        {
            m_psrGuides.Shutdown();
            failureReason = "The primary surface replacement guides (" + std::to_string(bytes) +
                " bytes) could not be allocated.";
            return UINT_MAX;
        }

        m_psrGuides.GetResource()->SetName(L"Path Trace PSR Guides");
    }

    return m_psrGuides.GetUAVDescriptorIndex();
}

bool PathTraceReferencePass::EnsurePrimarySurfaceReplacementGBuffer(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    std::string& failureReason)
{
    if(!m_initialized)
    {
        failureReason = "The path tracer is not initialized.";
        return false;
    }

    if(m_psrGBufferPipelinesFailed)
    {
        failureReason = m_psrGBufferFailure;
        return false;
    }

    if(m_psrGBufferPipelinesReady)
    {
        return true;
    }

    RayTracingPipeline* pipelines[2] = {&m_psrGBufferDirectPipeline, &m_psrGBufferIndirectPipeline};

    for(int pass = 0; pass < 2 && m_psrGBufferFailure.empty(); ++pass)
    {
        const bool directPass = pass == 0;
        RayTracingPipelineDescription description = BuildReSTIRDISplitPipelineDescription(directPass, m_libraryVariant);
        description.shaderPath = PsrChainLibraryPath(PrimarySurfaceReplacementGBufferLibraryPath(m_libraryVariant));
        // After the split pair's own parameters: b4 (the replacement pass's camera and identity) and t2 (its transforms).
        description.rootParameters.push_back({RootParameterType::ConstantBuffer, 4});
        description.rootParameters.push_back({RootParameterType::ShaderResource, 2});
        description.rasterMaterialSampler = true;

        // RayQuery 2M E3: the direct pass's own description (its compute shader in production).
        if(directPass)
        {
            description = DirectPassDescription(false, m_psrChain);
        }

        const std::string libraryName = description.shaderPath.generic_string();
        const ShaderLibraryCompileResult* library = AcquireLibrary(shaderLibrary, description.shaderPath);

        if(!library)
        {
            m_psrGBufferFailure = "The " + libraryName + " DXR library is unavailable or failed to compile" +
                TroubleshootingHint() + ".";
        }
        else if(!LibraryHasEntryPoint(library->bytecode, description))
        {
            m_psrGBufferFailure = "The " + libraryName + " DXR library has no split ReSTIR DI entry point.";
        }
        else if(!InitializeVisibilityPipelines(graphicsDevice, shaderLibrary, *pipelines[pass], library->bytecode,
            description))
        {
            m_psrGBufferFailure = "The " + libraryName + " PSR.2 lighting pipeline could not be created" +
                TroubleshootingHint() + ".";
        }

        // GI.6 twins leave the shared readiness set (the compute direct pass has none).
        const auto twin = m_rayQueryPipelines.find(pipelines[pass]);

        if(twin != m_rayQueryPipelines.end())
        {
            m_psrRayQueryPipelines[pipelines[pass]] = std::move(twin->second);
            m_rayQueryPipelines.erase(twin);
        }
    }

    if(!m_psrGBufferFailure.empty())
    {
        for(RayTracingPipeline* pipeline : pipelines)
        {
            pipeline->Shutdown();
            m_psrRayQueryPipelines.erase(pipeline);
        }

        m_psrGBufferPipelinesFailed = true;
        failureReason = m_psrGBufferFailure;
        return false;
    }

    m_psrGBufferPipelinesReady = true;
    return true;
}

bool PathTraceReferencePass::EnsurePrimarySurfaceReplacementGBufferExtraction(GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary, bool rawDiffuseHitDistance, std::string& failureReason)
{
    const int kind = rawDiffuseHitDistance ? 1 : 0;

    if(!m_initialized)
    {
        failureReason = "The path tracer is not initialized.";
        return false;
    }

    if(m_psrGBufferExtractFailed[kind])
    {
        failureReason = m_psrGBufferExtractFailure[kind];
        return false;
    }

    if(m_psrGBufferExtractReady[kind])
    {
        return true;
    }

    // The direct pass is shared by both kinds (as the production extraction direct pass is).
    std::vector<std::pair<RayTracingPipeline*, bool>> pipelines;

    if(!m_psrGBufferExtractDirectPipeline.IsInitialized())
    {
        pipelines.emplace_back(&m_psrGBufferExtractDirectPipeline, true);
    }

    pipelines.emplace_back(rawDiffuseHitDistance ? &m_psrGBufferExtractIndirectRawPipeline :
        &m_psrGBufferExtractIndirectPipeline, false);
    std::string& failure = m_psrGBufferExtractFailure[kind];

    for(const auto& [pipeline, directPass] : pipelines)
    {
        RayTracingPipelineDescription description = BuildReSTIRDISplitExtractionPipelineDescription(directPass,
            m_libraryVariant);
        description.shaderPath = PsrChainLibraryPath(PrimarySurfaceReplacementGBufferExtractionLibraryPath(
            rawDiffuseHitDistance, m_libraryVariant));
        description.rootParameters.push_back({RootParameterType::ConstantBuffer, 4});
        description.rootParameters.push_back({RootParameterType::ShaderResource, 2});
        description.rasterMaterialSampler = true;

        // RayQuery 2M E3: the direct pass's own description (its compute shader in production).
        if(directPass)
        {
            description = DirectPassDescription(true, m_psrChain);
        }

        const std::string libraryName = description.shaderPath.generic_string();
        const ShaderLibraryCompileResult* library = AcquireLibrary(shaderLibrary, description.shaderPath);

        if(!library)
        {
            failure = "The " + libraryName + " DXR library is unavailable or failed to compile" + TroubleshootingHint() +
                ".";
        }
        else if(!LibraryHasEntryPoint(library->bytecode, description))
        {
            failure = "The " + libraryName + " DXR library has no signal extraction entry point.";
        }
        else if(!InitializeVisibilityPipelines(graphicsDevice, shaderLibrary, *pipeline, library->bytecode, description))
        {
            failure = "The " + libraryName + " PSR.2 extraction pipeline could not be created" + TroubleshootingHint() +
                ".";
        }

        const auto twin = m_rayQueryPipelines.find(pipeline);

        if(twin != m_rayQueryPipelines.end())
        {
            m_psrRayQueryPipelines[pipeline] = std::move(twin->second);
            m_rayQueryPipelines.erase(twin);
        }

        if(!failure.empty())
        {
            for(const auto& created : pipelines)
            {
                created.first->Shutdown();
                m_psrRayQueryPipelines.erase(created.first);
            }

            m_psrGBufferExtractFailed[kind] = true;
            failureReason = failure;
            return false;
        }
    }

    m_psrGBufferExtractReady[kind] = true;
    return true;
}

bool PathTraceReferencePass::IsPrimarySurfaceReplacementGBufferExtractionReady(bool rawDiffuseHitDistance)
{
    RayTracingPipeline* indirect = rawDiffuseHitDistance ? &m_psrGBufferExtractIndirectRawPipeline :
        &m_psrGBufferExtractIndirectPipeline;
    return m_psrGBufferExtractReady[rawDiffuseHitDistance ? 1 : 0] &&
        (!m_secondaryShadowRayQuery || m_psrRayQueryPipelines[indirect]);
}

bool PathTraceReferencePass::EnsurePsrProbePipelinesForTesting(GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary)
{
    for(int storage = 0; storage < 2; ++storage)
    {
        if(m_psrProbePipelines[storage].IsInitialized())
        {
            continue;
        }

        const bool gBuffer = storage == 1;
        RayTracingPipelineDescription description = BuildReSTIRDISplitPipelineDescription(false, m_libraryVariant);
        description.rayGenerationShader = L"PathTracePsrProbeRayGeneration";
        description.shaderPath = gBuffer ? PrimarySurfaceReplacementGBufferLibraryPath(m_libraryVariant) :
            PrimarySurfaceReplacementLibraryPath(m_libraryVariant);

        if(gBuffer)
        {
            description.rootParameters.push_back({RootParameterType::ConstantBuffer, 4});
            description.rootParameters.push_back({RootParameterType::ShaderResource, 2});
            description.rasterMaterialSampler = true;
        }

        const ShaderLibraryCompileResult* library = AcquireLibrary(shaderLibrary, description.shaderPath);

        if(!library || !CreatePipeline(graphicsDevice, m_psrProbePipelines[storage], library->bytecode, description))
        {
            return false;
        }
    }

    return true;
}

bool PathTraceReferencePass::EnsurePsrChainProbePipelinesForTesting(GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary, const std::filesystem::path& indirectLibrary)
{
    // The PSR.2 indirect pass's own description, with its PSR bindings appended.
    RayTracingPipelineDescription description = BuildReSTIRDISplitPipelineDescription(false, m_libraryVariant);
    description.rayGenerationShader = L"PsrChainProbeRayGeneration";
    description.shaderPath = indirectLibrary;
    description.rootParameters.push_back({RootParameterType::ConstantBuffer, 4});
    description.rootParameters.push_back({RootParameterType::ShaderResource, 2});
    description.rasterMaterialSampler = true;
    m_psrChainProbePipeline.Shutdown();
    const ShaderLibraryCompileResult* library = AcquireLibrary(shaderLibrary, description.shaderPath);
    return library && CreatePipeline(graphicsDevice, m_psrChainProbePipeline, library->bytecode, description);
}

bool PathTraceReferencePass::IsPrimarySurfaceReplacementGBufferReady()
{
    // The twin the frame's visibility selection needs (GI.6 for the indirect pass).
    return m_psrGBufferPipelinesReady &&
        (!m_secondaryShadowRayQuery || m_psrRayQueryPipelines[&m_psrGBufferIndirectPipeline]);
}

void PathTraceReferencePass::RetainPsrPipeline(RayTracingPipeline& pipeline)
{
    if(!pipeline.IsInitialized()) return;
    const auto identity = pipeline.GetIdentity();
    // Diagnostic pipelines without an identity obey their existing idle-device teardown contract.
    if(identity.empty()) { pipeline.Shutdown(); return; }
    auto retained = std::make_unique<RayTracingPipeline>();
    retained->Swap(pipeline);
    if(m_retainedPsrPipelines.contains(identity)) m_retiredPsrDuplicates.push_back(std::move(retained));
    else m_retainedPsrPipelines.emplace(identity, std::move(retained));
}

void PathTraceReferencePass::ReleasePrimarySurfaceReplacementPipelines()
{
    // The GI.6 twins are registered against these slots; dropping them keeps the secondary selector's readiness about
    // the pipelines that exist.
    for(auto& pipeline : m_psrSharcEnergyPipelines) pipeline.Shutdown();

    for(RayTracingPipeline* original : {&m_psrRecordPipeline, &m_psrRasterPipeline, &m_psrGuidePipeline,
        &m_psrGBufferDirectPipeline,
        &m_psrGBufferIndirectPipeline, &m_psrSharcImageProbePipeline, &m_psrSharcTrainingProbePipeline, &m_psrProbePipelines[0], &m_psrProbePipelines[1],
        &m_psrChainProbePipeline,
        &m_psrGBufferExtractDirectPipeline, &m_psrGBufferExtractIndirectPipeline, &m_psrGBufferExtractIndirectRawPipeline})
    {
        const auto found = m_psrRayQueryPipelines.find(original);

        if(found != m_psrRayQueryPipelines.end())
        {
            if(found->second)
            {
                RetainPsrPipeline(*found->second);
            }

            m_psrRayQueryPipelines.erase(found);
        }

        RetainPsrPipeline(*original);
    }

    m_psrPipelinesReady = false;
    m_psrGuidePipelineReady = false;
    m_psrGBufferPipelinesReady = false;
    m_psrGBufferExtractReady[0] = false;
    m_psrGBufferExtractReady[1] = false;
}

bool PathTraceReferencePass::EnsurePsrSharcProbesForTesting(GraphicsDevice& device, ShaderLibrary& shaders, std::string& failure)
{
    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);

    RayTracingPipeline* pipelines[] = {&m_psrSharcImageProbePipeline, &m_psrSharcTrainingProbePipeline};
    for(int index = 0; index < 2; ++index)
    {
        if(pipelines[index]->IsInitialized()) continue;
        auto description = BuildRadianceCachePipelineDescription(m_libraryVariant, index == 0 ?
            PathTraceRadianceCacheLibrary::PsrRenderSplitIndirect : PathTraceRadianceCacheLibrary::PsrTraining);
        description.rayGenerationShader = index == 0 ? L"PathTracePsrProbeRayGeneration" :
            L"PathTracePsrSharcTrainingProbeRayGeneration";
        const auto* library = AcquireLibrary(shaders, description.shaderPath);
        if(!library || !pipelines[index]->Initialize(device.GetRayTracingDevice(), library->bytecode, description))
        {
            failure = "PSR + SHARC diagnostic state object failed.";
            return false;
        }
    }
    return true;
}

bool PathTraceReferencePass::EnsurePsrSharcEnergyForTesting(GraphicsDevice& device, ShaderLibrary& shaders, std::string& failure)
{
    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);

    const char* suffix = m_libraryVariant == PathTraceLibraryVariant::SM66 ? "" :
        m_libraryVariant == PathTraceLibraryVariant::SM69 ? "SM69" : "SERSM69";
    for(int index = 0; index < 2; ++index)
    {
        auto& pipeline = m_psrSharcEnergyPipelines[index];
        if(pipeline.IsInitialized()) continue;
        auto description = BuildRadianceCachePipelineDescription(m_libraryVariant, PathTraceRadianceCacheLibrary::PsrTraining);
        description.shaderPath = std::string("PathTraceReferencePsrSharcEnergy") + (index == 0 ? "Training" : "Reference") + suffix + ".hlsl";
        const auto* library = AcquireLibrary(shaders, description.shaderPath);
        if(!library || !pipeline.Initialize(device.GetRayTracingDevice(), library->bytecode, description))
        {
            failure = "M2A energy diagnostic pipeline unavailable.";
            return false;
        }
    }
    return true;
}

bool PathTraceReferencePass::PsrSharcInfrastructureReady(bool extract, bool raw)
{
    const auto ready = [&](PathTraceRadianceCacheLibrary library)
    {
        auto& entry = m_radianceCache[static_cast<size_t>(library)];
        return entry.ready && (!m_secondaryShadowRayQuery || library == PathTraceRadianceCacheLibrary::PsrTraining ||
            m_psrRayQueryPipelines[&entry.pipeline] != nullptr);
    };
    return (extract ? IsPrimarySurfaceReplacementGBufferExtractionReady(raw) :
        IsPrimarySurfaceReplacementGBufferReady()) && ready(PathTraceRadianceCacheLibrary::PsrTraining) &&
        ready(PsrRenderLibrary(extract, raw));
}

bool PathTraceReferencePass::SetPrimarySurfaceReplacementChain(std::uint32_t chain)
{
    chain = chain >= 2u ? 2u : 1u;

    if(chain == m_psrChain)
    {
        return false;
    }

    m_psrChain = chain;
    ResetPrimarySurfaceReplacementChainPipelines();
    return true;
}

void PathTraceReferencePass::SetPrimarySurfaceReplacementChainDirectoryForTesting(const std::filesystem::path& directory)
{
    if(directory == m_psrChainDirectoryForTesting)
    {
        return;
    }

    m_psrChainDirectoryForTesting = directory;

    if(m_psrChain >= 2)
    {
        ResetPrimarySurfaceReplacementChainPipelines();
    }
}

void PathTraceReferencePass::ResetPrimarySurfaceReplacementChainPipelines()
{
    ReleasePrimarySurfaceReplacementPipelines();
    m_psrGBufferPipelinesFailed = false;
    m_psrGBufferFailure.clear();

    for(int kind = 0; kind < 2; ++kind)
    {
        m_psrGBufferExtractFailed[kind] = false;
        m_psrGBufferExtractFailure[kind].clear();
    }

    // The combined PSR + SHARC learning pipelines and their GI.6 twins.
    // Only the D6 cache has combined PSR libraries.
    RadianceCacheLearningPipeline* d6Pipelines = m_radianceCacheSets[static_cast<size_t>(PathTraceRadianceCacheImplementation::EngineD6)];

    for(size_t index = 0; index < static_cast<size_t>(PathTraceRadianceCacheLibrary::Count); ++index)
    {
        if(!IsPsrCacheLibrary(static_cast<PathTraceRadianceCacheLibrary>(index)))
        {
            continue;
        }

        RadianceCacheLearningPipeline& learning = d6Pipelines[index];
        const auto twin = m_psrRayQueryPipelines.find(&learning.pipeline);

        if(twin != m_psrRayQueryPipelines.end())
        {
            if(twin->second)
            {
                RetainPsrPipeline(*twin->second);
            }

            m_psrRayQueryPipelines.erase(twin);
        }

        RetainPsrPipeline(learning.pipeline);
        learning.ready = false;
        learning.failed = false;
        learning.failure.clear();
    }
}

bool PathTraceReferencePass::EnsurePsrSharcInfrastructure(GraphicsDevice& device, ShaderLibrary& shaders,
    bool extract, bool raw, std::string& failure)
{
    if(!EnsurePrimarySurfaceReplacementGBuffer(device, shaders, failure) ||
        (extract && !EnsurePrimarySurfaceReplacementGBufferExtraction(device, shaders, false, failure)) ||
        (extract && raw && !EnsurePrimarySurfaceReplacementGBufferExtraction(device, shaders, true, failure)) ||
        !EnsureRadianceCacheLearning(device, shaders, failure, PathTraceRadianceCacheLibrary::PsrTraining) ||
        !EnsureRadianceCacheLearning(device, shaders, failure, PsrRenderLibrary(extract, raw))) return false;
    if(!PsrSharcInfrastructureReady(extract, raw))
    {
        failure = "PSR + SHARC infrastructure visibility pipelines are unavailable.";
        return false;
    }
    return true;
}

void PathTraceReferencePass::ExecuteRadianceCacheTraining(
    ID3D12GraphicsCommandList4* commandList,
    DescriptorHeap& descriptorHeap,
    const RayTracingSceneResources& rayTracingScene,
    D3D12_GPU_VIRTUAL_ADDRESS constantsAddress,
    D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstantsAddress,
    D3D12_GPU_VIRTUAL_ADDRESS trainingConstants,
    std::uint32_t threads, D3D12_GPU_VIRTUAL_ADDRESS psrConstants, D3D12_GPU_VIRTUAL_ADDRESS psrTransforms)
{
    if((psrConstants == 0) != (psrTransforms == 0)) return;
    const bool psr = psrConstants != 0;
    RadianceCacheLearningPipeline& training =
        m_radianceCache[static_cast<size_t>(psr ? PathTraceRadianceCacheLibrary::PsrTraining : PathTraceRadianceCacheLibrary::Training)];

    if(!m_initialized || !commandList || !training.ready || trainingConstants == 0 || threads == 0)
    {
        return;
    }

    // The Raster Primary root signature: the image tables are bound but the training ray generation writes none of them.
    RayTracingPipeline& trainingPipeline = psr && m_psrSharcEnergyForTesting ? m_psrSharcEnergyPipelines[0] : training.pipeline;
    if(!trainingPipeline.IsInitialized()) return;
    trainingPipeline.Bind(commandList);
    commandList->SetComputeRootShaderResourceView(0, rayTracingScene.GetTopLevel().GetGPUAddress());
    commandList->SetComputeRootDescriptorTable(1, descriptorHeap.GetGPUHandle(m_accumulationTexture.GetUAVIndex()));
    commandList->SetComputeRootDescriptorTable(2, descriptorHeap.GetGPUHandle(m_outputTexture.GetUAVIndex()));
    commandList->SetComputeRootDescriptorTable(3, descriptorHeap.GetGPUHandle(m_statisticsTexture.GetUAVIndex()));
    commandList->SetComputeRootConstantBufferView(4, constantsAddress);
    commandList->SetComputeRootConstantBufferView(5, sceneLightConstantsAddress);
    commandList->SetComputeRootConstantBufferView(6, trainingConstants);
    if(psr)
    {
        commandList->SetComputeRootConstantBufferView(7, psrConstants);
        commandList->SetComputeRootShaderResourceView(8, psrTransforms);
    }
    trainingPipeline.Dispatch(commandList, threads, 1);
    if(psr && m_psrSharcEnergyForTesting && m_psrSharcEnergyPipelines[1].IsInitialized())
    {
        m_psrSharcEnergyPipelines[1].Bind(commandList);
        commandList->SetComputeRootShaderResourceView(0, rayTracingScene.GetTopLevel().GetGPUAddress());
        commandList->SetComputeRootDescriptorTable(1, descriptorHeap.GetGPUHandle(m_accumulationTexture.GetUAVIndex()));
        commandList->SetComputeRootDescriptorTable(2, descriptorHeap.GetGPUHandle(m_outputTexture.GetUAVIndex()));
        commandList->SetComputeRootDescriptorTable(3, descriptorHeap.GetGPUHandle(m_statisticsTexture.GetUAVIndex()));
        commandList->SetComputeRootConstantBufferView(4, constantsAddress);
        commandList->SetComputeRootConstantBufferView(5, sceneLightConstantsAddress);
        commandList->SetComputeRootConstantBufferView(6, trainingConstants);
        commandList->SetComputeRootConstantBufferView(7, psrConstants);
        commandList->SetComputeRootShaderResourceView(8, psrTransforms);
        m_psrSharcEnergyPipelines[1].Dispatch(commandList, threads, 1);
    }
    if(psr && m_psrSharcTrainingProbeForTesting && m_psrSharcTrainingProbePipeline.IsInitialized())
    {
        // Readback follows this dispatch; the image probe and sparse probe use separate frames.
        m_psrSharcTrainingProbePipeline.Bind(commandList);
        commandList->SetComputeRootShaderResourceView(0, rayTracingScene.GetTopLevel().GetGPUAddress());
        commandList->SetComputeRootDescriptorTable(1, descriptorHeap.GetGPUHandle(m_accumulationTexture.GetUAVIndex()));
        commandList->SetComputeRootDescriptorTable(2, descriptorHeap.GetGPUHandle(m_outputTexture.GetUAVIndex()));
        commandList->SetComputeRootDescriptorTable(3, descriptorHeap.GetGPUHandle(m_statisticsTexture.GetUAVIndex()));
        commandList->SetComputeRootConstantBufferView(4, constantsAddress);
        commandList->SetComputeRootConstantBufferView(5, sceneLightConstantsAddress);
        commandList->SetComputeRootConstantBufferView(6, trainingConstants);
        commandList->SetComputeRootConstantBufferView(7, psrConstants);
        commandList->SetComputeRootShaderResourceView(8, psrTransforms);
        m_psrSharcTrainingProbePipeline.Dispatch(commandList, threads, 1);
    }
}

bool PathTraceReferencePass::EnsureRadianceCacheLearning(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    std::string& failureReason,
    PathTraceRadianceCacheLibrary library)
{
    TransitionTiming::Scope er1Scope("EnsureRadianceCacheLearning", 0.5);
    if(!m_initialized)
    {
        failureReason = "The path tracer is not initialized.";
        return false;
    }

    RadianceCacheLearningPipeline& learning = m_radianceCache[static_cast<size_t>(library)];

    if(learning.ready)
    {
        return true;
    }

    if(learning.failed)
    {
        failureReason = learning.failure;
        return false;
    }

    if(!RadianceCacheLibraryPath(m_libraryVariant, library, m_radianceCacheImplementation))
    {
        failureReason = "NVIDIA SHARC has no counterpart of this D6 cache library.";
        return false;
    }

    RayTracingPipelineDescription description = BuildRadianceCachePipelineDescription(m_libraryVariant, library,
        m_radianceCacheImplementation);

    // Multibounce PSR: the combined PSR + SHARC libraries follow the PSR chain length.
    if(IsPsrCacheLibrary(library))
    {
        description.shaderPath = PsrChainLibraryPath(description.shaderPath);
    }

    const std::string libraryName = description.shaderPath.generic_string();
    const ShaderLibraryCompileResult* compiled = AcquireLibrary(shaderLibrary, description.shaderPath);

    if(!compiled)
    {
        learning.failure = "The " + libraryName + " DXR library is unavailable or failed to compile" +
            TroubleshootingHint() + ".";
    }
    else if(!LibraryHasEntryPoint(compiled->bytecode, description))
    {
        learning.failure = "The " + libraryName + " DXR library has no Raster Primary entry point.";
    }
    else if(!InitializeVisibilityPipelines(graphicsDevice, shaderLibrary, learning.pipeline, compiled->bytecode, description))
    {
        learning.failure = "The " + libraryName + " learning DXR pipeline could not be created" +
            TroubleshootingHint() + ".";
    }

    // Combined libraries are optional development paths. Their visibility twins must never
    // change the ordinary SHARC/GI.6 readiness set, including a failed combined preparation.
    if(IsPsrCacheLibrary(library))
    {
        const auto twin = m_rayQueryPipelines.find(&learning.pipeline);
        if(twin != m_rayQueryPipelines.end())
        {
            m_psrRayQueryPipelines[&learning.pipeline] = std::move(twin->second);
            m_rayQueryPipelines.erase(twin);
        }
    }

    if(!learning.failure.empty())
    {
        learning.pipeline.Shutdown();
        learning.failed = true;
        failureReason = learning.failure;
        return false;
    }

    learning.ready = true;
    return true;
}

void PathTraceReferencePass::RecordOutputBarriers(ID3D12GraphicsCommandList* commandList, bool extract)
{
    // The next frame reads this frame's running mean; order the UAV accesses explicitly. The extraction outputs
    // are rewritten every extraction frame, so their writes are ordered too.
    D3D12_RESOURCE_BARRIER barriers[4]{};
    UINT barrierCount = 0;

    const auto addBarrier = [&](ID3D12Resource* resource)
    {
        barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[barrierCount].UAV.pResource = resource;
        ++barrierCount;
    };

    addBarrier(m_accumulationTexture.GetResource());
    addBarrier(m_statisticsTexture.GetResource());

    if(extract)
    {
        addBarrier(m_diffuseTexture.GetResource());
        addBarrier(m_specularTexture.GetResource());
    }

    commandList->ResourceBarrier(barrierCount, barriers);
}

bool PathTraceReferencePass::EnsureReSTIRDISplitPipelines(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    std::string& failureReason)
{
    TransitionTiming::Scope er1Scope("EnsureReSTIRDISplitPipelines", 0.5);
    if(!m_initialized)
    {
        failureReason = "The path tracer is not initialized.";
        return false;
    }

    if(m_splitPipelinesReady)
    {
        return true;
    }

    if(m_splitPipelinesFailed)
    {
        failureReason = m_splitPipelineFailure;
        return false;
    }

    RayTracingPipeline* pipelines[2] = {&m_splitDirectPipeline, &m_splitIndirectPipeline};

    for(int pass = 0; pass < 2; ++pass)
    {
        const bool directPass = pass == 0;
        // RayQuery 2M E3: the direct pass's own description (its compute shader in production).
        const RayTracingPipelineDescription description = directPass ? DirectPassDescription(false, 0u) :
            BuildReSTIRDISplitPipelineDescription(false, m_libraryVariant);
        const std::string libraryName = description.shaderPath.generic_string();
        const ShaderLibraryCompileResult* library = AcquireLibrary(shaderLibrary, description.shaderPath);

        if(!library)
        {
            m_splitPipelineFailure = "The " + libraryName + " DXR library is unavailable or failed to compile" +
                TroubleshootingHint() + ".";
        }
        else if(!LibraryHasEntryPoint(library->bytecode, description))
        {
            m_splitPipelineFailure = "The " + libraryName + " DXR library has no split ReSTIR DI " +
                (directPass ? "direct" : "indirect") + " entry point.";
        }
        else if(!InitializeVisibilityPipelines(graphicsDevice, shaderLibrary, *pipelines[pass], library->bytecode, description))
        {
            m_splitPipelineFailure = "The " + libraryName + " split ReSTIR DI DXR pipeline could not be created" +
                TroubleshootingHint() + ".";
        }

        if(!m_splitPipelineFailure.empty())
        {
            m_splitDirectPipeline.Shutdown();
            m_splitIndirectPipeline.Shutdown();
            m_splitPipelinesFailed = true;
            failureReason = m_splitPipelineFailure;
            return false;
        }
    }

    // Prepare both direct entry points before enabling the primary visibility selector. This prevents an
    // NRD/extraction change from constructing or replacing a direct state object after RayQuery is active.
    // A preparation failure disables only DI.1 selection; the original non-extraction pass remains usable.
    PreparePrimaryDiExtractionPipeline(graphicsDevice, shaderLibrary);
    m_splitPipelinesReady = true;
    return true;
}

bool PathTraceReferencePass::EnsureSignalExtraction(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    DescriptorHeap& descriptorHeap,
    PathTracePrimary primary,
    std::string& failureReason,
    bool rawDiffuseHitDistance)
{
    if(!m_initialized)
    {
        failureReason = "The path tracer is not initialized.";
        return false;
    }

    const bool split = primary == PathTracePrimary::RasterReSTIRDI;

    if(!split && primary != PathTracePrimary::Raster)
    {
        failureReason = "Signal extraction supports Raster Primary and split ReSTIR DI only.";
        return false;
    }

    if(!EnsureExtractionPipelines(graphicsDevice, shaderLibrary, split, false, failureReason) ||
        (rawDiffuseHitDistance && !EnsureExtractionPipelines(graphicsDevice, shaderLibrary, split, true, failureReason)))
    {
        return false;
    }

    if(!CreateExtractionTextures(graphicsDevice.GetDevice(), descriptorHeap))
    {
        failureReason = "The signal extraction outputs or their descriptors could not be allocated.";
        return false;
    }

    return true;
}

bool PathTraceReferencePass::EnsureExtractionPipelines(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    bool split,
    bool rawDiffuseHitDistance,
    std::string& failureReason)
{
    bool* readyState = split ? &m_extractSplitReady : &m_extractRasterReady;
    bool* failedState = split ? &m_extractSplitFailed : &m_extractRasterFailed;
    std::string* failureState = split ? &m_extractSplitFailure : &m_extractRasterFailure;

    if(rawDiffuseHitDistance)
    {
        readyState = split ? &m_extractSplitRawReady : &m_extractRasterRawReady;
        failedState = split ? &m_extractSplitRawFailed : &m_extractRasterRawFailed;
        failureState = split ? &m_extractSplitRawFailure : &m_extractRasterRawFailure;
    }

    bool& ready = *readyState;
    bool& failed = *failedState;
    std::string& failure = *failureState;

    if(!ready && failed)
    {
        failureReason = failure;
        return false;
    }

    if(!ready)
    {
        std::vector<std::pair<RayTracingPipeline*, RayTracingPipelineDescription>> pipelines;

        if(rawDiffuseHitDistance)
        {
            // The legacy extraction's description with the raw-distance build of the reference library: the same
            // export, root signature and bindings. Split ReSTIR DI needs only its indirect pass.
            RayTracingPipelineDescription description = split ?
                BuildReSTIRDISplitExtractionPipelineDescription(false, m_libraryVariant) :
                BuildRasterExtractionPipelineDescription(m_libraryVariant);
            description.shaderPath = RawDiffuseHitDistanceLibraryPath(m_libraryVariant);
            pipelines.emplace_back(split ? &m_extractSplitIndirectRawPipeline : &m_extractRasterRawPipeline,
                description);
        }
        else if(split)
        {
            if(!m_primaryDiExtractionPipelineReady)
            {
                pipelines.emplace_back(&m_extractSplitDirectPipeline, DirectPassDescription(true, 0u));
            }

            pipelines.emplace_back(&m_extractSplitIndirectPipeline,
                BuildReSTIRDISplitExtractionPipelineDescription(false, m_libraryVariant));
        }
        else
        {
            pipelines.emplace_back(&m_extractRasterPipeline, BuildRasterExtractionPipelineDescription(m_libraryVariant));
        }

        for(auto& [pipeline, description] : pipelines)
        {
            const std::string libraryName = description.shaderPath.generic_string();
            const ShaderLibraryCompileResult* library = AcquireLibrary(shaderLibrary, description.shaderPath);

            if(!library)
            {
                failure = "The " + libraryName + " DXR library is unavailable or failed to compile" +
                    TroubleshootingHint() + ".";
            }
            else if(!LibraryHasEntryPoint(library->bytecode, description))
            {
                failure = "The " + libraryName + " DXR library has no signal extraction entry point.";
            }
            else if(!InitializeVisibilityPipelines(graphicsDevice, shaderLibrary, *pipeline, library->bytecode, description))
            {
                failure = "The " + libraryName + " signal extraction DXR pipeline could not be created" +
                    TroubleshootingHint() + ".";
            }

            if(!failure.empty())
            {
                for(auto& created : pipelines)
                {
                    created.first->Shutdown();
                }

                failed = true;
                failureReason = failure;
                return false;
            }
        }

        if(split && !rawDiffuseHitDistance)
        {
            m_primaryDiExtractionPipelineReady = true;
        }

        ready = true;
    }

    return true;
}

bool PathTraceReferencePass::CreateExtractionTextures(ID3D12Device* device, DescriptorHeap& descriptorHeap)
{
    if(m_diffuseTexture.GetResource() && m_specularTexture.GetResource())
    {
        return true;
    }

    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);

    constexpr D3D12_RESOURCE_FLAGS unorderedAccess = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    constexpr D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Texture* outputs[2] = {&m_diffuseTexture, &m_specularTexture};
    const wchar_t* names[2] = {L"Path Trace Diffuse", L"Path Trace Specular"};

    for(int index = 0; index < 2; ++index)
    {
        Texture& output = *outputs[index];

        if(!output.Initialize(device, m_width, m_height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, unorderedAccess,
            initialState) || !output.CreateSRV(device, descriptorHeap) || !output.CreateUAV(device, descriptorHeap))
        {
            Log::Error("Failed to create the path tracing signal extraction outputs.");
            m_diffuseTexture.Shutdown();
            m_specularTexture.Shutdown();
            return false;
        }

        output.GetResource()->SetName(names[index]);
    }

    return true;
}

Texture* PathTraceReferencePass::EnsureHistoryPriorTexture(ID3D12Device* device, DescriptorHeap& descriptorHeap)
{
    if(m_historyPriorTexture.GetResource())
    {
        return &m_historyPriorTexture;
    }

    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);

    if(m_width == 0 || m_height == 0 || !m_historyPriorTexture.Initialize(device, m_width, m_height, 1, 1,
        DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS) || !m_historyPriorTexture.CreateSRV(device, descriptorHeap) ||
        !m_historyPriorTexture.CreateUAV(device, descriptorHeap))
    {
        Log::Error("Failed to create the SHARC history prior texture; history initialization is skipped.");
        m_historyPriorTexture.Shutdown();
        return nullptr;
    }

    m_historyPriorTexture.GetResource()->SetName(L"SHARC History Prior");
    return &m_historyPriorTexture;
}

Texture* PathTraceReferencePass::EnsureRawDiffuseHitDistanceTexture(ID3D12Device* device, DescriptorHeap& descriptorHeap)
{
    if(m_rawDiffuseHitDistanceTexture.GetResource())
    {
        return &m_rawDiffuseHitDistanceTexture;
    }

    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);

    if(m_width == 0 || m_height == 0 || !m_rawDiffuseHitDistanceTexture.Initialize(device, m_width, m_height, 1, 1,
        DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ||
        !m_rawDiffuseHitDistanceTexture.CreateSRV(device, descriptorHeap) ||
        !m_rawDiffuseHitDistanceTexture.CreateUAV(device, descriptorHeap))
    {
        Log::Error("Failed to create the raw diffuse hit distance texture.");
        m_rawDiffuseHitDistanceTexture.Shutdown();
        return nullptr;
    }

    m_rawDiffuseHitDistanceTexture.GetResource()->SetName(L"Path Trace Raw Diffuse Hit Distance");
    return &m_rawDiffuseHitDistanceTexture;
}

bool PathTraceReferencePass::HasRawDiffuseHitDistanceResources() const
{
    bool cacheLibraries = false;

    for(const auto& set : m_radianceCacheSets)
    {
        const auto& renderExtraction =
            set[static_cast<size_t>(PathTraceRadianceCacheLibrary::RenderExtractionRawDiffuseHitDistance)];
        const auto& renderSplitExtraction = set[static_cast<size_t>(
            PathTraceRadianceCacheLibrary::RenderSplitIndirectExtractionRawDiffuseHitDistance)];
        cacheLibraries = cacheLibraries || renderExtraction.ready || renderSplitExtraction.ready || renderExtraction.failed ||
            renderSplitExtraction.failed;
    }

    return m_rawDiffuseHitDistanceTexture.GetResource() || m_extractRasterRawReady || m_extractSplitRawReady ||
        m_extractRasterRawFailed || m_extractSplitRawFailed || cacheLibraries;
}

void PathTraceReferencePass::ReleaseRawDiffuseHitDistanceResources()
{
    m_rawDiffuseHitDistanceTexture.Shutdown();
    m_rawDiffuseHitDistanceWritten = false;

    // ER.1 Phase 3.1: the GI.6 RayQuery twins of these pipelines go with them (erased, so the remaining twins still decide
    // IsSecondaryShadowRayQueryReady); the next creation pairs new ones.
    const auto releaseTwin = [&](RayTracingPipeline& original)
    {
        const auto found = m_rayQueryPipelines.find(&original);

        if(found != m_rayQueryPipelines.end())
        {
            if(found->second)
            {
                found->second->Shutdown();
            }

            m_rayQueryPipelines.erase(found);
        }
    };

    releaseTwin(m_extractRasterRawPipeline);
    releaseTwin(m_extractSplitIndirectRawPipeline);

    for(auto& set : m_radianceCacheSets)
    {
        for(const PathTraceRadianceCacheLibrary library : {PathTraceRadianceCacheLibrary::RenderExtractionRawDiffuseHitDistance,
            PathTraceRadianceCacheLibrary::RenderSplitIndirectExtractionRawDiffuseHitDistance})
        {
            releaseTwin(set[static_cast<size_t>(library)].pipeline);
        }
    }

    m_extractRasterRawPipeline.Shutdown();
    m_extractSplitIndirectRawPipeline.Shutdown();
    m_extractRasterRawReady = false;
    m_extractRasterRawFailed = false;
    m_extractSplitRawReady = false;
    m_extractSplitRawFailed = false;
    m_extractRasterRawFailure.clear();
    m_extractSplitRawFailure.clear();

    for(auto& set : m_radianceCacheSets)
    {
        for(const PathTraceRadianceCacheLibrary library : {PathTraceRadianceCacheLibrary::RenderExtractionRawDiffuseHitDistance,
            PathTraceRadianceCacheLibrary::RenderSplitIndirectExtractionRawDiffuseHitDistance})
        {
            RadianceCacheLearningPipeline& pipeline = set[static_cast<size_t>(library)];
            pipeline.pipeline.Shutdown();
            pipeline.ready = false;
            pipeline.failed = false;
            pipeline.failure.clear();
        }
    }
}

unsigned int PathTraceReferencePass::EnsureSplitDirectRadiance(
    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    std::uint32_t samplesPerFrame,
    std::uint64_t lastSubmissionFence,
    std::uint64_t completedFence)
{
    return EnsureSplitSampleBuffer(m_splitDirectRadiance, L"Path Trace Split Direct Radiance", device, descriptorHeap,
        samplesPerFrame, lastSubmissionFence, completedFence);
}

unsigned int PathTraceReferencePass::EnsureSplitDirectDiffuse(
    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    std::uint32_t samplesPerFrame,
    std::uint64_t lastSubmissionFence,
    std::uint64_t completedFence)
{
    return EnsureSplitSampleBuffer(m_splitDirectDiffuse, L"Path Trace Split Direct Diffuse", device, descriptorHeap,
        samplesPerFrame, lastSubmissionFence, completedFence);
}

unsigned int PathTraceReferencePass::EnsureSplitSampleBuffer(
    SplitSampleBuffer& target,
    const wchar_t* name,
    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    std::uint32_t samplesPerFrame,
    std::uint64_t lastSubmissionFence,
    std::uint64_t completedFence)
{
    if(!m_initialized || samplesPerFrame == 0)
    {
        return UINT_MAX;
    }

    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);

    // Retired buffers whose last possible reader has completed.
    std::erase_if(target.retired,
        [&](RetiredBuffer& retired)
        {
            if(retired.submissionFence > completedFence)
            {
                return false;
            }

            retired.buffer.Shutdown();
            return true;
        });

    if(target.buffer.GetResource() && target.samples == samplesPerFrame)
    {
        return target.buffer.GetUAVDescriptorIndex();
    }

    // A failed size is not retried every frame; a new size, or a resize, tries again.
    if(target.failedSamples == samplesPerFrame)
    {
        return UINT_MAX;
    }

    // Submitted frames may still read the current buffer through its descriptor, so neither is released yet.
    if(target.buffer.GetResource())
    {
        target.retired.push_back(RetiredBuffer{std::move(target.buffer), lastSubmissionFence});
        target.samples = 0;
    }

    const std::uint64_t bytes = static_cast<std::uint64_t>(m_width) * m_height * samplesPerFrame *
        SplitDirectRadianceStride;

    if(bytes > UINT_MAX)
    {
        target.failedSamples = samplesPerFrame;
        Log::Error("Split ReSTIR DI per-sample radiance for " + std::to_string(samplesPerFrame) +
            " samples per frame at " + std::to_string(m_width) + "x" + std::to_string(m_height) +
            " exceeds 32-bit shader addressing.");
        return UINT_MAX;
    }

    if(!target.buffer.InitializeGPUWritable(device, static_cast<size_t>(bytes)) ||
        !target.buffer.CreateRawUAV(device, descriptorHeap))
    {
        target.buffer.Shutdown();
        target.failedSamples = samplesPerFrame;
        Log::Error("Failed to create a split ReSTIR DI per-sample radiance buffer (" + std::to_string(bytes) +
            " bytes).");
        return UINT_MAX;
    }

    target.buffer.GetResource()->SetName(name);
    target.samples = samplesPerFrame;
    target.failedSamples = 0;
    return target.buffer.GetUAVDescriptorIndex();
}

void PathTraceReferencePass::ReleaseSplitSampleBuffer(SplitSampleBuffer& target)
{
    for(RetiredBuffer& retired : target.retired)
    {
        retired.buffer.Shutdown();
    }

    target.retired.clear();
    target.buffer.Shutdown();
    target.samples = 0;
    target.failedSamples = 0;
}

void PathTraceReferencePass::ReleaseSplitDirectRadiance()
{
    ReleaseSplitSampleBuffer(m_splitDirectRadiance);
    ReleaseSplitSampleBuffer(m_splitDirectDiffuse);
}

void PathTraceReferencePass::ExecuteReSTIRDISplit(
    ID3D12GraphicsCommandList4* commandList,
    GpuTimestampProfiler& profiler,
    unsigned int frameIndex,
    DescriptorHeap& descriptorHeap,
    const RayTracingSceneResources& rayTracingScene,
    D3D12_GPU_VIRTUAL_ADDRESS constantsAddress,
    D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstantsAddress,
    unsigned int width,
    unsigned int height,
    D3D12_GPU_VIRTUAL_ADDRESS rtxdiConstantsAddress,
    bool extract,
    D3D12_GPU_VIRTUAL_ADDRESS radianceCacheConstants,
    PathTraceRadianceCacheLibrary radianceCacheLibrary,
    bool rawDiffuseHitDistance,
    D3D12_GPU_VIRTUAL_ADDRESS psrGBufferConstants,
    D3D12_GPU_VIRTUAL_ADDRESS psrGBufferTransforms)
{
    m_rawDiffuseHitDistanceWritten = false;
    m_splitDirectDiffuseWritten = false;
    m_psrWritten = false;
    m_psrGuidesWritten = false;
    m_psrGBufferWritten = false;
    const bool ready = extract ?
        m_extractSplitReady && m_splitDirectDiffuse.buffer.GetResource() && m_diffuseTexture.GetResource() :
        m_splitPipelinesReady;

#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
    m_sharcDiagnosticWritten = false;
#endif

    if(!m_initialized || !commandList || width != m_width || height != m_height || !ready ||
        !m_splitDirectRadiance.buffer.GetResource() || rtxdiConstantsAddress == 0)
    {
        return;
    }

    // SHARC.5B: the indirect pass may come from the cached-radiance library (its split-indirect export matching the
    // frame's extraction); the direct pass never does.
    const RadianceCacheLearningPipeline& radianceCache = m_radianceCache[static_cast<size_t>(radianceCacheLibrary)];
    // NRD.RELAX.2B: as in Execute, raw-distance extraction only with its output.
    const bool raw = extract && rawDiffuseHitDistance && m_rawDiffuseHitDistanceTexture.GetResource();
    const PathTraceRadianceCacheLibrary extractionLibrary = raw ?
        PathTraceRadianceCacheLibrary::RenderSplitIndirectExtractionRawDiffuseHitDistance :
        PathTraceRadianceCacheLibrary::RenderSplitIndirectExtraction;
    const PathTraceRadianceCacheLibrary expectedLibrary = extract ? extractionLibrary :
        PathTraceRadianceCacheLibrary::RenderSplitIndirect;
    const bool combined = IsPsrCacheLibrary(radianceCacheLibrary) && psrGBufferConstants != 0 &&
        psrGBufferTransforms != 0 && radianceCacheConstants != 0 &&
        radianceCacheLibrary == PsrRenderLibrary(extract, raw) && PsrSharcInfrastructureReady(extract, raw);
    const bool cachedIndirect = radianceCacheConstants != 0 &&
        (radianceCacheLibrary == expectedLibrary || combined) && radianceCache.ready;
    const bool rawPipeline = raw && !cachedIndirect && m_extractSplitRawReady;
    // PSR.2 Stage 3A: without a cache library, with both of its bindings. Stage 3B: with extraction, its extraction pair
    // (raw-distance indirect when the frame records raw distances).
    const bool gBufferReplacement = psrGBufferConstants != 0 && psrGBufferTransforms != 0 && (!cachedIndirect || combined) &&
        (extract ? IsPrimarySurfaceReplacementGBufferExtractionReady(raw) : IsPrimarySurfaceReplacementGBufferReady());
    // The direct pass is a compute shader (DirectPassDescription); only the indirect pass has RayQuery twins.
    RayTracingPipeline& directPipeline = extract ?
        (gBufferReplacement ? m_psrGBufferExtractDirectPipeline : m_extractSplitDirectPipeline) :
        (gBufferReplacement ? m_psrGBufferDirectPipeline : m_splitDirectPipeline);
    RayTracingPipeline& indirectPipeline = VisibilityPipeline(cachedIndirect ?
        m_radianceCache[static_cast<size_t>(radianceCacheLibrary)].pipeline :
        (extract ? (gBufferReplacement ? (raw ? m_psrGBufferExtractIndirectRawPipeline : m_psrGBufferExtractIndirectPipeline) :
        (rawPipeline ? m_extractSplitIndirectRawPipeline : m_extractSplitIndirectPipeline)) :
        (gBufferReplacement ? m_psrGBufferIndirectPipeline : m_splitIndirectPipeline)));

    m_accumulationTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_outputTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_statisticsTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_splitDirectRadiance.buffer.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if(extract)
    {
        m_splitDirectDiffuse.buffer.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_diffuseTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_specularTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // Both root signatures begin with Execute's six parameters; binding a pipeline may change the root
    // signature, which discards earlier root arguments, so each pass binds its own.
    const auto bindSharedRootParameters = [&]()
    {
        commandList->SetComputeRootShaderResourceView(0, rayTracingScene.GetTopLevel().GetGPUAddress());
        commandList->SetComputeRootDescriptorTable(1, descriptorHeap.GetGPUHandle(m_accumulationTexture.GetUAVIndex()));
        commandList->SetComputeRootDescriptorTable(2, descriptorHeap.GetGPUHandle(m_outputTexture.GetUAVIndex()));
        commandList->SetComputeRootDescriptorTable(3, descriptorHeap.GetGPUHandle(m_statisticsTexture.GetUAVIndex()));
        commandList->SetComputeRootConstantBufferView(4, constantsAddress);
        commandList->SetComputeRootConstantBufferView(5, sceneLightConstantsAddress);
    };

    // Direct pass: RTXDI sampling, reuse, visibility and reservoir publication; writes the direct radiance.
    profiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::PathTraceSplitDirect);
    directPipeline.Bind(commandList);
    bindSharedRootParameters();
    commandList->SetComputeRootConstantBufferView(6, rtxdiConstantsAddress);

    if(gBufferReplacement)
    {
        commandList->SetComputeRootConstantBufferView(7, psrGBufferConstants);
        commandList->SetComputeRootShaderResourceView(8, psrGBufferTransforms);
    }

    BeginPathTraceEvent(commandList, L"Engine/PT/ReSTIRDI/DirectLighting");
    // RayQuery 2M E3: the direct pipelines are compute shaders (DirectPassDescription), dispatched in thread groups.
    directPipeline.Dispatch(commandList, width, height);
    LogDirectPassBackend("compute shader, inline RayQuery visibility", extract, gBufferReplacement ? m_psrChain : 0u);
    EndPathTraceEvent(commandList);
    profiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::PathTraceSplitDirect);
    m_splitDirectDiffuseWritten = extract;

    // The indirect pass reads every direct radiance slot the direct pass wrote, and with extraction every direct
    // diffuse slot. Both passes only add to the statistics row atomically, so it needs no ordering between them.
    D3D12_RESOURCE_BARRIER directBarriers[2]{};
    directBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    directBarriers[0].UAV.pResource = m_splitDirectRadiance.buffer.GetResource();
    directBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    directBarriers[1].UAV.pResource = m_splitDirectDiffuse.buffer.GetResource();
    commandList->ResourceBarrier(extract ? 2 : 1, directBarriers);

    // Indirect pass: the path from vertex 0 onwards; accumulates and counts samples.
    profiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::PathTraceSplitIndirect);
    indirectPipeline.Bind(commandList);
    bindSharedRootParameters();

    if(cachedIndirect)
    {
        commandList->SetComputeRootConstantBufferView(6, radianceCacheConstants);
        if(combined)
        {
            commandList->SetComputeRootConstantBufferView(7, psrGBufferConstants);
            commandList->SetComputeRootShaderResourceView(8, psrGBufferTransforms);
        }
    }
    else if(gBufferReplacement)
    {
        commandList->SetComputeRootConstantBufferView(6, psrGBufferConstants);
        commandList->SetComputeRootShaderResourceView(7, psrGBufferTransforms);
    }

    BeginPathTraceEvent(commandList, L"Engine/PT/ReSTIRDI/IndirectLighting");
    indirectPipeline.Dispatch(commandList, width, height);
    EndPathTraceEvent(commandList);
#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
    m_sharcDiagnosticWritten = cachedIndirect;
#endif
    profiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::PathTraceSplitIndirect);
    m_rawDiffuseHitDistanceWritten = cachedIndirect ? IsRawDiffuseHitDistanceLibrary(radianceCacheLibrary) :
        (gBufferReplacement && extract ? raw : rawPipeline);

    m_psrGBufferWritten = gBufferReplacement;

    // Verification only: the recorded PSR pair's probe, with its indirect pass's bindings.
    RayTracingPipeline& probePipeline = combined ? m_psrSharcImageProbePipeline :
        (gBufferReplacement && m_psrChainProbePipeline.IsInitialized() ? m_psrChainProbePipeline :
        m_psrProbePipelines[gBufferReplacement ? 1 : 0]);

    if(m_psrProbeForTesting && !m_psrSharcEnergyForTesting && !m_psrSharcTrainingProbeForTesting && gBufferReplacement && probePipeline.IsInitialized())
    {
        m_psrProbeForTesting->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        probePipeline.Bind(commandList);
        bindSharedRootParameters();

        if(combined)
        {
            commandList->SetComputeRootConstantBufferView(6, radianceCacheConstants);
            commandList->SetComputeRootConstantBufferView(7, psrGBufferConstants);
            commandList->SetComputeRootShaderResourceView(8, psrGBufferTransforms);
        }
        else if(gBufferReplacement)
        {
            commandList->SetComputeRootConstantBufferView(6, psrGBufferConstants);
            commandList->SetComputeRootShaderResourceView(7, psrGBufferTransforms);
        }

        probePipeline.Dispatch(commandList, width, height);
    }

    RecordOutputBarriers(commandList, extract);
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
    std::uint64_t bytes = pixels * (16 + 8) + StatisticsWidth * 4;

    if(m_splitDirectRadiance.buffer.GetResource())
    {
        bytes += pixels * m_splitDirectRadiance.samples * SplitDirectRadianceStride;
    }

    // RTXPT D.1B.2 extraction: the per-sample direct diffuse radiance and two RGBA16F outputs.
    if(m_splitDirectDiffuse.buffer.GetResource())
    {
        bytes += pixels * m_splitDirectDiffuse.samples * SplitDirectRadianceStride;
    }

    if(m_diffuseTexture.GetResource())
    {
        bytes += pixels * (8 + 8);
    }

    if(m_historyPriorTexture.GetResource())
    {
        bytes += pixels * 8;
    }

    if(m_rawDiffuseHitDistanceTexture.GetResource())
    {
        bytes += pixels * 4;
    }

    return bytes;
}

void PathTraceReferencePass::Shutdown()
{
    ReleaseResources();
    m_libraries.clear();
    m_failedLibraries.clear();
    m_packagedLibraryFailures.clear();
    m_reportedMissingTwins.clear();
    // ER.1 Phase 4: never used by the GPU; the Renderer joined the pipeline worker before this.
    m_preparedPipelines.clear();
    for(auto& [identity, pipeline] : m_retainedPsrPipelines) pipeline->Shutdown();
    m_retainedPsrPipelines.clear();
    for(auto& pipeline : m_retiredPsrDuplicates) pipeline->Shutdown();
    m_retiredPsrDuplicates.clear();
    m_failedPipelines.clear();
}

void PathTraceReferencePass::ReleaseResources()
{
    // Same idle-device contract as the original pipeline teardown below.
    for(auto* pipelines : {&m_rayQueryPipelines})
    {
        for(auto& [original, alternate] : *pipelines)
        {
            if(alternate)
            {
                alternate->Shutdown();
            }
        }

        pipelines->clear();
    }

    m_secondaryShadowRayQuery = false;
    m_primaryDiExtractionPipelineReady = false;

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

    m_splitDirectPipeline.Shutdown();
    m_splitIndirectPipeline.Shutdown();
    m_splitPipelinesReady = false;
    m_splitPipelinesFailed = false;
    m_splitPipelineFailure.clear();
    ReleaseSplitDirectRadiance();

    m_extractRasterPipeline.Shutdown();
    m_extractSplitDirectPipeline.Shutdown();
    m_extractSplitIndirectPipeline.Shutdown();
    m_extractRasterReady = false;
    m_extractRasterFailed = false;
    m_extractSplitReady = false;
    m_extractSplitFailed = false;
    m_extractRasterFailure.clear();
    m_extractSplitFailure.clear();
    m_extractRasterRawPipeline.Shutdown();
    m_extractSplitIndirectRawPipeline.Shutdown();
    m_extractRasterRawReady = false;
    m_extractRasterRawFailed = false;
    m_extractSplitRawReady = false;
    m_extractSplitRawFailed = false;
    m_extractRasterRawFailure.clear();
    m_extractSplitRawFailure.clear();
    m_rawDiffuseHitDistanceWritten = false;
    m_diffuseTexture.Shutdown();
    m_specularTexture.Shutdown();
    m_historyPriorTexture.Shutdown();
    m_rawDiffuseHitDistanceTexture.Shutdown();

    for(auto& set : m_radianceCacheSets)
    {
        for(RadianceCacheLearningPipeline& learning : set)
        {
            const auto twin = m_psrRayQueryPipelines.find(&learning.pipeline);
            if(twin != m_psrRayQueryPipelines.end())
            {
                if(twin->second) twin->second->Shutdown();
                m_psrRayQueryPipelines.erase(twin);
            }
            learning.pipeline.Shutdown();
            learning.ready = false;
            learning.failed = false;
            learning.failure.clear();
        }
    }

    ReleasePrimarySurfaceReplacementPipelines();
    m_psrPipelinesFailed = false;
    m_psrFailure.clear();
    m_psrRecords.Shutdown();
    m_psrWritten = false;
    m_psrGuidePipelineFailed = false;
    m_psrGuideFailure.clear();
    m_psrGBufferPipelinesFailed = false;
    m_psrGBufferFailure.clear();

    for(int kind = 0; kind < 2; ++kind)
    {
        m_psrGBufferExtractFailed[kind] = false;
        m_psrGBufferExtractFailure[kind].clear();
    }
    m_psrGuides.Shutdown();
    m_psrGuidesWritten = false;

    m_width = 0;
    m_height = 0;
    m_initialized = false;
}

std::uint32_t PathTraceSequenceSeed(std::uint64_t sequenceFrame)
{
    std::uint32_t value = static_cast<std::uint32_t>(sequenceFrame ^ (sequenceFrame >> 32));
    value ^= value >> 16;
    value *= 0x85EBCA6Bu;
    value ^= value >> 13;
    value *= 0xC2B2AE35u;
    value ^= value >> 16;
    return value;
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

bool PathTraceReferencePass::InitializeVisibilityPipelines(
    GraphicsDevice& device, ShaderLibrary& shaders, RayTracingPipeline& original,
    const std::vector<char>& bytecode, const RayTracingPipelineDescription& description)
{
    TransitionTiming::Scope er1Scope("InitializeVisibilityPipelines", 0.5);
    if(!CreatePipeline(device, original, bytecode, description))
    {
        return false;
    }

    // GI.6: the RayQuery twin of an indirect, Reference or raster library (the compute direct pass has none).
    const std::optional<std::filesystem::path> twinPath = RayQueryTwinLibraryPath(description.shaderPath);

    if(!twinPath)
    {
        return true;
    }

    auto& alternate = m_rayQueryPipelines[&original];

    if(alternate)
    {
        alternate->Shutdown();
    }

    RayTracingPipelineDescription queryDescription = description;
    queryDescription.shaderPath = *twinPath;

    // PS.5B: a game cooked before its twins existed keeps the TraceRay pipeline it already built. That is the expected
    // fallback, so it is reported once per twin; a cooked twin that fails is a genuine error.
    if(shaders.IsPackaged() && !shaders.HasPackagedLibrary(queryDescription.shaderPath))
    {
        alternate.reset();
        ReportMissingPackagedTwin(queryDescription.shaderPath);
        return true;
    }

    // A cooked twin that PreparePackagedLibrary already rejected (and reported) is not admitted again.
    if(m_packagedLibraryFailures.contains(queryDescription.shaderPath))
    {
        alternate.reset();
        return true;
    }

    alternate = std::make_unique<RayTracingPipeline>();
    const ShaderLibraryCompileResult* library = AcquireLibrary(shaders, queryDescription.shaderPath);

    if(!library || !CreatePipeline(device, *alternate, library->bytecode, queryDescription))
    {
        alternate->Shutdown();
        alternate.reset();
        const std::string message = std::string("GI.6") +
            " RayQuery unavailable; original TraceRay retained: " + queryDescription.shaderPath.string();

        if(shaders.IsPackaged())
        {
            Log::Error(message);
        }
        else
        {
            Log::Warning(message);
        }
    }

    return true;
}

bool PathTraceReferencePass::IsSecondaryShadowRayQueryReady() const
{
    return m_initialized && !m_rayQueryPipelines.empty() &&
        std::all_of(m_rayQueryPipelines.begin(), m_rayQueryPipelines.end(),
            [](const auto& entry)
            {
                return entry.second != nullptr;
            });
}

void PathTraceReferencePass::SelectSecondaryShadowRayQuery(bool enabled)
{
    m_secondaryShadowRayQuery = enabled && IsSecondaryShadowRayQueryReady();
}

RayTracingPipeline& PathTraceReferencePass::VisibilityPipeline(RayTracingPipeline& original)
{
    const auto found = m_rayQueryPipelines.find(&original);

    if(m_secondaryShadowRayQuery && found != m_rayQueryPipelines.end() && found->second)
    {
        return *found->second;
    }

    // PSR.1a-P: the PSR twins, outside the shared readiness set.
    const auto replacement = m_psrRayQueryPipelines.find(&original);

    if(m_secondaryShadowRayQuery && replacement != m_psrRayQueryPipelines.end() && replacement->second)
    {
        return *replacement->second;
    }

    return original;
}

bool PathTraceReferencePass::PreparePrimaryDiExtractionPipeline(GraphicsDevice& device, ShaderLibrary& shaders)
{
    TransitionTiming::Scope er1Scope("PreparePrimaryDiExtractionPipeline", 0.5);
    if(m_primaryDiExtractionPipelineReady)
    {
        return true;
    }

    const RayTracingPipelineDescription description = DirectPassDescription(true, 0u);
    const ShaderLibraryCompileResult* library = AcquireLibrary(shaders, description.shaderPath);

    if(!library || !LibraryHasEntryPoint(library->bytecode, description) ||
        !InitializeVisibilityPipelines(device, shaders, m_extractSplitDirectPipeline, library->bytecode, description))
    {
        Log::Warning("DI.1 direct extraction preparation unavailable; primary visibility selector disabled.");
        return false;
    }

    m_primaryDiExtractionPipelineReady = true;
    return true;
}

RayTracingPipelineDescription PathTraceReferencePass::DirectPassDescription(bool extraction, std::uint32_t psrChain) const
{
    RayTracingPipelineDescription description = extraction ?
        BuildReSTIRDISplitExtractionPipelineDescription(true, m_libraryVariant) :
        BuildReSTIRDISplitPipelineDescription(true, m_libraryVariant);

    // PSR.2 (G-buffer storage): after the split pair's own parameters, b4 (the replacement pass's camera and identity)
    // and t2 (its transforms), with the geometry pass's material sampler.
    if(psrChain != 0u)
    {
        description.rootParameters.push_back({RootParameterType::ConstantBuffer, 4});
        description.rootParameters.push_back({RootParameterType::ShaderResource, 2});
        description.rasterMaterialSampler = true;
    }

    // The compute shader reproduces this description's radiance hit and miss (PathTraceTraceRadianceInline); if they no
    // longer match, it has no shader path, so its preparation fails visibly instead of lighting incorrectly.
    const bool contract = ComputeRetraceContractHolds(description);

    if(!contract)
    {
        static bool reported = false;

        if(!reported)
        {
            reported = true;
            Log::Error("The ReSTIR DI direct pass's hit groups no longer match its compute shader's inline radiance ray "
                "(PathTraceTraceRadianceInline); the compute direct pass is unavailable.");
        }
    }

    description.shaderPath = contract ? DirectLightingComputePath(extraction, psrChain) : std::filesystem::path();
    description.computeShader = true;
    description.rayGenerationShader = ShaderCompiler::ComputeLibraryEntryPoint;
    description.missShaders.clear();
    description.hitGroups.clear();
    description.maxPayloadSize = 0;
    description.maxAttributeSize = 0;
    return description;
}
