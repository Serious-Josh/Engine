#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>

struct ID3D12Resource;

// M0b.1 Editor-only GPU memory accounting.
//
// The registry observes ID3D12Resource lifetimes; it never owns or retains a resource. TrackResource attaches a COM
// sentinel to the resource through SetPrivateDataInterface, and the D3D12 runtime releases that sentinel when the
// resource object is genuinely destroyed. Accounting therefore follows the COM object rather than any C++ wrapper:
// moved wrappers, deferred GPU retirement and explicit Release calls need no extra bookkeeping.
//
// Build boundary: only the Editor executable (Engine) links MemoryRegistry.cpp. Every other consumer of EngineCore,
// including the packaged EngineRuntime, resolves these functions from MemoryRegistryDisabled.cpp, whose bodies are
// empty. Packaged games therefore contain no sentinel type, no allocation-size queries and no registration state.
enum class MemoryCategory : std::uint8_t
{
    CoreRenderer,
    LegacyRayTracing,
    PathTracing,
    DenoisersUpscaling,
    MeshesTextures,
    AccelerationStructures,
    AirVolumetrics,
    VoxelResidency,
    UploadReadback,
    Editor,
    Unattributed,
    Count
};

inline constexpr std::size_t MemoryCategoryCount = static_cast<std::size_t>(MemoryCategory::Count);

struct MemoryCategoryStatistics
{
    // Bytes are D3D12 allocation sizes (GetResourceAllocationInfo), not logical payload sizes.
    std::uint64_t currentBytes = 0;
    std::uint64_t peakBytes = 0;
    std::uint64_t liveResources = 0;
};

struct MemoryRegistrySnapshot
{
    // False in builds that link the disabled registry; every other field is then zero.
    bool available = false;
    MemoryCategoryStatistics total;
    std::array<MemoryCategoryStatistics, MemoryCategoryCount> categories{};
    // Registrations of a resource that already carried a sentinel; ignored without changing counters.
    std::uint64_t duplicateRegistrations = 0;
    // Resources that could not carry a sentinel and are therefore not counted.
    std::uint64_t failedRegistrations = 0;
    // Live resources counted with zero bytes because D3D12 reported an invalid allocation size.
    std::uint64_t unsizedRegistrations = 0;
};

// What a resource's sentinel recorded at registration.
struct MemoryResourceRecord
{
    MemoryCategory category = MemoryCategory::Unattributed;
    std::uint64_t bytes = 0;
};

namespace MemoryRegistry
{
    // Registers a newly created resource. The category resolves in order: UploadReadback for any CPU-visible heap
    // (upload, readback, or a CPU-accessible custom heap), then an explicit category other than Unattributed, then the
    // innermost MemoryCategoryScope on this thread, then Unattributed. Registering a resource again is ignored, so wrappers and owners may both register safely.
    void TrackResource(ID3D12Resource* resource, MemoryCategory category = MemoryCategory::Unattributed);

    // Accepts ComPtr-style owners so direct allocation sites register without unwrapping.
    template<typename ResourcePointer>
        requires requires(const ResourcePointer& pointer)
        {
            { pointer.Get() } -> std::convertible_to<ID3D12Resource*>;
        }
    void TrackResource(const ResourcePointer& resource, MemoryCategory category = MemoryCategory::Unattributed)
    {
        TrackResource(resource.Get(), category);
    }

    MemoryRegistrySnapshot Snapshot();

    // True when the resource carries a live accounting sentinel. Always false in builds with the disabled registry.
    bool IsTracked(ID3D12Resource* resource);

    // The resource's recorded category and bytes, or nothing when it is untracked (always nothing when disabled).
    std::optional<MemoryResourceRecord> FindTrackedResource(ID3D12Resource* resource);

    // Thread-local attribution used when a wrapper is created without an explicit category. Use MemoryCategoryScope.
    MemoryCategory PushAttribution(MemoryCategory category);
    void PopAttribution(MemoryCategory previous);

    constexpr const char* CategoryName(MemoryCategory category)
    {
        switch(category)
        {
            case MemoryCategory::CoreRenderer:
                return "Core Renderer";
            case MemoryCategory::LegacyRayTracing:
                return "Legacy Ray Tracing";
            case MemoryCategory::PathTracing:
                return "Path Tracing";
            case MemoryCategory::DenoisersUpscaling:
                return "Denoisers / Upscaling";
            case MemoryCategory::MeshesTextures:
                return "Meshes / Textures";
            case MemoryCategory::AccelerationStructures:
                return "Acceleration Structures";
            case MemoryCategory::AirVolumetrics:
                return "AIR / Volumetrics";
            case MemoryCategory::VoxelResidency:
                return "Voxel Residency";
            case MemoryCategory::UploadReadback:
                return "Upload / Readback";
            case MemoryCategory::Editor:
                return "Editor";
            case MemoryCategory::Unattributed:
            case MemoryCategory::Count:
                break;
        }

        return "Unattributed";
    }
}

// Attributes wrapper allocations made on this thread to a category until the scope ends. Scopes nest.
class MemoryCategoryScope
{
    public:
    explicit MemoryCategoryScope(MemoryCategory category)
        : m_previous(MemoryRegistry::PushAttribution(category))
    {
    }

    ~MemoryCategoryScope()
    {
        MemoryRegistry::PopAttribution(m_previous);
    }

    MemoryCategoryScope(const MemoryCategoryScope&) = delete;
    MemoryCategoryScope& operator=(const MemoryCategoryScope&) = delete;

    private:
    MemoryCategory m_previous;
};
