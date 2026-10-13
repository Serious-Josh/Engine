#pragma once

#include "Renderer/ShaderData.h"
#include <d3d12.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <array>
#include <optional>

// Owned CPU snapshot; texture descriptors remain invalid until texture support exists.
struct MaterialPreviewRequest
{
    std::filesystem::path projectRoot;
    std::string materialIdentity;
    std::uint64_t revision = 0;

    GPUMaterial material{};

    // Temporary Inspector orientation, independent of material/scene persistence.
    float yaw = 0.0f;
    float pitch = 0.0f;

    // Base, normal, roughness, metallic, ARM, AO, height (canonical references).
    std::array<std::optional<std::string>, 7> textures;
    unsigned int dependencyRevision = 0;
};

// Borrowed display descriptor. Renderer owns it; valid only during the current UI frame.
struct MaterialPreviewImage
{
    D3D12_GPU_DESCRIPTOR_HANDLE srv{};
    std::filesystem::path projectRoot;
    std::string materialIdentity;
    std::uint64_t revision = 0;
};
