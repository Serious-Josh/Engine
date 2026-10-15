#pragma once

#include "assets/ImportedMesh.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace CookedMesh
{
    // Wire values, never C++ object layouts or AssetType enum ordinals.
    namespace V1
    {
        inline constexpr std::array<std::uint8_t, 8> Magic{'E', 'N', 'G', 'M', 'E', 'S', 'H', 0};
        inline constexpr std::uint16_t Version = 1;
        inline constexpr std::uint16_t HeaderBytes = 128;
        inline constexpr std::uint32_t MeshType = 1;

        inline constexpr std::uint32_t VertexLayout = 1;
        inline constexpr std::uint32_t VertexBytes = 48;
        inline constexpr std::uint32_t IndexBytes = 4;
        inline constexpr std::uint32_t SubmeshBytes = 24;

        inline constexpr std::uint32_t MaterialSlotBytes = 24;
        inline constexpr std::uint32_t SectionAlignment = 8;
        inline constexpr std::uint32_t VertexAlignment = 16;
        inline constexpr std::uint32_t SupportedFlags = 0;

        // Flattened static triangles, baked transforms/winding, normalized normals,
        // UV0 with scene FlipV policy already applied, generated tangent xyz/sign.
        // Independent of ModelImporter and its revision numbering.
        inline constexpr std::uint32_t CookCompatibility = 1;
        inline constexpr double UnitLengthSquaredTolerance = 0.001;

        namespace Header
        {
            inline constexpr size_t Magic = 0, Version = 8, HeaderBytes = 10, AssetType = 12;
            inline constexpr size_t PayloadBytes = 16, Flags = 24, VertexLayout = 28;
            inline constexpr size_t VertexCount = 32, IndexCount = 36, SubmeshCount = 40, MaterialSlotCount = 44;
            inline constexpr size_t IndexBytes = 48, CookCompatibility = 52, BoundsMin = 56, BoundsMax = 68;
            inline constexpr size_t Submeshes = 80, MaterialSlots = 88, Strings = 96, Vertices = 104;
            inline constexpr size_t Indices = 112, StringBytes = 120, Reserved = 124;
        }
    }

    struct Policy
    {
        size_t maxPayloadBytes = 160 * 1024 * 1024;
        size_t maxDecodedBytes = 128 * 1024 * 1024;
        size_t maxSubmeshes = 65'536;
        size_t maxMaterialSlots = 4'096;

        size_t maxStringTableBytes = 4 * 1024 * 1024;
        size_t maxStringBytes = 4'096;
    };

    enum class Status
    {
        Success,
        Corrupt,
        Unsupported,
        IncompatibleCook,
        AdmissionLimit,
        AllocationFailure,
        IdentityFailure
    };

    struct Evidence
    {
        std::array<std::uint8_t, 32> payloadSHA256{};
        std::uint64_t payloadBytes = 0;
        std::uint32_t cookCompatibility = 0;
        std::uint16_t formatVersion = 0;

        bool operator==(const Evidence&) const = default;
    };

    struct MetadataSnapshot
    {
        MeshMetadata metadata;
        Vector3 boundsMin{};
        Vector3 boundsMax{};

        std::uint32_t vertexCount = 0;
        std::uint32_t indexCount = 0;
        Evidence evidence;
    };

    struct DecodeResult
    {
        Status status = Status::Corrupt;
        std::string_view message; // Static diagnostic; no allocation on error paths.
        std::shared_ptr<const ImportedMesh> geometry;
        Evidence evidence;
    };

    struct MetadataResult
    {
        Status status = Status::Corrupt;
        std::string_view message;
        std::shared_ptr<const MetadataSnapshot> snapshot;
    };

    struct EncodeResult
    {
        Status status = Status::Corrupt;
        std::string_view message;
        std::vector<std::byte> payload;
        Evidence evidence;
    };

    // Deterministic v1 encoding. Input is unchanged; output owns its bytes.
    // The caller supplies geometry already prepared for CookCompatibility 1.
    EncodeResult Encode(const ImportedMesh& geometry, const Policy& policy);

    // Input must remain stable for the call. Successful outputs own all memory.
    // No filesystem, provider, importer, cache, scene or GPU work occurs here.
    DecodeResult Decode(std::span<const std::byte> payload, const Policy& policy);

    // Validates structure, metadata and bounds, not vertex/index contents. Hashes
    // the complete bounded input for later equality with DecodeResult::evidence.
    MetadataResult DecodeMetadata(std::span<const std::byte> payload, const Policy& policy);
}
