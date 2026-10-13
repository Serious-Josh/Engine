#include "MaterialThumbnailValidation.h"
#include "assets/MaterialAssetSerializer.h"
#include <algorithm>
#include <array>
#include <stdexcept>

#pragma comment(lib, "bcrypt.lib")

namespace
{
    void CreateHash(BCRYPT_HASH_HANDLE& hash)
    {
        if(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        {
            throw std::runtime_error("Cannot create thumbnail validation hash.");
        }
    }

    std::string FinishHash(BCRYPT_HASH_HANDLE hash)
    {
        std::array<unsigned char, 32> bytes{};

        if(BCryptFinishHash(hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0) < 0)
        {
            throw std::runtime_error("Cannot finish thumbnail validation hash.");
        }

        std::string digest;
        constexpr char hex[] = "0123456789abcdef";

        for(auto value : bytes)
        {
            digest += hex[value >> 4];
            digest += hex[value & 15];
        }

        return digest;
    }

    void ReadChunk(std::ifstream& file, BCRYPT_HASH_HANDLE hash)
    {
        std::array<char, 256 * 1024> bytes{};
        file.read(bytes.data(), bytes.size());

        if(file.bad() || (file.fail() && !file.eof()) ||
           BCryptHashData(hash, reinterpret_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(file.gcount()), 0) < 0)
        {
            throw std::runtime_error("Cannot read/hash thumbnail source.");
        }
    }
}

MaterialThumbnailValidation::Metadata MaterialThumbnailValidation::Inspect(const std::filesystem::path& path)
{
    return {std::filesystem::file_size(path), std::filesystem::last_write_time(path)};
}

MaterialThumbnailValidation::~MaterialThumbnailValidation()
{
    CancelVerification();
}

void MaterialThumbnailValidation::CancelVerification()
{
    if(m_hash)
    {
        BCryptDestroyHash(m_hash);
        m_hash = nullptr;
    }

    m_verifyOffset = 0;
    m_verifyingPath.clear();
    m_verifyingDigest.clear();
}

void MaterialThumbnailValidation::Invalidate(const std::filesystem::path& path)
{
    if(path == m_verifyingPath)
    {
        CancelVerification();
    }

    m_entries.erase(path);
}

void MaterialThumbnailValidation::Clear()
{
    CancelVerification();
    m_entries.clear();
    m_useSerial = 0;
}

std::string MaterialThumbnailValidation::Digest(const std::filesystem::path& path)
{
    const auto metadata = Inspect(path);
    auto found = m_entries.find(path);

    if(found != m_entries.end() && found->second.metadata == metadata)
    {
        found->second.lastUse = ++m_useSerial;
        return found->second.digest;
    }

    // Cold/changed sources require a complete digest once. Never cache an unstable read.
    BCRYPT_HASH_HANDLE hash = nullptr;
    CreateHash(hash);
    std::string digest;

    try
    {
        std::ifstream file(path, std::ios::binary);

        if(!file)
        {
            throw std::runtime_error("Cannot open thumbnail source: " + path.string());
        }

        while(!file.eof())
        {
            ReadChunk(file, hash);
        }

        digest = FinishHash(hash);
    }
    catch(...)
    {
        BCryptDestroyHash(hash);
        throw;
    }

    BCryptDestroyHash(hash);

    if(Inspect(path) != metadata)
    {
        throw std::runtime_error("Thumbnail source changed during validation; retry.");
    }

    if(found == m_entries.end() && m_entries.size() >= kCapacity)
    {
        auto victim = std::min_element(m_entries.begin(), m_entries.end(), [](const auto& left, const auto& right)
        {
            return left.second.lastUse < right.second.lastUse;
        });

        if(victim->first == m_verifyingPath)
        {
            CancelVerification();
        }

        m_entries.erase(victim);
    }

    auto& entry = m_entries[path];
    entry = {metadata, digest, std::nullopt, ++m_useSerial,
        std::chrono::steady_clock::now() + std::chrono::seconds(60)};
    return digest;
}

MaterialThumbnailValidation::Snapshot MaterialThumbnailValidation::LoadMaterial(const std::filesystem::path& path)
{
    const auto digest = Digest(path);
    auto& entry = m_entries.at(path);

    if(entry.material)
    {
        return {*entry.material, digest};
    }

    MaterialAssetData material;
    std::string error;

    if(!MaterialAssetSerializer::LoadFromFile(path, material, &error))
    {
        throw std::runtime_error("Cannot load saved thumbnail material: " + error);
    }

    if(Inspect(path) != entry.metadata)
    {
        m_entries.erase(path);
        throw std::runtime_error("Material changed during parsing; retry.");
    }

    entry.material = material;
    return {material, digest};
}

void MaterialThumbnailValidation::Poll()
{
    try
    {
        if(!m_hash)
        {
            const auto now = std::chrono::steady_clock::now();
            auto candidate = m_entries.end();

            for(auto entry = m_entries.begin(); entry != m_entries.end(); ++entry)
            {
                if(entry->second.verifyAfter <= now &&
                   (candidate == m_entries.end() || entry->second.verifyAfter < candidate->second.verifyAfter))
                {
                    candidate = entry;
                }
            }

            if(candidate == m_entries.end())
            {
                return;
            }

            m_verifyingPath = candidate->first;
            m_verifyingMetadata = candidate->second.metadata;
            m_verifyingDigest = candidate->second.digest;
            if(Inspect(m_verifyingPath) != m_verifyingMetadata)
            {
                throw std::runtime_error("Thumbnail periodic verification source changed.");
            }

            CreateHash(m_hash);
        }

        // Do not retain file handles across frames: rename/delete/save must remain possible.
        std::ifstream file(m_verifyingPath, std::ios::binary);

        if(!file || Inspect(m_verifyingPath) != m_verifyingMetadata)
        {
            throw std::runtime_error("Thumbnail periodic verification source changed.");
        }

        file.seekg(static_cast<std::streamoff>(m_verifyOffset));
        ReadChunk(file, m_hash);
        m_verifyOffset += static_cast<std::uintmax_t>(file.gcount());

        if(Inspect(m_verifyingPath) != m_verifyingMetadata)
        {
            throw std::runtime_error("Thumbnail periodic verification source changed.");
        }

        if(file.eof())
        {
            const auto digest = FinishHash(m_hash);
            auto entry = m_entries.find(m_verifyingPath);

            if(entry != m_entries.end() && entry->second.metadata == m_verifyingMetadata &&
               entry->second.digest == m_verifyingDigest && Inspect(m_verifyingPath) == m_verifyingMetadata)
            {
                if(entry->second.digest != digest)
                {
                    entry->second.digest = digest;
                    entry->second.material.reset();
                }

                entry->second.verifyAfter = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            }

            CancelVerification();
        }
    }
    catch(...)
    {
        // Do not use a cached identity after indeterminate verification; normal resolution
        // will report missing/inaccessible sources or perform checked validation again.
        m_entries.erase(m_verifyingPath);
        CancelVerification();
    }
}
