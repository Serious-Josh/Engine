#include "Renderer/PackagedShaderInventory.h"

#include "Renderer/Passes/PathTraceReferencePass.h"

#include <iterator>

namespace
{
    using Feature = PackagedShaderFeature;
    constexpr ShaderStage Vertex = ShaderStage::Vertex;
    constexpr ShaderStage Pixel = ShaderStage::Pixel;
    constexpr ShaderStage Compute = ShaderStage::Compute;

    // Each pass's ShaderDescription, by feature. A pass that loads a new entry point adds its row here.
    constexpr PackagedShaderEntry kShaders[] = {
        // Core raster, exposure, tone mapping and the material preview.
        {"BRDFIntegration.hlsl", "VSMain", Vertex, Feature::Core},
        {"BRDFIntegration.hlsl", "PSMain", Pixel, Feature::Core},
        {"DeferredLighting.hlsl", "VSMain", Vertex, Feature::Core},
        {"DeferredLighting.hlsl", "PSMain", Pixel, Feature::Core},
        {"DiffuseIrradiance.hlsl", "VSMain", Vertex, Feature::Core},
        {"DiffuseIrradiance.hlsl", "PSMain", Pixel, Feature::Core},
        {"EnvironmentMip.hlsl", "VSMain", Vertex, Feature::Core},
        {"EnvironmentMip.hlsl", "PSMain", Pixel, Feature::Core},
        {"EquirectangularToCubemap.hlsl", "VSMain", Vertex, Feature::Core},
        {"EquirectangularToCubemap.hlsl", "PSMain", Pixel, Feature::Core},
        {"GGXAverageAlbedo.hlsl", "VSMain", Vertex, Feature::Core},
        {"GGXAverageAlbedo.hlsl", "PSMain", Pixel, Feature::Core},
        {"GGXMultiscatterIrradiance.hlsl", "VSMain", Vertex, Feature::Core},
        {"GGXMultiscatterIrradiance.hlsl", "PSMain", Pixel, Feature::Core},
        {"SelectionMask.hlsl", "VSMain", Vertex, Feature::Core},
        {"SelectionMask.hlsl", "PSMain", Pixel, Feature::Core},
        {"ToneMap.hlsl", "VSMain", Vertex, Feature::Core},
        {"ToneMap.hlsl", "PSMain", Pixel, Feature::Core},
        {"Triangle.hlsl", "VSMain", Vertex, Feature::Core},
        {"Triangle.hlsl", "PSMain", Pixel, Feature::Core},
        {"AutoExposure.hlsl", "CSClear", Compute, Feature::Core},
        {"AutoExposure.hlsl", "CSHistogram", Compute, Feature::Core},
        {"AutoExposure.hlsl", "CSAdapt", Compute, Feature::Core},
        {"MaterialPreview.hlsl", "SphereVS", Vertex, Feature::Core},
        {"MaterialPreview.hlsl", "LightingVS", Vertex, Feature::Core},
        {"MaterialPreview.hlsl", "SpherePS", Pixel, Feature::Core},
        {"MaterialPreview.hlsl", "LightingPS", Pixel, Feature::Core},
        {"DisplacementIndices.hlsl", "CSMain", Compute, Feature::Core},
        {"DisplacementVertices.hlsl", "CSMain", Compute, Feature::Core},

        // Legacy lighting's denoisers and resolves.
        {"AODenoise.hlsl", "CSMain", Compute, Feature::Legacy},
        {"AOResolve.hlsl", "CSMain", Compute, Feature::Legacy},
        {"AOTemporal.hlsl", "CSMain", Compute, Feature::Legacy},
        {"GISignedCorrection.hlsl", "CSMain", Compute, Feature::Legacy},
        {"GISignedReconstruct.hlsl", "CSMain", Compute, Feature::Legacy},
        {"GISpatialDenoise.hlsl", "CSMain", Compute, Feature::Legacy},
        {"NrdDiffuseResolve.hlsl", "CSMain", Compute, Feature::Legacy},
        {"NrdGIPrepare.hlsl", "CSMain", Compute, Feature::Legacy},
        {"NrdReflectionPrepare.hlsl", "CSMain", Compute, Feature::Legacy},
        {"NrdReflectionResolve.hlsl", "CSMain", Compute, Feature::Legacy},
        {"RayTracedReflectionsDenoise.hlsl", "CSMain", Compute, Feature::Legacy},
        {"RayTracedReflectionTemporal.hlsl", "CSMain", Compute, Feature::Legacy},
        {"ReflectionContributionLimit.hlsl", "CSMain", Compute, Feature::Legacy},
        {"ShadowDenoise.hlsl", "CSMain", Compute, Feature::Legacy},

        // The Engine denoiser (PathTraceTemporalPass, PathTraceSpatialPass) and NrdPathTracePreparePass.
        {"PathTraceTemporal.hlsl", "CSMain", Compute, Feature::PathTraceDenoiser},
        {"PathTraceSpatial.hlsl", "CSMain", Compute, Feature::PathTraceDenoiser},
        {"PathTraceSpatial.hlsl", "CSDiagnostic", Compute, Feature::PathTraceDenoiser},
        {"PathTraceSpatialAtrous.hlsl", "CSFirst", Compute, Feature::PathTraceDenoiser},
        {"PathTraceSpatialAtrous.hlsl", "CSFirstPresent", Compute, Feature::PathTraceDenoiser},
        {"PathTraceSpatialAtrous.hlsl", "CSSecond", Compute, Feature::PathTraceDenoiser},
        {"PathTraceSpatialAtrous.hlsl", "CSFirstPresentDiagnostic", Compute, Feature::PathTraceDenoiser},
        {"PathTraceSpatialAtrous.hlsl", "CSSecondDiagnostic", Compute, Feature::PathTraceDenoiser},
        {"NrdPathTracePrepare.hlsl", "CSPrepare", Compute, Feature::PathTraceDenoiser},
        {"NrdPathTracePrepare.hlsl", "CSResolve", Compute, Feature::PathTraceDenoiser},
        {"NrdPathTracePrepare.hlsl", "CSResolveDenoised", Compute, Feature::PathTraceDenoiser},
        {"NrdPathTracePrepare.hlsl", "CSPrepareRelax", Compute, Feature::PathTraceDenoiser},
        {"NrdPathTracePrepare.hlsl", "CSResolveRelax", Compute, Feature::PathTraceDenoiser},
        {"NrdPathTracePrepare.hlsl", "CSResolveDenoisedRelax", Compute, Feature::PathTraceDenoiser},
        {"NrdPathTracePrepare.hlsl", "CSResolveRelaxSignalView", Compute, Feature::PathTraceDenoiser},
        {"NrdPathTracePrepare.hlsl", "CSPrepareHybrid", Compute, Feature::PathTraceDenoiser},
        {"NrdPathTracePrepare.hlsl", "CSResolveDenoisedHybrid", Compute, Feature::PathTraceDenoiser},

        // PresentationResolvePass (its SHARC spatial diagnostic pixel shader is a compile-time diagnostic, never packaged).
        {"PresentationResolve.hlsl", "VSMain", Vertex, Feature::Presentation},
        {"PresentationResolve.hlsl", "PSMain", Pixel, Feature::Presentation},

        // PSR: PsrInPlaceReplacePass (PsrInPlaceReplaceK2 is the two-bounce pixel shader) and the PSR variants of the
        // Engine denoiser and NRD preparation passes, created by Renderer::PreparePsrAuxiliary on the pipeline worker.
        {"PsrInPlaceReplace.hlsl", "VSMain", Vertex, Feature::PrimarySurfaceReplacement},
        {"PsrInPlaceReplace.hlsl", "PSMain", Pixel, Feature::PrimarySurfaceReplacement},
        {"PsrInPlaceReplaceK2.hlsl", "PSMain", Pixel, Feature::PrimarySurfaceReplacement},
        {"PathTraceTemporalPsr.hlsl", "CSMain", Compute, Feature::PrimarySurfaceReplacement},
        {"PathTraceSpatialAtrousPsr.hlsl", "CSFirst", Compute, Feature::PrimarySurfaceReplacement},
        {"PathTraceSpatialAtrousPsr.hlsl", "CSFirstPresent", Compute, Feature::PrimarySurfaceReplacement},
        {"PathTraceSpatialAtrousPsr.hlsl", "CSSecond", Compute, Feature::PrimarySurfaceReplacement},
        {"PathTraceSpatialAtrousPsr.hlsl", "CSFirstPresentDiagnostic", Compute, Feature::PrimarySurfaceReplacement},
        {"PathTraceSpatialAtrousPsr.hlsl", "CSSecondDiagnostic", Compute, Feature::PrimarySurfaceReplacement},
        {"NrdPathTracePreparePsr.hlsl", "CSPrepare", Compute, Feature::PrimarySurfaceReplacement},
        {"NrdPathTracePreparePsr.hlsl", "CSResolve", Compute, Feature::PrimarySurfaceReplacement},
        {"NrdPathTracePreparePsr.hlsl", "CSResolveDenoised", Compute, Feature::PrimarySurfaceReplacement},
        {"NrdPathTracePreparePsr.hlsl", "CSPrepareRelax", Compute, Feature::PrimarySurfaceReplacement},
        {"NrdPathTracePreparePsr.hlsl", "CSResolveRelax", Compute, Feature::PrimarySurfaceReplacement},
        {"NrdPathTracePreparePsr.hlsl", "CSResolveDenoisedRelax", Compute, Feature::PrimarySurfaceReplacement},
        {"NrdPathTracePreparePsr.hlsl", "CSPrepareHybrid", Compute, Feature::PrimarySurfaceReplacement},
        {"NrdPathTracePreparePsr.hlsl", "CSResolveDenoisedHybrid", Compute, Feature::PrimarySurfaceReplacement},

        // SHARC: the cache's clear, header and resolve (RadianceCacheD6::EnsureResources) and its record insertion
        // (EnsureInsertPipeline), which production SHARC frames create.
        {"RadianceCacheD6.hlsl", "CSClearWords", Compute, Feature::Sharc},
        {"RadianceCacheD6.hlsl", "CSWriteHeader", Compute, Feature::Sharc},
        {"RadianceCacheD6Resolve.hlsl", "CSResolve", Compute, Feature::Sharc},
        {"RadianceCacheD6Insert.hlsl", "CSInsertRecords", Compute, Feature::Sharc},
        {"RadianceCacheD6Insert.hlsl", "CSClearRecordStamps", Compute, Feature::Sharc},

        // DLSS: the input preparation every DLSS frame records (Renderer::RecordRenderFrame, DlssInputPreparationPass).
        {"DlssInputPreparation.hlsl", "CSMain", Compute, Feature::Dlss}};
}

const char* PackagedShaderFeatureName(PackagedShaderFeature feature)
{
    switch(feature)
    {
    case Feature::Core:
        return "core";
    case Feature::Legacy:
        return "Legacy";
    case Feature::PathTracing:
        return "Path Tracing";
    case Feature::PathTraceDenoiser:
        return "path-tracing denoiser";
    case Feature::PrimarySurfaceReplacement:
        return "Primary Surface Replacement";
    case Feature::Sharc:
        return "SHARC";
    case Feature::Dlss:
        return "DLSS";
    case Feature::Presentation:
    default:
        return "presentation";
    }
}

std::span<const PackagedShaderEntry> GetPackagedShaderInventory()
{
    return kShaders;
}

std::vector<PackagedLibraryEntry> GetPackagedLibraryInventory()
{
    // Legacy ray-traced lighting.
    std::vector<PackagedLibraryEntry> libraries = {
        {"RayTracedAO.hlsl", Feature::Legacy},
        {"RayTracedGI.hlsl", Feature::Legacy},
        {"RayTracedReflection.hlsl", Feature::Legacy},
        {"RayTracedShadow.hlsl", Feature::Legacy}};

    // The path tracer's own inventory (GetRequiredLibraries) for the packaged configurations.
    for(const PathTraceLibraryRequirement& library : PathTraceReferencePass::GetPackagedLibraries())
    {
        const std::string name = library.path.generic_string();
        const Feature feature = name.find("Sharc") != std::string::npos ||
            name.find("RadianceCache") != std::string::npos ? Feature::Sharc :
            name.find("Psr") != std::string::npos ? Feature::PrimarySurfaceReplacement : Feature::PathTracing;
        libraries.push_back({library.path, feature, library.optional});
    }

    return libraries;
}
