#pragma once

#include "ShaderTable.h"
#include "RayTracingPipelineDescription.h"

#include <d3d12.h>

#include <cstdint>
#include <string>
#include <utility>
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

    bool IsInitialized() const
    {
        return m_stateObject != nullptr;
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

    // ER.1 Phase 4: exchanges every owned object with another pipeline (state object, root signature, tables). The
    // render thread adopts a pipeline the background worker built this way; neither is in use by the GPU.
    void Swap(RayTracingPipeline& other) noexcept;

    void Bind(ID3D12GraphicsCommandList4* commandList) const;
    // Diagnostics: the stack size the driver reports for one export of the state object (0 when unavailable).
    std::uint64_t GetShaderStackSize(const wchar_t* exportName) const;
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
    std::string m_identity;
};
