#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

// Session token bound to a canonical project identity by AssetBrowser. No paths
// or borrowed pointers cross ImGui's copied payload boundary.
struct MeshDragPayload
{
    std::uint32_t version = 1;
    std::uint32_t reserved = 0;
    std::uint64_t projectIdentity = 0;

    std::array<char, 43> meshReference{}; // asset: + canonical UUID + terminator
};

inline constexpr char MeshDragPayloadType[] = "ENGINE_MESH_ASSET";
static_assert(std::is_trivially_copyable_v<MeshDragPayload>);
