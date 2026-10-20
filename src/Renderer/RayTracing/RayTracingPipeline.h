#pragma once

#include "ShaderTable.h"
#include "RayTracingPipelineDescription.h"

#include <d3d12.h>

#include <vector>

class ShaderLibrary;

class RayTracingPipeline
{
    public:
    RayTracingPipeline() = default;
    RayTracingPipeline(const RayTracingPipeline&) = delete;
    RayTracingPipeline& operator=(const RayTracingPipeline&) = delete;

    bool Initialize(
        ID3D12Device5* device,
        ShaderLibrary& shaderLibrary,
        const RayTracingPipelineDescription& description);
    // Builds the pipeline from an already compiled description.shaderPath library, which the caller keeps
    // or discards; the state object holds its own copy.
    bool Initialize(
        ID3D12Device5* device,
        const std::vector<char>& library,
        const RayTracingPipelineDescription& description);
    void Shutdown();

    void Bind(ID3D12GraphicsCommandList4* commandList) const;
    void Dispatch(
        ID3D12GraphicsCommandList4* commandList,
        unsigned int width,
        unsigned int height,
        unsigned int depth = 1) const;

    private:
    ID3D12StateObject* m_stateObject = nullptr;

    ID3D12RootSignature* m_globalRootSignature = nullptr;

    ShaderTable m_rayGenerationShaderTable;
    ShaderTable m_missShaderTable;
    ShaderTable m_hitGroupShaderTable;
};
