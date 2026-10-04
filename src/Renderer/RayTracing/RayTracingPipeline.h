#pragma once

#include "ShaderTable.h"
#include "RayTracingPipelineDescription.h"

#include <d3d12.h>

class ShaderLibrary;

class RayTracingPipeline
{
    public:
        RayTracingPipeline() = default;
        RayTracingPipeline(const RayTracingPipeline&) = delete;
        RayTracingPipeline& operator=(const RayTracingPipeline&) = delete;

        bool Initialize(ID3D12Device5* device, ShaderLibrary& shaderLibrary, const RayTracingPipelineDescription& description);
        void Shutdown();

        ID3D12StateObject* GetStateObject() const;
        ID3D12RootSignature* GetGlobalRootSignature() const;

        D3D12_DISPATCH_RAYS_DESC CreateDispatchDescription(unsigned int width, unsigned int height, unsigned int depth = 1) const;

    private:
        ID3D12StateObject* m_stateObject = nullptr;

        ID3D12RootSignature* m_globalRootSignature = nullptr;

        ShaderTable m_rayGenerationShaderTable;
        ShaderTable m_missShaderTable;
        ShaderTable m_hitGroupShaderTable;
};