#pragma once

#include <cstdint>
#include <string>
#include <type_traits>

// Copied header followed by objectIDBytes bytes, including the terminating NUL.
// IDs are variable length; scene documents do not require UUID-form object IDs.
struct HierarchyDragIdentity
{
    std::uint64_t sessionEpoch = 0;
    std::uint64_t runtimeIdentity = 0;
    std::uint64_t structureRevision = 0;
    std::uint64_t hierarchyRevision = 0;

    bool operator==(const HierarchyDragIdentity&) const = default;
};

struct HierarchyDragPayload
{
    std::uint32_t version = 2;
    std::uint32_t objectIDBytes = 0;
    HierarchyDragIdentity identity;
};

struct HierarchyReorderRequest
{
    HierarchyDragIdentity identity;
    std::string sourceID;
    std::string destinationID;
    bool insertAfter = false;
};

inline constexpr char HierarchyDragPayloadType[] = "EON_HIERARCHY_OBJECT";
static_assert(std::is_trivially_copyable_v<HierarchyDragPayload>);

enum class HierarchyDropZone { Parent, Before, After, Root };

struct HierarchyDropRequest
{
    HierarchyDragIdentity identity;
    std::string sourceID;
    std::string targetID;
    HierarchyDropZone zone = HierarchyDropZone::Parent;
    bool keepWorld = true;
};
