#pragma once

#include "Renderer/PipelineDescription.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct RayTracingHitGroupDescription
{
    std::wstring name;
    std::wstring closestHitShader;
    std::wstring anyHitShader;
};

struct RayTracingPipelineDescription
{
    std::filesystem::path shaderPath;

    std::wstring rayGenerationShader;

    std::vector<std::wstring> missShaders;

    std::vector<RayTracingHitGroupDescription> hitGroups;

    std::vector<RootParameterDescription> rootParameters;

    std::uint32_t maxPayloadSize = 0;
    std::uint32_t maxAttributeSize = 0;
    std::uint32_t maxRecursionDepth = 1;

    // PSR.2 Stage 3A: adds s2, the geometry pass's material sampler (trilinear clamp), to the global root signature.
    bool rasterMaterialSampler = false;
};
