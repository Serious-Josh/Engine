#include "Pipeline.h"

bool Pipeline::Initialize(
    ID3D12Device* device,
    const char* vertexShaderPath,
    const char* pixelShaderPath,
    const VertexLayout& vertexLayout
)
{
    D3D12_ROOT_SIGNATURE_DESC rootSignatureDescription{};

    rootSignatureDescription.NumParameters = 0;
    rootSignatureDescription.pParameters = nullptr;
    rootSignatureDescription.NumStaticSamplers = 0;
    rootSignatureDescription.pStaticSamplers = nullptr;
    rootSignatureDescription.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* rootSignatureBlob = nullptr;
    ID3DBlob* rootSignatureError = nullptr;

    HRESULT result = D3D12SerializeRootSignature(
        &rootSignatureDescription,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &rootSignatureBlob,
        &rootSignatureError
    );

    if(FAILED(result))
    {
        if(rootSignatureError)
        {
            rootSignatureError->Release();
        }

        return false;
    }

    result = device->CreateRootSignature(
        0,
        rootSignatureBlob->GetBufferPointer(),
        rootSignatureBlob->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature)
    );

    rootSignatureBlob->Release();

    if(rootSignatureError)
    {
        rootSignatureError->Release();
    }

    if(FAILED(result))
    {
        return false;
    }

    if(!m_vertexShader.Load(vertexShaderPath)){
        return false;
    }

    if(!m_pixelShader.Load(pixelShaderPath)){
        m_vertexShader.Unload();
        return false;
    }

    std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;

    for(const VertexAttribute& attribute : vertexLayout.GetAttributes()){
        inputElements.push_back({
            attribute.semantic, attribute.semanticIndex, VertexLayout::GetDXGIFormat(attribute.type), 0, attribute.offset, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
        });
    }

    D3D12_RASTERIZER_DESC rasterizerDescription{};

    rasterizerDescription.FillMode =
        D3D12_FILL_MODE_SOLID;

    rasterizerDescription.CullMode =
        D3D12_CULL_MODE_BACK;

    rasterizerDescription.FrontCounterClockwise = FALSE;
    rasterizerDescription.DepthBias = 0;
    rasterizerDescription.DepthBiasClamp = 0.0f;
    rasterizerDescription.SlopeScaledDepthBias = 0.0f;
    rasterizerDescription.DepthClipEnable = TRUE;
    rasterizerDescription.MultisampleEnable = FALSE;
    rasterizerDescription.AntialiasedLineEnable = FALSE;
    rasterizerDescription.ForcedSampleCount = 0;
    rasterizerDescription.ConservativeRaster =
        D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_BLEND_DESC blendDescription{};

    blendDescription.AlphaToCoverageEnable = FALSE;
    blendDescription.IndependentBlendEnable = FALSE;

    blendDescription.RenderTarget[0].BlendEnable = FALSE;
    blendDescription.RenderTarget[0].LogicOpEnable = FALSE;
    blendDescription.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blendDescription.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    blendDescription.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendDescription.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDescription.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blendDescription.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDescription.RenderTarget[0].LogicOp =
        D3D12_LOGIC_OP_NOOP;

    blendDescription.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_DEPTH_STENCIL_DESC depthStencilDescription{};

    depthStencilDescription.DepthEnable = FALSE;
    depthStencilDescription.DepthWriteMask =
        D3D12_DEPTH_WRITE_MASK_ZERO;
    depthStencilDescription.DepthFunc =
        D3D12_COMPARISON_FUNC_ALWAYS;
    depthStencilDescription.StencilEnable = FALSE;
    depthStencilDescription.StencilReadMask =
        D3D12_DEFAULT_STENCIL_READ_MASK;
    depthStencilDescription.StencilWriteMask =
        D3D12_DEFAULT_STENCIL_WRITE_MASK;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDescription{};

    pipelineDescription.pRootSignature = m_rootSignature;
    pipelineDescription.VS.pShaderBytecode = m_vertexShader.GetByteCode();
    pipelineDescription.VS.BytecodeLength = m_vertexShader.GetBytecodeSize();
    pipelineDescription.PS.pShaderBytecode = m_pixelShader.GetByteCode();
    pipelineDescription.PS.BytecodeLength = m_pixelShader.GetBytecodeSize();
    pipelineDescription.BlendState = blendDescription;
    pipelineDescription.SampleMask = UINT_MAX;
    pipelineDescription.RasterizerState = rasterizerDescription;
    pipelineDescription.DepthStencilState = depthStencilDescription;
    pipelineDescription.InputLayout.pInputElementDescs = inputElements.data();
    pipelineDescription.InputLayout.NumElements = static_cast<UINT>(inputElements.size());
    pipelineDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipelineDescription.NumRenderTargets = 1;
    pipelineDescription.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipelineDescription.SampleDesc.Count = 1;

    result = device->CreateGraphicsPipelineState(
        &pipelineDescription,
        IID_PPV_ARGS(&m_pipelineState)
    );

    if(FAILED(result))
    {
        return false;
    }

    return true;
}

void Pipeline::Shutdown()
{
    m_vertexShader.Unload();
    m_pixelShader.Unload();
    
    if(m_pipelineState){
        m_pipelineState->Release();
        m_pipelineState = nullptr;
    }

    if(m_rootSignature){
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