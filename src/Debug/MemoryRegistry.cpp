#include "Debug/MemoryRegistry.h"

#include <atomic>
#include <d3d12.h>
#include <new>
#include <type_traits>
#include <wrl/client.h>

// Editor-only implementation; see MemoryRegistry.h for the build boundary.
//
// Lifetime design: all accounting state is a constant-initialized, trivially destructible table of atomics with static
// storage duration. It is never constructed at runtime and never destroyed, so a sentinel released at any point (late in
// Renderer shutdown, during static destruction, or by the D3D12 runtime on another thread) only performs atomic
// arithmetic on storage that stays valid until the process ends. Sentinels hold plain values (category and byte count),
// never a pointer to a registry object or to the tracked resource.

namespace
{
    // {6B1D5C2E-8F43-4A7B-9E06-3D2C71A5B984}
    constexpr GUID MemorySentinelGuid{0x6b1d5c2e, 0x8f43, 0x4a7b, {0x9e, 0x06, 0x3d, 0x2c, 0x71, 0xa5, 0xb9, 0x84}};

    struct CategoryCounters
    {
        std::atomic<std::uint64_t> currentBytes{0};
        std::atomic<std::uint64_t> peakBytes{0};
        std::atomic<std::uint64_t> liveResources{0};
    };

    struct RegistryCounters
    {
        CategoryCounters total;
        CategoryCounters categories[MemoryCategoryCount];
        std::atomic<std::uint64_t> duplicateRegistrations{0};
        std::atomic<std::uint64_t> failedRegistrations{0};
        std::atomic<std::uint64_t> unsizedRegistrations{0};
    };

    static_assert(std::is_trivially_destructible_v<RegistryCounters>,
                  "Registry counters must outlive every sentinel, so they may never run a destructor");

    constinit RegistryCounters g_counters;
    constinit thread_local MemoryCategory g_attribution = MemoryCategory::Unattributed;

    void RaisePeak(std::atomic<std::uint64_t>& peak, std::uint64_t candidate)
    {
        std::uint64_t observed = peak.load(std::memory_order_relaxed);

        while(candidate > observed && !peak.compare_exchange_weak(observed, candidate, std::memory_order_relaxed))
        {
        }
    }

    void AddAllocation(CategoryCounters& counters, std::uint64_t bytes)
    {
        const std::uint64_t current = counters.currentBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        RaisePeak(counters.peakBytes, current);
        counters.liveResources.fetch_add(1, std::memory_order_relaxed);
    }

    void RemoveAllocation(CategoryCounters& counters, std::uint64_t bytes)
    {
        counters.currentBytes.fetch_sub(bytes, std::memory_order_relaxed);
        counters.liveResources.fetch_sub(1, std::memory_order_relaxed);
    }

    MemoryCategoryStatistics Load(const CategoryCounters& counters)
    {
        MemoryCategoryStatistics statistics;
        statistics.currentBytes = counters.currentBytes.load(std::memory_order_relaxed);
        statistics.peakBytes = counters.peakBytes.load(std::memory_order_relaxed);
        statistics.liveResources = counters.liveResources.load(std::memory_order_relaxed);

        return statistics;
    }

    // Owned solely by the resource's private-data store. The registry's creation reference is dropped right after
    // attachment, so the D3D12 runtime's release of private data on resource destruction is the final Release.
    class MemorySentinel final : public IUnknown
    {
        public:
        MemorySentinel(MemoryCategory category, std::uint64_t bytes)
            : m_category(category), m_bytes(bytes)
        {
        }

        // Called only after SetPrivateDataInterface succeeded. A sentinel whose attachment failed is destroyed
        // uncommitted and leaves the counters untouched.
        void Commit()
        {
            AddAllocation(g_counters.total, m_bytes);
            AddAllocation(g_counters.categories[static_cast<std::size_t>(m_category)], m_bytes);
            m_committed = true;
        }

        MemoryCategory GetCategory() const
        {
            return m_category;
        }

        std::uint64_t GetBytes() const
        {
            return m_bytes;
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID identifier, void** object) override
        {
            if(object == nullptr)
            {
                return E_POINTER;
            }

            if(identifier == __uuidof(IUnknown))
            {
                *object = static_cast<IUnknown*>(this);
                AddRef();

                return S_OK;
            }

            *object = nullptr;

            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override
        {
            return m_references.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG references = m_references.fetch_sub(1, std::memory_order_acq_rel) - 1;

            if(references == 0)
            {
                delete this;
            }

            return references;
        }

        private:
        ~MemorySentinel()
        {
            if(m_committed)
            {
                RemoveAllocation(g_counters.categories[static_cast<std::size_t>(m_category)], m_bytes);
                RemoveAllocation(g_counters.total, m_bytes);
            }
        }

        std::atomic<ULONG> m_references{1};
        MemoryCategory m_category;
        std::uint64_t m_bytes;
        bool m_committed = false;
    };

    bool IsAlreadyTracked(ID3D12Resource* resource)
    {
        UINT size = 0;

        return SUCCEEDED(resource->GetPrivateData(MemorySentinelGuid, &size, nullptr));
    }

    // GetPrivateData returns the interface with a reference the caller releases.
    bool ReadSentinel(ID3D12Resource* resource, MemoryResourceRecord& record)
    {
        IUnknown* unknown = nullptr;
        UINT size = sizeof(unknown);

        if(FAILED(resource->GetPrivateData(MemorySentinelGuid, &size, &unknown)) || unknown == nullptr)
        {
            return false;
        }

        // Only TrackResource stores data under MemorySentinelGuid, and it always stores a MemorySentinel.
        const auto* sentinel = static_cast<MemorySentinel*>(unknown);
        record.category = sentinel->GetCategory();
        record.bytes = sentinel->GetBytes();
        unknown->Release();

        return true;
    }

    bool IsCPUVisibleHeap(ID3D12Resource* resource)
    {
        D3D12_HEAP_PROPERTIES properties{};

        // Reserved resources have no heap and report failure; they are never upload or readback memory.
        if(FAILED(resource->GetHeapProperties(&properties, nullptr)))
        {
            return false;
        }

        if(properties.Type == D3D12_HEAP_TYPE_UPLOAD || properties.Type == D3D12_HEAP_TYPE_READBACK)
        {
            return true;
        }

        return properties.Type == D3D12_HEAP_TYPE_CUSTOM &&
               properties.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE &&
               properties.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    }

    MemoryCategory ResolveCategory(ID3D12Resource* resource, MemoryCategory requested)
    {
        // CPU-visible heaps classify by heap type ahead of any requested category, so all staging, constant-upload and
        // readback memory stays in one category whichever subsystem created it.
        if(IsCPUVisibleHeap(resource))
        {
            return MemoryCategory::UploadReadback;
        }

        if(requested != MemoryCategory::Unattributed && requested < MemoryCategory::Count)
        {
            return requested;
        }

        if(g_attribution != MemoryCategory::Unattributed)
        {
            return g_attribution;
        }

        return MemoryCategory::Unattributed;
    }

    // Returns false when D3D12 cannot size the resource; the resource is then counted live with zero bytes.
    bool QueryAllocationBytes(ID3D12Resource* resource, std::uint64_t& bytes)
    {
        bytes = 0;
        Microsoft::WRL::ComPtr<ID3D12Device> device;

        if(FAILED(resource->GetDevice(IID_PPV_ARGS(&device))))
        {
            return false;
        }

        const D3D12_RESOURCE_DESC description = resource->GetDesc();
        const D3D12_RESOURCE_ALLOCATION_INFO information = device->GetResourceAllocationInfo(0, 1, &description);

        if(information.SizeInBytes == 0 || information.SizeInBytes == UINT64_MAX)
        {
            return false;
        }

        bytes = information.SizeInBytes;

        return true;
    }
}

namespace MemoryRegistry
{
    void TrackResource(ID3D12Resource* resource, MemoryCategory category)
    {
        if(resource == nullptr)
        {
            return;
        }

        if(IsAlreadyTracked(resource))
        {
            g_counters.duplicateRegistrations.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const MemoryCategory resolvedCategory = ResolveCategory(resource, category);

        std::uint64_t bytes = 0;
        const bool sized = QueryAllocationBytes(resource, bytes);

        auto* sentinel = new(std::nothrow) MemorySentinel(resolvedCategory, bytes);

        if(sentinel == nullptr)
        {
            g_counters.failedRegistrations.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // On success the resource holds its own reference. Commit while the caller's reference still keeps the
        // resource (and therefore the sentinel) alive, then drop the creation reference. On failure the runtime took no
        // reference, so the Release below destroys the uncommitted sentinel.
        if(SUCCEEDED(resource->SetPrivateDataInterface(MemorySentinelGuid, sentinel)))
        {
            sentinel->Commit();

            if(!sized)
            {
                g_counters.unsizedRegistrations.fetch_add(1, std::memory_order_relaxed);
            }
        }
        else
        {
            g_counters.failedRegistrations.fetch_add(1, std::memory_order_relaxed);
        }

        sentinel->Release();
    }

    MemoryRegistrySnapshot Snapshot()
    {
        MemoryRegistrySnapshot snapshot;
        snapshot.available = true;
        snapshot.total = Load(g_counters.total);

        for(std::size_t index = 0; index < MemoryCategoryCount; ++index)
        {
            snapshot.categories[index] = Load(g_counters.categories[index]);
        }

        snapshot.duplicateRegistrations = g_counters.duplicateRegistrations.load(std::memory_order_relaxed);
        snapshot.failedRegistrations = g_counters.failedRegistrations.load(std::memory_order_relaxed);
        snapshot.unsizedRegistrations = g_counters.unsizedRegistrations.load(std::memory_order_relaxed);

        return snapshot;
    }

    bool IsTracked(ID3D12Resource* resource)
    {
        return resource != nullptr && IsAlreadyTracked(resource);
    }

    std::optional<MemoryResourceRecord> FindTrackedResource(ID3D12Resource* resource)
    {
        MemoryResourceRecord record;

        if(resource == nullptr || !ReadSentinel(resource, record))
        {
            return std::nullopt;
        }

        return record;
    }

    MemoryCategory PushAttribution(MemoryCategory category)
    {
        const MemoryCategory previous = g_attribution;
        g_attribution = category;

        return previous;
    }

    void PopAttribution(MemoryCategory previous)
    {
        g_attribution = previous;
    }
}
