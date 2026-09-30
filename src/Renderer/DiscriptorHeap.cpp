#include "DescriptorHeap.h"
#include "Core/Log.h"

#include <climits>
#include <utility>

DescriptorHeap::DescriptorHeap(DescriptorHeap&& other) noexcept
    : m_heap(other.m_heap)
    , m_type(other.m_type)
    , m_descriptorSize(other.m_descriptorSize)
    , m_capacity(other.m_capacity)
    , m_shaderVisible(other.m_shaderVisible)
    , m_freeIndices(std::move(other.m_freeIndices))
{
    other.m_heap = nullptr;
    other.m_descriptorSize = 0;
    other.m_capacity = 0;
}

DescriptorHeap& DescriptorHeap::operator=(DescriptorHeap&& other) noexcept
{
    if(this != &other)
    {
        Shutdown();

        m_heap = other.m_heap;
        m_type = other.m_type;
        m_descriptorSize = other.m_descriptorSize;
        m_capacity = other.m_capacity;
        m_shaderVisible = other.m_shaderVisible;
        m_freeIndices = std::move(other.m_freeIndices);

        other.m_heap = nullptr;
        other.m_descriptorSize = 0;
        other.m_capacity = 0;
    }

    return *this;
}

bool DescriptorHeap::Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, unsigned int capacity, bool shaderVisible)
{
    m_type = type;
    m_capacity = capacity;
    m_shaderVisible = shaderVisible;

    D3D12_DESCRIPTOR_HEAP_DESC description{};
    description.Type = type;
    description.NumDescriptors = capacity;
    description.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    HRESULT result = device-> CreateDescriptorHeap(&description, IID_PPV_ARGS(&m_heap));

    if(FAILED(result)){
        m_heap = nullptr;
        return false;
    }

    m_descriptorSize = device->GetDescriptorHandleIncrementSize(type);

    m_freeIndices.clear();
    m_freeIndices.reserve(m_capacity);

    for(unsigned int i = m_capacity; i > 0; --i){
        m_freeIndices.push_back(i - 1);
    }

    return true;
}

void DescriptorHeap::Shutdown()
{
    if(m_heap){
        m_heap->Release();
        m_heap = nullptr;
    }

    m_freeIndices.clear();
    m_descriptorSize = 0;
    m_capacity = 0;
}

unsigned int DescriptorHeap::Allocate()
{

    if(m_freeIndices.empty())
    {
        Log::Error("Descriptor heap is full.");
        return UINT_MAX;
    }

    unsigned int index = m_freeIndices.back();
    m_freeIndices.pop_back();


    return index;
}

void DescriptorHeap::Free(unsigned int index)
{
    if(index >= m_capacity){
        return;
    }

    m_freeIndices.push_back(index);
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::GetCPUHandle(unsigned int index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_heap->GetCPUDescriptorHandleForHeapStart();

    handle.ptr += static_cast<SIZE_T>(index) * m_descriptorSize;

    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::GetGPUHandle(unsigned int index) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_heap->GetGPUDescriptorHandleForHeapStart();

    handle.ptr += static_cast<UINT64>(index) * m_descriptorSize;

    return handle;
}

ID3D12DescriptorHeap* DescriptorHeap::GetHeap() const
{
    return m_heap;
}

unsigned int DescriptorHeap::GetIndex(D3D12_CPU_DESCRIPTOR_HANDLE handle) const
{
    if(!m_heap || m_descriptorSize == 0){
        return UINT_MAX;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE start = m_heap->GetCPUDescriptorHandleForHeapStart();

    if(handle.ptr < start.ptr){
        return UINT_MAX;
    }

    SIZE_T offset = handle.ptr - start.ptr;

    if(offset % m_descriptorSize != 0){
        return UINT_MAX;
    }

    unsigned int index = static_cast<unsigned int>(offset / m_descriptorSize);

    if(index >= m_capacity){
        return UINT_MAX;
    }

    return index;
}