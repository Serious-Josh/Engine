#pragma once

#include "Renderer/VertexLayout.h"
#include "Renderer/Shader.h"

#include <d3d12.h>
#include <array>
#include <vector>

enum class RootParameterType
{
    Constants,
    ConstantBuffer
};

struct RootParameterDescription
{
    RootParameterType type = RootParameterType::Constants;
    
    unsigned int shaderRegister= 0;
    unsigned int constantCount = 0;
    
    D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL;
};

struct PipelineRenderTargets
{
    std::array<DXGI_FORMAT, 8> formats{};
    unsigned int count = 0;
};

struct PipelineDescription
{
    const char* vertexShaderPath = nullptr;
    const char* pixelShaderPath = nullptr;

    const VertexLayout* vertexLayout = nullptr;

    PipelineRenderTargets renderTargets{};

    std::vector<RootParameterDescription> rootParameters;

    bool depthEnabled = false;
    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
};

class Pipeline
{
    public:
        Pipeline() = default;

        Pipeline(const Pipeline&) = delete;
        Pipeline& operator=(const Pipeline&) = delete;
        
        bool Initialize(ID3D12Device* device, const PipelineDescription& description);
        void Shutdown();
        void Bind(ID3D12GraphicsCommandList* commandList) const;

        ID3D12RootSignature* GetRootSignature() const;

    private:
        ID3D12RootSignature* m_rootSignature = nullptr;
        ID3D12PipelineState* m_pipelineState = nullptr;

        Shader m_vertexShader;
        Shader m_pixelShader;
};