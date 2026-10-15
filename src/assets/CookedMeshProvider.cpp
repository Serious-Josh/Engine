#include "assets/CookedMeshProvider.h"
#include <atomic>

std::uint64_t CookedMeshSource::NextMappingGeneration() noexcept
{
    static std::atomic_uint64_t next{1};
    auto value = next.load();
    while(value != UINT64_MAX)
    {
        if(next.compare_exchange_weak(value, value + 1)) { return value; }
    }
    return 0;
}
