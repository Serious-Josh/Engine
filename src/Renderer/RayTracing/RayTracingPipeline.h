#pragma once

#include "ShaderTable.h"
#include "RayTracingPipelineDescription.h"

#include <d3d12.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class ShaderLibrary;

// A path-tracing pipeline: a DXR state object (ray generation, miss and hit shaders, shader tables), or (RayQuery 2M E3,
// RayTracingPipelineDescription::computeShader) a compute pipeline whose shader traces inline RayQuery. Both use the same
// global root signature from the description, so their passes bind the same root arguments.
class RayTracingPipeline
{
    public:
    RayTracingPipeline() = default;
    ~RayTracingPipeline() { Shutdown(); }
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

    bool IsInitialized() const
    {
        return m_stateObject != nullptr || m_computePipeline != nullptr;
    }

    // RayQuery 2M E3: a compute pipeline (its Dispatch launches thread groups, not rays).
    bool IsCompute() const
    {
        return m_computePipeline != nullptr;
    }

    // ER.1 Phase 4: the owner's identity for what this pipeline was built from (description key and library revision);
    // cleared by Shutdown, exchanged by Swap.
    void SetIdentity(std::string identity)
    {
        m_identity = std::move(identity);
    }

    const std::string& GetIdentity() const
    {
        return m_identity;
    }

    // ER.1 Phase 4: exchanges every owned object with another pipeline (state object, root signature, tables, and the
    // M4.2C sampled variant). The render thread adopts a pipeline the background worker built this way; neither is in
    // use by the GPU.
    void Swap(RayTracingPipeline& other) noexcept;

    // M4.2C: the separate permutation of this pipeline with the sampled light estimator (Engine's M42A libraries), used
    // instead of it on frames that select sampled NEE. Owned by this pipeline, so it is shut down, retained and adopted
    // with it; null when none was built.
    RayTracingPipeline* GetSampledVariant() const
    {
        return m_sampledVariant && m_sampledVariant->IsInitialized() ? m_sampledVariant.get() : nullptr;
    }

    void SetSampledVariant(std::unique_ptr<RayTracingPipeline> variant)
    {
        if(m_sampledVariant)
        {
            m_sampledVariant->Shutdown();
        }

        m_sampledVariant = std::move(variant);
    }

    void Bind(ID3D12GraphicsCommandList4* commandList) const;
    // Diagnostics: the stack size the driver reports for one export of the state object (0 when unavailable).
    std::uint64_t GetShaderStackSize(const wchar_t* exportName) const;
    // Rays for width x height x depth pixels, or for a compute pipeline the thread groups that cover them
    // (RayTracingPipelineDescription::computeThreadGroupSize squares).
    void Dispatch(
        ID3D12GraphicsCommandList4* commandList,
        unsigned int width,
        unsigned int height,
        unsigned int depth = 1) const;

    private:
    bool CreateGlobalRootSignature(ID3D12Device5* device, const RayTracingPipelineDescription& description);
    bool InitializeCompute(ID3D12Device5* device, const std::vector<char>& shader,
        const RayTracingPipelineDescription& description);

    ID3D12StateObject* m_stateObject = nullptr;
    ID3D12PipelineState* m_computePipeline = nullptr;

    ID3D12RootSignature* m_globalRootSignature = nullptr;

    ShaderTable m_rayGenerationShaderTable;
    ShaderTable m_missShaderTable;
    ShaderTable m_hitGroupShaderTable;
    std::string m_identity;
    std::unique_ptr<RayTracingPipeline> m_sampledVariant;
};
