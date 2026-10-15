#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

// ImGui copies this value. The token binds it to the browser's project/scene
// session; the canonical AssetID reference survives file moves and renames.
struct MaterialDragPayload
{
    std::uint32_t version = 1;
    std::uint32_t reserved = 0;
    std::uint64_t projectIdentity = 0;
    std::array<char, 43> materialReference{};
};
inline constexpr char MaterialDragPayloadType[] = "ENGINE_MATERIAL_ASSET";
static_assert(std::is_trivially_copyable_v<MaterialDragPayload>);
