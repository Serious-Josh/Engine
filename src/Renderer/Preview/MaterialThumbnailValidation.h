#pragma once

#include "assets/MaterialAssetData.h"
#include <filesystem>
#include <map>
#include <optional>
#include <chrono>
#include <fstream>
#include <Windows.h>
#include <bcrypt.h>

// CPU-only, bounded memoization. Callers resolve/validate project paths before access.
class MaterialThumbnailValidation
{
public:
    MaterialThumbnailValidation() = default;
    MaterialThumbnailValidation(const MaterialThumbnailValidation&) = delete;
    MaterialThumbnailValidation& operator=(const MaterialThumbnailValidation&) = delete;
    ~MaterialThumbnailValidation();
    std::string Digest(const std::filesystem::path& path);
    struct Snapshot
    {
        MaterialAssetData material;
        std::string digest;
    };
    Snapshot LoadMaterial(const std::filesystem::path& path);
    void Invalidate(const std::filesystem::path& path);
    // At most 256 KiB of periodic verification per pre-recording poll; no worker thread.
    void Poll();
    void Clear();

private:
    struct Metadata
    {
        std::uintmax_t size = 0;
        std::filesystem::file_time_type modified;
        bool operator==(const Metadata&) const = default;
    };
    struct Entry
    {
        Metadata metadata;
        std::string digest;
        std::optional<MaterialAssetData> material;
        std::uint64_t lastUse = 0;
        std::chrono::steady_clock::time_point verifyAfter;
    };
    static Metadata Inspect(const std::filesystem::path& path);
    void CancelVerification();
    static constexpr size_t kCapacity = 256;
    std::map<std::filesystem::path, Entry> m_entries;
    std::uint64_t m_useSerial = 0;
    std::filesystem::path m_verifyingPath;
    Metadata m_verifyingMetadata;
    std::string m_verifyingDigest;
    std::uintmax_t m_verifyOffset = 0;
    BCRYPT_HASH_HANDLE m_hash = nullptr;
};
