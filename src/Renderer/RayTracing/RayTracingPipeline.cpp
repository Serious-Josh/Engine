#include "RayTracingPipeline.h"

#include "Renderer/Shader/ShaderLibrary.h"

bool RayTracingPipeline::Initialize(ID3D12Device5* device, ShaderLibrary& shaderLibrary, const std::filesystem::path& shaderPath)
{
    if(!device || shaderPath.empty()){
        return false;
    }

    ShaderLibraryCompileResult compileResult{};
    if(!shaderLibrary.CompileLibrary(shaderPath, compileResult)){
        return false;
    }

    D3D12_EXPORT_DESC exports[3]{};

    exports[0].Name = L"RayGeneration";
    exports[1].Name = L"Miss";
    exports[2].Name = L"ClosestHit";

    D3D12_DXIL_LIBRARY_DESC libraryDescription{};

    D3D12_SHADER_BYTECODE libraryBytecode{};
    libraryBytecode.pShaderBytecode = compileResult.bytecode.data();
    libraryBytecode.BytecodeLength = compileResult.bytecode.size();

    libraryDescription.DXILLibrary = libraryBytecode;
    libraryDescription.NumExports = 3;
    libraryDescription.pExports = exports;

    D3D12_HIT_GROUP_DESC hitGroupDescription{};
    hitGroupDescription.HitGroupExport = L"HitGroup";
    hitGroupDescription.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hitGroupDescription.ClosestHitShaderImport = L"ClosestHit";

    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{};
    shaderConfig.MaxPayloadSizeInBytes = sizeof(float) * 3;
    shaderConfig.MaxAttributeSizeInBytes = sizeof(float) * 2;

    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{};
    pipelineConfig.MaxTraceRecursionDepth = 1;

    D3D12_ROOT_PARAMETER rootParameters[3]{};

    // t0 - TLAS
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParameters[0].Descriptor.ShaderRegister = 0;
    rootParameters[0].Descriptor.RegisterSpace = 0;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // u0 - output texture
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParameters[1].Descriptor.ShaderRegister = 0;
    rootParameters[1].Descriptor.RegisterSpace = 0;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // b0 - camera
    rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[2].Descriptor.ShaderRegister = 0;
    rootParameters[2].Descriptor.RegisterSpace = 0;
    rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDescription{};
    rootSignatureDescription.NumParameters = 3;
    rootSignatureDescription.pParameters = rootParameters;
    rootSignatureDescription.NumStaticSamplers = 0;
    rootSignatureDescription.pStaticSamplers = nullptr;
    rootSignatureDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ID3DBlob* serializedRootSignature = nullptr;
    ID3DBlob* rootSignatureErrors = nullptr;

    HRESULT result = D3D12SerializeRootSignature(&rootSignatureDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serializedRootSignature, &rootSignatureErrors);

    if(FAILED(result)){
        if(rootSignatureErrors){
            rootSignatureErrors->Release();
        }

        return false;
    }

    result = device->CreateRootSignature(0, serializedRootSignature->GetBufferPointer(), serializedRootSignature->GetBufferSize(), IID_PPV_ARGS(&m_globalRootSignature));

    serializedRootSignature->Release();

    if(FAILED(result)){
        return false;
    }

    D3D12_GLOBAL_ROOT_SIGNATURE globalRootSignatureDescription{};
    globalRootSignatureDescription.pGlobalRootSignature = m_globalRootSignature;

    D3D12_STATE_SUBOBJECT subobjects[5]{};
    subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[0].pDesc = &libraryDescription;

    subobjects[1].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    subobjects[1].pDesc = &hitGroupDescription;

    subobjects[2].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    subobjects[2].pDesc = &shaderConfig;

    subobjects[3].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    subobjects[3].pDesc = &pipelineConfig;

    subobjects[4].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    subobjects[4].pDesc = &globalRootSignatureDescription;

    D3D12_STATE_OBJECT_DESC stateObjectDescription{};
    stateObjectDescription.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    stateObjectDescription.NumSubobjects = 5;
    stateObjectDescription.pSubobjects = subobjects;

    result = device->CreateStateObject(&stateObjectDescription, IID_PPV_ARGS(&m_stateObject));

    if(FAILED(result)){
        return false;
    }

    return true;
}

void RayTracingPipeline::Shutdown()
{
    if(m_stateObject){
        m_stateObject->Release();
        m_stateObject = nullptr;
    }

    if(m_globalRootSignature){
        m_globalRootSignature->Release();
        m_globalRootSignature = nullptr;
    }
}