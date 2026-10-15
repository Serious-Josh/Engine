#pragma once
#include "assets/AssetID.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace EngineAssetPak
{
    enum class Type : std::uint32_t { Mesh = 1, Texture = 2, Material = 3, Scene = 4, Shader = 5, Resource = 6 };
    using Digest = std::array<std::byte, 32>;
    struct Limits
    {
        std::uint64_t entries = 65536, tocBytes = 16 * 1024 * 1024;
        std::uint64_t payloadBytes = 256 * 1024 * 1024, archiveBytes = 64ull * 1024 * 1024 * 1024;
        std::uint64_t readBytes = 256 * 1024 * 1024;
    };
    struct PreparedAsset
    {
        AssetID id;
        Type type;
        std::uint32_t payloadVersion;
        std::uint64_t generation;
        std::span<const std::byte> bytes;
        std::uint32_t alignment = 16;
    };
    struct Entry
    {
        AssetID id;
        Type type;
        std::uint32_t payloadVersion;
        std::uint64_t generation, offset, storedBytes, uncompressedBytes;
        std::uint32_t alignment;
        Digest sha256;
    };
    // Throws std::runtime_error on invalid metadata, integrity or IO failure;
    // allocation exceptions propagate. Inputs must remain stable during Write.
    // Explicit paths: no filename extension participates in format identity.
    void Write(const std::filesystem::path& toc, const std::filesystem::path& payload,
        std::span<const PreparedAsset> assets, const Limits& limits = {});
    class Reader
    {
    public:
        static Reader Open(const std::filesystem::path& toc, const std::filesystem::path& payload, const Limits& limits = {});
        std::span<const Entry> Entries() const noexcept;
        const Entry* Lookup(const AssetID& id) const noexcept;
        const Digest& Identity() const noexcept;
        std::vector<std::byte> Read(const AssetID& id) const;
    private:
        struct State;
        explicit Reader(std::shared_ptr<State> state) : m_state(std::move(state)) {}
        std::shared_ptr<State> m_state;
    };
}
