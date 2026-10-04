#include "RayTracingPipeline.h"

#include "Renderer/Shader/ShaderLibrary.h"

#include <vector>

bool RayTracingPipeline::Initialize(ID3D12Device5* device, ShaderLibrary& shaderLibrary, const RayTracingPipelineDescription& description)
{
    const std::filesystem::path& shaderPath = description.shaderPath;

    if(!device ||
        description.shaderPath.empty() ||
        description.rayGenerationShader.empty() ||
        description.missShaders.empty() ||
        description.hitGroups.empty() ||
        description.rootParameters.empty() ||
        description.maxPayloadSize == 0 ||
        description.maxAttributeSize == 0 ||
        description.maxRecursionDepth == 0)
    {
        return false;
    }


    for(const std::wstring& missShader : description.missShaders){
        if(missShader.empty()){
            return false;
        }
    }

    for(const RayTracingHitGroupDescription& hitGroup : description.hitGroups){
        if(hitGroup.name.empty() || hitGroup.closestHitShader.empty()){
            return false;
        }
    }

    ShaderLibraryCompileResult compileResult{};
    if(!shaderLibrary.CompileLibrary(shaderPath, compileResult)){
        return false;
    }

    const std::wstring& rayGenerationShader = description.rayGenerationShader;

    std::vector<D3D12_EXPORT_DESC> exports;

    auto AddExport =
        [&exports](const std::wstring& name)
        {
            for(const D3D12_EXPORT_DESC& existingExport : exports){
                if(name == existingExport.Name){
                    return;
                }
            }

            D3D12_EXPORT_DESC exportDescription{};
            exportDescription.Name = name.c_str();

            exports.push_back(exportDescription);
        };

    AddExport(description.rayGenerationShader);

    for(const std::wstring& missShader : description.missShaders){
        AddExport(missShader);
    }

    for(const RayTracingHitGroupDescription& hitGroup : description.hitGroups){
        AddExport(hitGroup.closestHitShader);

        if(!hitGroup.anyHitShader.empty()){
            AddExport(hitGroup.anyHitShader);
        }
    }

    D3D12_DXIL_LIBRARY_DESC libraryDescription{};

    D3D12_SHADER_BYTECODE libraryBytecode{};
    libraryBytecode.pShaderBytecode = compileResult.bytecode.data();
    libraryBytecode.BytecodeLength = compileResult.bytecode.size();

    libraryDescription.DXILLibrary = libraryBytecode;
    libraryDescription.NumExports = static_cast<UINT>(exports.size());
    libraryDescription.pExports = exports.data();

    std::vector<D3D12_HIT_GROUP_DESC> hitGroupDescriptions;
    hitGroupDescriptions.reserve(description.hitGroups.size());

    for(const RayTracingHitGroupDescription& hitGroup : description.hitGroups){
        D3D12_HIT_GROUP_DESC hitGroupDescription{};
        hitGroupDescription.HitGroupExport = hitGroup.name.c_str();
        hitGroupDescription.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hitGroupDescription.ClosestHitShaderImport = hitGroup.closestHitShader.c_str();

        if(!hitGroup.anyHitShader.empty()){
            hitGroupDescription.AnyHitShaderImport = hitGroup.anyHitShader.c_str();
        }

        hitGroupDescriptions.push_back(hitGroupDescription);
    }

    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{};
    shaderConfig.MaxPayloadSizeInBytes = description.maxPayloadSize;
    shaderConfig.MaxAttributeSizeInBytes = description.maxAttributeSize;

    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{};
    pipelineConfig.MaxTraceRecursionDepth = description.maxRecursionDepth;

    std::vector<D3D12_ROOT_PARAMETER> rootParameters;
    rootParameters.reserve(description.rootParameters.size());

    std::vector<D3D12_DESCRIPTOR_RANGE> descriptorRanges;
    descriptorRanges.reserve(description.rootParameters.size());

    for(const RootParameterDescription& parameterDescription : description.rootParameters){
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ShaderVisibility = parameterDescription.visibility;

        switch(parameterDescription.type){
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
            case RootParameterType::ShaderResource:
            {
                parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
                parameter.Descriptor.ShaderRegister = parameterDescription.shaderRegister;
                parameter.Descriptor.RegisterSpace = 0;
                break;
            }
            case RootParameterType::DescriptorTable:
            {
                D3D12_DESCRIPTOR_RANGE range{};

                switch(parameterDescription.descriptorRangeType){
                    case DescriptorRangeType::ShaderResource:
                    {
                        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                        break;
                    }
                    case DescriptorRangeType::UnorderedAccess:
                    {
                        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
                        break;
                    }
                }

                range.NumDescriptors = parameterDescription.descriptorCount;
                range.BaseShaderRegister = parameterDescription.shaderRegister;
                range.RegisterSpace = 0;
                range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                
                descriptorRanges.push_back(range);

                parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                parameter.DescriptorTable.NumDescriptorRanges = 1;
                parameter.DescriptorTable.pDescriptorRanges = &descriptorRanges.back();
                break;
            }
        }

        rootParameters.push_back(parameter);
    }

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDescription{};
    rootSignatureDescription.NumParameters = static_cast<UINT>(rootParameters.size());
    rootSignatureDescription.pParameters = rootParameters.empty() ? nullptr : rootParameters.data();
    rootSignatureDescription.NumStaticSamplers = 0;
    rootSignatureDescription.pStaticSamplers = nullptr;
    rootSignatureDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

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

    std::vector<D3D12_STATE_SUBOBJECT> subobjects;
    subobjects.reserve(4 + hitGroupDescriptions.size());

    D3D12_STATE_SUBOBJECT librarySubobject{};
    librarySubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    librarySubobject.pDesc = &libraryDescription;

    subobjects.push_back(librarySubobject);

    for(D3D12_HIT_GROUP_DESC& hitGroupDescription : hitGroupDescriptions){
        D3D12_STATE_SUBOBJECT hitGroupSubobject{};
        hitGroupSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
        hitGroupSubobject.pDesc = &hitGroupDescription;
        subobjects.push_back(hitGroupSubobject);
    }

    D3D12_STATE_SUBOBJECT shaderConfigSubobject{};
    shaderConfigSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    shaderConfigSubobject.pDesc = &shaderConfig;

    subobjects.push_back(shaderConfigSubobject);

    D3D12_STATE_SUBOBJECT pipelineConfigSubobject{};
    pipelineConfigSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    pipelineConfigSubobject.pDesc = &pipelineConfig;

    subobjects.push_back(pipelineConfigSubobject);

    D3D12_STATE_SUBOBJECT rootSignatureSubobject{};
    rootSignatureSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    rootSignatureSubobject.pDesc = &globalRootSignatureDescription;

    subobjects.push_back(rootSignatureSubobject);

    D3D12_STATE_OBJECT_DESC stateObjectDescription{};
    stateObjectDescription.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    stateObjectDescription.NumSubobjects = static_cast<UINT>(subobjects.size());
    stateObjectDescription.pSubobjects = subobjects.data();

    result = device->CreateStateObject(&stateObjectDescription, IID_PPV_ARGS(&m_stateObject));

    if(FAILED(result)){
        return false;
    }

    ID3D12StateObjectProperties* stateObjectProperties = nullptr;

    result = m_stateObject->QueryInterface(IID_PPV_ARGS(&stateObjectProperties));

    if(FAILED(result)){
        Shutdown();
        return false;
    }

    const void* rayGenerationIdentifier = stateObjectProperties->GetShaderIdentifier(rayGenerationShader.c_str());

    if(!rayGenerationIdentifier){
        stateObjectProperties->Release();
        Shutdown();
        return false;
    }

    const std::vector<ShaderRecord> rayGenerationRecords = {{rayGenerationIdentifier}};

    std::vector<ShaderRecord> missRecords;
    missRecords.reserve(description.missShaders.size());

    for(const std::wstring& missShader : description.missShaders){
        const void* identifier = stateObjectProperties->GetShaderIdentifier(missShader.c_str());

        if(!identifier){
            stateObjectProperties->Release();
            Shutdown();
            return false;
        }

        missRecords.push_back({identifier});
    }

    std::vector<ShaderRecord> hitGroupRecords;
    hitGroupRecords.reserve(description.hitGroups.size());

    for(const RayTracingHitGroupDescription& hitGroup : description.hitGroups){
        const void* identifier = stateObjectProperties->GetShaderIdentifier(hitGroup.name.c_str());

        if(!identifier){
            stateObjectProperties->Release();
            Shutdown();
            return false;
        }

        hitGroupRecords.push_back({identifier});
    }

    if(!m_rayGenerationShaderTable.Initialize(device, rayGenerationRecords) ||
       !m_missShaderTable.Initialize(device, missRecords) ||
       !m_hitGroupShaderTable.Initialize(device, hitGroupRecords))
       {
        stateObjectProperties->Release();
        Shutdown();
        return false;
       }

    stateObjectProperties->Release();

        return true;
    }

void RayTracingPipeline::Shutdown()
{
    m_hitGroupShaderTable.Shutdown();
    m_missShaderTable.Shutdown();
    m_rayGenerationShaderTable.Shutdown();

    if(m_stateObject){
        m_stateObject->Release();
        m_stateObject = nullptr;
    }

    if(m_globalRootSignature){
        m_globalRootSignature->Release();
        m_globalRootSignature = nullptr;
    }
}

void RayTracingPipeline::Bind(ID3D12GraphicsCommandList4* commandList) const
{
    if(!commandList){
        return;
    }

    commandList->SetPipelineState1(m_stateObject);
    commandList->SetComputeRootSignature(m_globalRootSignature);
}

void RayTracingPipeline::Dispatch(ID3D12GraphicsCommandList4* commandList, unsigned int width, unsigned int height, unsigned int depth) const
{
    if(!commandList || width == 0 || height == 0 || depth == 0){
        return;
    }

    D3D12_DISPATCH_RAYS_DESC description{};
    description.RayGenerationShaderRecord.StartAddress = m_rayGenerationShaderTable.GetGPUAddress();
    description.RayGenerationShaderRecord.SizeInBytes = m_rayGenerationShaderTable.GetRecordSize();
    description.MissShaderTable.StartAddress = m_missShaderTable.GetGPUAddress();
    description.MissShaderTable.SizeInBytes = m_missShaderTable.GetSizeInBytes();
    description.MissShaderTable.StrideInBytes = m_missShaderTable.GetRecordSize();
    description.HitGroupTable.StartAddress = m_hitGroupShaderTable.GetGPUAddress();
    description.HitGroupTable.SizeInBytes = m_hitGroupShaderTable.GetSizeInBytes();
    description.HitGroupTable.StrideInBytes = m_hitGroupShaderTable.GetRecordSize();

    description.Width = width;
    description.Height = height;
    description.Depth = depth;

    commandList->DispatchRays(&description);
}