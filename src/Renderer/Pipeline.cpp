#include "Pipeline.h"

bool Pipeline::Initialize(
    ID3D12Device* device,
    const PipelineDescription& description)
{
    if (description.renderTargets.count > 8)
    {
        return false;
    }

    std::vector<D3D12_ROOT_PARAMETER> rootParameters;

    rootParameters.reserve(description.rootParameters.size());

    for (const RootParameterDescription& parameterDescription : description.rootParameters)
    {
        D3D12_ROOT_PARAMETER parameter{};

        parameter.ShaderVisibility = parameterDescription.visibility;

        switch (parameterDescription.type)
        {
        case RootParameterType::Constants:
        {
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameter.Constants.ShaderRegister = parameterDescription.shaderRegister;
            parameter.Constants.RegisterSpace = 0;
            parameter.Constants.Num32BitValues = parameterDescription.constantCount;
            break;
        }

        case RootParameterType::ConstantBuffer:
        {
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameter.Descriptor.ShaderRegister = parameterDescription.shaderRegister;
            parameter.Descriptor.RegisterSpace = 0;
            break;
        }
        }

        rootParameters.push_back(parameter);
    }

    D3D12_STATIC_SAMPLER_DESC sampler{};

    sampler.Filter = description.sampler.filter;

    sampler.AddressU = description.sampler.addressU;

    sampler.AddressV = description.sampler.addressV;

    sampler.AddressW = description.sampler.addressW;
    sampler.MipLODBias = 0.0f;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;

    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = description.sampler.shaderRegister;

    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = description.sampler.visibility;

    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers{sampler};
    samplers.insert(samplers.end(), description.additionalSamplers.begin(), description.additionalSamplers.end());
    D3D12_ROOT_SIGNATURE_DESC rootSignatureDescription{};

    rootSignatureDescription.NumParameters = static_cast<UINT>(rootParameters.size());
    rootSignatureDescription.pParameters = rootParameters.empty() ? nullptr : rootParameters.data();
    rootSignatureDescription.NumStaticSamplers = static_cast<UINT>(samplers.size());
    rootSignatureDescription.pStaticSamplers = samplers.data();
    rootSignatureDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                                     D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    ID3DBlob* rootSignatureBlob = nullptr;
    ID3DBlob* rootSignatureError = nullptr;

    HRESULT result = D3D12SerializeRootSignature(&rootSignatureDescription, D3D_ROOT_SIGNATURE_VERSION_1,
                                                 &rootSignatureBlob, &rootSignatureError);

    if (FAILED(result))
    {
        if (rootSignatureError)
        {
            rootSignatureError->Release();
        }

        return false;
    }

    result = device->CreateRootSignature(0, rootSignatureBlob->GetBufferPointer(), rootSignatureBlob->GetBufferSize(),
                                         IID_PPV_ARGS(&m_rootSignature));

    rootSignatureBlob->Release();

    if (rootSignatureError)
    {
        rootSignatureError->Release();
    }

    if (FAILED(result))
    {
        return false;
    }

    if (!description.vertexShader || !description.pixelShader)
    {
        return false;
    }

    if (description.vertexShader->GetBytecodeSize() == 0 || description.pixelShader->GetBytecodeSize() == 0)
    {
        return false;
    }

    std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;

    if (description.vertexLayout)
    {

        for (const VertexAttribute& attribute : description.vertexLayout->GetAttributes())
        {
            inputElements.push_back({attribute.semantic, attribute.semanticIndex,
                                     VertexLayout::GetDXGIFormat(attribute.type), 0, attribute.offset,
                                     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0});
        }
    }

    D3D12_RASTERIZER_DESC rasterizerDescription{};

    rasterizerDescription.FillMode = D3D12_FILL_MODE_SOLID;

    rasterizerDescription.CullMode = D3D12_CULL_MODE_BACK;

    rasterizerDescription.FrontCounterClockwise = FALSE;
    rasterizerDescription.DepthBias = 0;
    rasterizerDescription.DepthBiasClamp = 0.0f;
    rasterizerDescription.SlopeScaledDepthBias = 0.0f;

    rasterizerDescription.DepthClipEnable = TRUE;
    rasterizerDescription.MultisampleEnable = FALSE;
    rasterizerDescription.AntialiasedLineEnable = FALSE;
    rasterizerDescription.ForcedSampleCount = 0;

    rasterizerDescription.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_BLEND_DESC blendDescription{};

    blendDescription.AlphaToCoverageEnable = FALSE;
    blendDescription.IndependentBlendEnable = FALSE;

    for (unsigned int i = 0; i < description.renderTargets.count; ++i)
    {
        blendDescription.RenderTarget[i].BlendEnable = FALSE;

        blendDescription.RenderTarget[i].LogicOpEnable = FALSE;
        blendDescription.RenderTarget[i].SrcBlend = D3D12_BLEND_ONE;
        blendDescription.RenderTarget[i].DestBlend = D3D12_BLEND_ZERO;

        blendDescription.RenderTarget[i].BlendOp = D3D12_BLEND_OP_ADD;
        blendDescription.RenderTarget[i].SrcBlendAlpha = D3D12_BLEND_ONE;
        blendDescription.RenderTarget[i].DestBlendAlpha = D3D12_BLEND_ZERO;
        blendDescription.RenderTarget[i].BlendOpAlpha = D3D12_BLEND_OP_ADD;

        blendDescription.RenderTarget[i].LogicOp = D3D12_LOGIC_OP_NOOP;
        blendDescription.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    D3D12_DEPTH_STENCIL_DESC depthStencilDescription{};

    depthStencilDescription.DepthEnable = description.depthEnabled;
    depthStencilDescription.DepthWriteMask =

        description.depthEnabled ? description.depthWriteMask : D3D12_DEPTH_WRITE_MASK_ZERO;
    depthStencilDescription.DepthFunc =
        description.depthEnabled ? description.depthComparison : D3D12_COMPARISON_FUNC_ALWAYS;
    depthStencilDescription.StencilEnable = FALSE;
    depthStencilDescription.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    depthStencilDescription.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDescription{};

    pipelineDescription.pRootSignature = m_rootSignature;
    pipelineDescription.VS.pShaderBytecode = description.vertexShader->GetByteCode();
    pipelineDescription.VS.BytecodeLength = description.vertexShader->GetBytecodeSize();
    pipelineDescription.PS.pShaderBytecode = description.pixelShader->GetByteCode();

    pipelineDescription.PS.BytecodeLength = description.pixelShader->GetBytecodeSize();
    pipelineDescription.BlendState = blendDescription;
    pipelineDescription.SampleMask = UINT_MAX;
    pipelineDescription.RasterizerState = rasterizerDescription;

    pipelineDescription.DepthStencilState = depthStencilDescription;
    pipelineDescription.InputLayout.pInputElementDescs = inputElements.data();
    pipelineDescription.InputLayout.NumElements = static_cast<UINT>(inputElements.size());
    pipelineDescription.PrimitiveTopologyType = description.topologyType;

    pipelineDescription.NumRenderTargets = description.renderTargets.count;

    for (unsigned int i = 0; i < description.renderTargets.count; ++i)
    {
        pipelineDescription.RTVFormats[i] = description.renderTargets.formats[i];
    }

    pipelineDescription.DSVFormat = description.depthEnabled ? description.depthFormat : DXGI_FORMAT_UNKNOWN;

    pipelineDescription.SampleDesc.Count = 1;

    result = device->CreateGraphicsPipelineState(&pipelineDescription, IID_PPV_ARGS(&m_pipelineState));

    if (FAILED(result))
    {
        return false;
    }

    return true;
}

void Pipeline::Shutdown()
{
    if (m_pipelineState)
    {
        m_pipelineState->Release();

        m_pipelineState = nullptr;
    }

    if (m_rootSignature)
    {
        m_rootSignature->Release();

        m_rootSignature = nullptr;
    }
}

void Pipeline::Bind(ID3D12GraphicsCommandList* commandList) const
{
    commandList->SetPipelineState(m_pipelineState);

    commandList->SetGraphicsRootSignature(m_rootSignature);
}

ID3D12RootSignature* Pipeline::GetRootSignature() const
{
    return m_rootSignature;
}

void Pipeline::Swap(Pipeline& other) noexcept
{
    std::swap(m_rootSignature, other.m_rootSignature);
    std::swap(m_pipelineState, other.m_pipelineState);
}
