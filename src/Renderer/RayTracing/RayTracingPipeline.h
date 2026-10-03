#pragma once

#include <d3d12.h>
#include <filesystem>

class ShaderLibrary;

class RayTracingPipeline
{
    public:
        RayTracingPipeline() = default;
        RayTracingPipeline(const RayTracingPipeline&) = delete;
        RayTracingPipeline& operator=(const RayTracingPipeline&) = delete;

        bool Initialize(ID3D12Device5* device, ShaderLibrary& shaderLibrary, const std::filesystem::path& shaderPath);
        void Shutdown();

    private:
        ID3D12StateObject* m_stateObject = nullptr;

        ID3D12RootSignature* m_globalRootSignature = nullptr;
};