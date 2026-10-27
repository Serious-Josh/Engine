#pragma once

#include <d3d12.h>
#include <vector>

struct ShaderRecord
{
    const void* shaderIdentifier = nullptr;

    const void* localData = nullptr;
    unsigned int localDataSize = 0;
};

class ShaderTable
{
    public:
    ShaderTable() = default;

    ShaderTable(const ShaderTable&) = delete;
    ShaderTable& operator=(const ShaderTable&) = delete;

    bool Initialize(
        ID3D12Device* device,
        const std::vector<ShaderRecord>& records);
    void Shutdown();
    // ER.1 Phase 4: exchanges ownership with another table (the background pipeline handoff).
    void Swap(ShaderTable& other) noexcept;

    D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress() const;

    unsigned int GetRecordSize() const;
    unsigned int GetSizeInBytes() const;

    private:
    ID3D12Resource* m_resource = nullptr;

    unsigned int m_recordSize = 0;
    unsigned int m_sizeInBytes = 0;
    unsigned int m_recordCount = 0;
};
