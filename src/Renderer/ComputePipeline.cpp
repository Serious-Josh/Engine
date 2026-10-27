#include "ComputePipeline.h"
#include "Debug/TransitionTiming.h"

bool ComputePipeline::Initialize(
    ID3D12Device* device,
    const ComputePipelineDescription& description)
{
    if (!device || !description.computeShader || description.computeShader->GetBytecodeSize() == 0)
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
        // A root SRV (a buffer or acceleration structure address).
        case RootParameterType::ShaderResource:
        {
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            parameter.Descriptor.ShaderRegister = parameterDescription.shaderRegister;
            parameter.Descriptor.RegisterSpace = 0;
            break;
        }
        }

        rootParameters.push_back(parameter);
    }

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDescription{};

    rootSignatureDescription.NumParameters = static_cast<UINT>(rootParameters.size());
    rootSignatureDescription.pParameters = rootParameters.empty() ? nullptr : rootParameters.data();
    rootSignatureDescription.NumStaticSamplers = static_cast<UINT>(description.staticSamplers.size());
    rootSignatureDescription.pStaticSamplers =
        description.staticSamplers.empty() ? nullptr : description.staticSamplers.data();
    rootSignatureDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

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

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription{};

    pipelineDescription.pRootSignature = m_rootSignature;

    pipelineDescription.CS.pShaderBytecode = description.computeShader->GetByteCode();
    pipelineDescription.CS.BytecodeLength = description.computeShader->GetBytecodeSize();

    TransitionTiming::Scope er1Scope("CreateComputePipelineState");
    result = device->CreateComputePipelineState(&pipelineDescription, IID_PPV_ARGS(&m_pipelineState));

    if (FAILED(result))
    {
        Shutdown();
        return false;
    }

    return true;
}

void ComputePipeline::Shutdown()
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

void ComputePipeline::Bind(ID3D12GraphicsCommandList* commandList) const
{
    commandList->SetPipelineState(m_pipelineState);

    commandList->SetComputeRootSignature(m_rootSignature);
}

ID3D12RootSignature* ComputePipeline::GetRootSignature() const
{
    return m_rootSignature;
}
