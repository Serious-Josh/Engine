#include "ShaderTable.h"
#include "Core/Log.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace
{
    unsigned int Align(unsigned int value, unsigned int alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }
}

bool ShaderTable::Initialize(ID3D12Device* device, const std::vector<ShaderRecord>& records)
{
    if(!device || records.empty()){
        return false;
    }

    unsigned int maximumLocalDataSize = 0;

    for(const ShaderRecord& record : records){
        if(!record.shaderIdentifier){
            return false;
        }

        if(record.localDataSize > 0 && !record.localData){
            return false;
        }

        maximumLocalDataSize = (std::max)(maximumLocalDataSize, record.localDataSize);
    }

    m_recordSize = Align(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES + maximumLocalDataSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
    m_recordCount = static_cast<unsigned int>(records.size());
    m_sizeInBytes = m_recordSize * m_recordCount;

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC resourceDescription{};
    resourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDescription.Width = m_sizeInBytes;
    resourceDescription.Height = 1;
    resourceDescription.DepthOrArraySize = 1;
    resourceDescription.MipLevels = 1;
    resourceDescription.Format = DXGI_FORMAT_UNKNOWN;
    resourceDescription.SampleDesc.Count = 1;
    resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT result = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_resource));

    if(FAILED(result)){
        Log::Error("Failed to allocate DXR shader-table resource. HRESULT: " + std::to_string(result));
        m_recordSize = 0;
        return false;
    }

    void* mappedData = nullptr;

    result = m_resource->Map(0, nullptr, &mappedData);

    if(FAILED(result)){
        Log::Error("Failed to map DXR shader-table resource. HRESULT: " + std::to_string(result));
        Shutdown();
        return false;
    }

    std::memset(mappedData, 0, m_sizeInBytes);
    
    unsigned char* destination = static_cast<unsigned char*>(mappedData);

    for(const ShaderRecord& record : records){
        std::memcpy(destination, record.shaderIdentifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

        if(record.localDataSize > 0){
            std::memcpy(destination + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, record.localData, record.localDataSize);
        }

        destination += m_recordSize;
    }

    m_resource->Unmap(0, nullptr);

    return true;
}

void ShaderTable::Shutdown()
{
    if(m_resource){
        m_resource->Release();
        m_resource = nullptr;
    }

    m_recordSize = 0;
    m_sizeInBytes = 0;
    m_recordCount = 0;
}

D3D12_GPU_VIRTUAL_ADDRESS ShaderTable::GetGPUAddress() const
{
    if(!m_resource){
        return 0;
    }

    return m_resource->GetGPUVirtualAddress();
}

unsigned int ShaderTable::GetRecordSize() const
{
    return m_recordSize;
}

unsigned int ShaderTable::GetSizeInBytes() const
{
    return m_sizeInBytes;
}
