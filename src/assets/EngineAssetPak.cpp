#include "assets/EngineAssetPak.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#pragma comment(lib, "bcrypt.lib")

namespace EngineAssetPak
{
namespace
{
constexpr size_t HeaderBytes = 64;
constexpr size_t EntryBytes = 128;
constexpr char TocMagic[] = "ENGTOC01";
constexpr char PakMagic[] = "ENGPAK01";
// Fixed offsets are shared by the TOC reader and writer; no native struct is serialized.
constexpr size_t HeaderVersionOffset = 8;
constexpr size_t HeaderSizeOffset = 12;
constexpr size_t HeaderPayloadBytesOffset = 16;
constexpr size_t HeaderEntryCountOffset = 24;
constexpr size_t HeaderIdentityOffset = 32;
constexpr size_t EntryTypeOffset = 36;
constexpr size_t EntryPayloadVersionOffset = 40;
constexpr size_t EntryAlignmentOffset = 44;
constexpr size_t EntryGenerationOffset = 48;
constexpr size_t EntryPayloadOffset = 56;
constexpr size_t EntryStoredBytesOffset = 64;
constexpr size_t EntryUncompressedBytesOffset = 72;
constexpr size_t EntryDigestOffset = 80;
constexpr size_t EntryReservedOffset = 112;
constexpr size_t EntryEndOffset = 128;

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}
struct File
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~File()
    {
        if (handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle);
        }
    }
    File(const std::filesystem::path& path, bool write = false)
    {
        handle = CreateFileW(path.c_str(), write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ, nullptr,
                             write ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(handle != INVALID_HANDLE_VALUE,
                "Archive file open failed (missing, locked or temporary file already exists).");
    }
    File(const File&) = delete;
    std::uint64_t Size() const
    {
        LARGE_INTEGER size{};
        Require(GetFileSizeEx(handle, &size) && size.QuadPart >= 0, "Archive size query failed.");
        return static_cast<std::uint64_t>(size.QuadPart);
    }
    void Transfer(std::uint64_t offset, std::span<std::byte> bytes, bool write = false)
    {
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        Require(offset <= INT64_MAX && SetFilePointerEx(handle, position, nullptr, FILE_BEGIN),
                "Archive seek failed.");
        while (!bytes.empty())
        {
            DWORD done = 0;
            DWORD count = static_cast<DWORD>(std::min<size_t>(bytes.size(), 1024 * 1024));
            Require(write ? WriteFile(handle, bytes.data(), count, &done, nullptr)
                          : ReadFile(handle, bytes.data(), count, &done, nullptr),
                    "Archive IO failed.");
            Require(done == count, "Truncated archive IO.");
            bytes = bytes.subspan(done);
        }
    }
    void WriteAt(std::uint64_t offset, std::span<const std::byte> bytes)
    {
        Transfer(offset, {const_cast<std::byte*>(bytes.data()), bytes.size()}, true);
    }
};
Digest ComputeDigest(std::span<const std::byte> bytes)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
            "SHA-256 unavailable.");
    struct Close
    {
        BCRYPT_ALG_HANDLE handle;
        ~Close()
        {
            BCryptCloseAlgorithmProvider(handle, 0);
        }
    } close{algorithm};
    Digest result{};
    Require(
        bytes.size() <= ULONG_MAX &&
            BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
                       static_cast<ULONG>(bytes.size()), reinterpret_cast<PUCHAR>(result.data()), 32) >= 0,
        "SHA-256 failed.");
    return result;
}

void WriteUnsigned(std::span<std::byte> bytes, size_t offset, std::uint64_t value, size_t width)
{
    for (size_t byteIndex = 0; byteIndex < width; ++byteIndex)
    {
        bytes[offset + byteIndex] = std::byte((value >> (byteIndex * 8)) & 255);
    }
}

std::uint64_t ReadUnsigned(std::span<const std::byte> bytes, size_t offset, size_t width)
{
    std::uint64_t value = 0;
    for (size_t byteIndex = 0; byteIndex < width; ++byteIndex)
    {
        value |= std::uint64_t(std::to_integer<unsigned char>(bytes[offset + byteIndex])) << (byteIndex * 8);
    }
    return value;
}

bool IsValidAlignment(std::uint64_t alignment)
{
    return alignment && alignment <= 4096 && !(alignment & (alignment - 1));
}

bool IsSupportedType(std::uint64_t type)
{
    return type >= 1 && type <= 6;
}

std::uint64_t AlignOffset(std::uint64_t offset, std::uint64_t alignment)
{
    Require(offset <= UINT64_MAX - (alignment - 1), "Archive offset overflow.");
    return (offset + alignment - 1) & ~(alignment - 1);
}

void CheckHeader(std::span<const std::byte> bytes, const char* magic)
{
    Require(std::equal(bytes.begin(), bytes.begin() + 8, reinterpret_cast<const std::byte*>(magic)),
            "Invalid archive magic.");
    Require(ReadUnsigned(bytes, HeaderVersionOffset, 4) == 1, "Unsupported archive format version.");
    Require(ReadUnsigned(bytes, HeaderSizeOffset, 4) == HeaderBytes, "Invalid archive header size.");
}

void InitializeHeader(std::span<std::byte> bytes, const char* magic, std::uint64_t size, std::uint64_t count)
{
    std::copy_n(reinterpret_cast<const std::byte*>(magic), 8, bytes.begin());
    WriteUnsigned(bytes, HeaderVersionOffset, 1, 4);
    WriteUnsigned(bytes, HeaderSizeOffset, HeaderBytes, 4);
    WriteUnsigned(bytes, HeaderPayloadBytesOffset, size, 8);
    WriteUnsigned(bytes, HeaderEntryCountOffset, count, 8);
}
} // namespace
struct Reader::State
{
    explicit State(const std::filesystem::path& path) : payload(path)
    {
    }
    File payload;
    Limits limits;
    Digest identity;
    std::vector<Entry> entries;
    mutable std::mutex mutex;
};
Reader Reader::Open(const std::filesystem::path& tocPath, const std::filesystem::path& payloadPath,
                    const Limits& limits)
{
    File toc(tocPath);
    auto state = std::make_shared<State>(payloadPath);
    state->limits = limits;
    auto tocBytes = toc.Size();
    auto payloadBytes = state->payload.Size();
    Require(tocBytes >= HeaderBytes && tocBytes <= limits.tocBytes && tocBytes <= SIZE_MAX &&
                tocBytes <= ULONG_MAX,
            "TOC size limit or truncation.");
    Require(payloadBytes >= HeaderBytes && payloadBytes <= limits.archiveBytes && payloadBytes <= INT64_MAX,
            "Payload archive size limit.");

    // Admit both file sizes before allocating and reading the TOC.
    std::vector<std::byte> bytes(static_cast<size_t>(tocBytes));
    toc.Transfer(0, bytes);
    CheckHeader(bytes, TocMagic);
    const auto entryCount = ReadUnsigned(bytes, HeaderEntryCountOffset, 8);
    Require(entryCount <= limits.entries && entryCount <= (tocBytes - HeaderBytes) / EntryBytes &&
                tocBytes == HeaderBytes + entryCount * EntryBytes,
            "Invalid TOC entry count or length.");
    Require(ReadUnsigned(bytes, HeaderPayloadBytesOffset, 8) == payloadBytes, "Archive length mismatch.");

    // The identity hashes the TOC with its identity field zeroed.
    std::copy_n(bytes.begin() + HeaderIdentityOffset, 32, state->identity.begin());
    Require(state->identity != Digest{}, "Invalid archive identity.");
    std::fill_n(bytes.begin() + HeaderIdentityOffset, 32, std::byte{});
    Require(ComputeDigest(bytes) == state->identity, "TOC integrity mismatch.");

    // Only after TOC integrity succeeds, verify the payload belongs to this pair.
    std::array<std::byte, HeaderBytes> header{};
    state->payload.Transfer(0, header);
    CheckHeader(header, PakMagic);
    Require(
        ReadUnsigned(header, HeaderPayloadBytesOffset, 8) == payloadBytes &&
            ReadUnsigned(header, HeaderEntryCountOffset, 8) == entryCount &&
            std::equal(state->identity.begin(), state->identity.end(), header.begin() + HeaderIdentityOffset),
        "TOC/payload pair mismatch.");

    std::uint64_t payloadEnd = HeaderBytes;
    for (size_t index = 0; index < entryCount; ++index)
    {
        const size_t recordOffset = HeaderBytes + index * EntryBytes;
        auto id = AssetID::Parse(std::string(reinterpret_cast<const char*>(bytes.data() + recordOffset), 36));
        Require(id.has_value(), "Invalid asset identity.");
        Require(state->entries.empty() || state->entries.back().id.ToString() < id->ToString(),
                "Duplicate or unsorted asset identity.");
        Require(IsSupportedType(ReadUnsigned(bytes, recordOffset + EntryTypeOffset, 4)),
                "Unsupported archive asset type.");

        Entry entry{
            *id,
            static_cast<Type>(ReadUnsigned(bytes, recordOffset + EntryTypeOffset, 4)),
            static_cast<std::uint32_t>(ReadUnsigned(bytes, recordOffset + EntryPayloadVersionOffset, 4)),
            ReadUnsigned(bytes, recordOffset + EntryGenerationOffset, 8),
            ReadUnsigned(bytes, recordOffset + EntryPayloadOffset, 8),
            ReadUnsigned(bytes, recordOffset + EntryStoredBytesOffset, 8),
            ReadUnsigned(bytes, recordOffset + EntryUncompressedBytesOffset, 8),
            static_cast<std::uint32_t>(ReadUnsigned(bytes, recordOffset + EntryAlignmentOffset, 4)),
            {}};
        std::copy_n(bytes.begin() + recordOffset + EntryDigestOffset, 32, entry.sha256.begin());
        Require(entry.payloadVersion && entry.generation && entry.sha256 != Digest{},
                "Invalid payload version, generation or digest.");
        Require(std::all_of(bytes.begin() + recordOffset + EntryReservedOffset,
                            bytes.begin() + recordOffset + EntryEndOffset,
                            [](auto byte)
                            {
                                return byte == std::byte{};
                            }),
                "Unsupported entry flags/reserved bytes.");

        // Each range starts at the canonical aligned end of the previous entry.
        Require(IsValidAlignment(entry.alignment) && entry.offset == AlignOffset(payloadEnd, entry.alignment),
                "Invalid alignment, overlap or noncanonical offset.");
        Require(entry.storedBytes == entry.uncompressedBytes && entry.storedBytes <= limits.payloadBytes &&
                    entry.offset <= payloadBytes && entry.storedBytes <= payloadBytes - entry.offset,
                "Invalid payload range or size.");
        payloadEnd = entry.offset + entry.storedBytes;
        state->entries.push_back(std::move(entry));
    }
    Require(payloadEnd == payloadBytes, "Invalid archive trailing size.");
    return Reader(std::move(state));
}

std::span<const Entry> Reader::Entries() const noexcept
{
    return m_state->entries;
}

const Digest& Reader::Identity() const noexcept
{
    return m_state->identity;
}

const Entry* Reader::Lookup(const AssetID& id) const noexcept
{
    const auto& entries = m_state->entries;
    auto entryPosition = std::lower_bound(entries.begin(), entries.end(), id.ToString(),
                                          [](const Entry& entry, const std::string& identityText)
                                          {
                                              return entry.id.ToString() < identityText;
                                          });
    return entryPosition != entries.end() && entryPosition->id == id ? &*entryPosition : nullptr;
}

std::vector<std::byte> Reader::Read(const AssetID& id) const
{
    const auto* entry = Lookup(id);
    Require(entry != nullptr, "Asset is absent from archive.");
    Require(entry->storedBytes <= m_state->limits.readBytes && entry->storedBytes <= SIZE_MAX,
            "Payload read allocation limit.");
    std::vector<std::byte> bytes(static_cast<size_t>(entry->storedBytes));
    // Serialize seeks and reads on the shared payload handle; hash after unlocking.
    {
        std::lock_guard lock(m_state->mutex);
        m_state->payload.Transfer(entry->offset, bytes);
    }
    Require(ComputeDigest(bytes) == entry->sha256, "Payload SHA-256 mismatch.");
    return bytes;
}

void Write(const std::filesystem::path& tocPath, const std::filesystem::path& payloadPath,
           std::span<const PreparedAsset> assets, const Limits& limits)
{
    Require(std::filesystem::absolute(tocPath).lexically_normal() !=
                std::filesystem::absolute(payloadPath).lexically_normal(),
            "Archive paths must differ.");
    Require(assets.size() <= limits.entries && limits.tocBytes >= HeaderBytes &&
                assets.size() <= (limits.tocBytes - HeaderBytes) / EntryBytes &&
                assets.size() <= (ULONG_MAX - HeaderBytes) / EntryBytes,
            "TOC admission limit.");

    // Sort borrowed inputs before encoding records and calculating payload positions.
    std::vector<const PreparedAsset*> sorted;
    sorted.reserve(assets.size());
    for (const auto& asset : assets)
    {
        sorted.push_back(&asset);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](auto leftAsset, auto rightAsset)
              {
                  return leftAsset->id.ToString() < rightAsset->id.ToString();
              });
    std::vector<std::byte> toc(HeaderBytes + sorted.size() * EntryBytes);
    std::uint64_t payloadBytes = HeaderBytes;
    for (size_t index = 0; index < sorted.size(); ++index)
    {
        const auto& asset = *sorted[index];
        Require(!index || sorted[index - 1]->id != asset.id, "Duplicate asset identity.");
        Require(IsSupportedType(static_cast<std::uint32_t>(asset.type)) && asset.payloadVersion &&
                    asset.generation && IsValidAlignment(asset.alignment),
                "Invalid prepared asset metadata.");
        Require(asset.bytes.size() <= limits.payloadBytes && asset.bytes.size() <= ULONG_MAX,
                "Payload admission limit.");
        payloadBytes = AlignOffset(payloadBytes, asset.alignment);
        Require(payloadBytes <= limits.archiveBytes &&
                    asset.bytes.size() <= limits.archiveBytes - payloadBytes &&
                    payloadBytes <= INT64_MAX - asset.bytes.size(),
                "Archive size limit.");

        size_t recordOffset = HeaderBytes + index * EntryBytes;
        std::copy_n(reinterpret_cast<const std::byte*>(asset.id.ToString().data()), 36,
                    toc.begin() + recordOffset);
        WriteUnsigned(toc, recordOffset + EntryTypeOffset, static_cast<std::uint32_t>(asset.type), 4);
        WriteUnsigned(toc, recordOffset + EntryPayloadVersionOffset, asset.payloadVersion, 4);
        WriteUnsigned(toc, recordOffset + EntryAlignmentOffset, asset.alignment, 4);
        WriteUnsigned(toc, recordOffset + EntryGenerationOffset, asset.generation, 8);
        WriteUnsigned(toc, recordOffset + EntryPayloadOffset, payloadBytes, 8);
        WriteUnsigned(toc, recordOffset + EntryStoredBytesOffset, asset.bytes.size(), 8);
        WriteUnsigned(toc, recordOffset + EntryUncompressedBytesOffset, asset.bytes.size(), 8);
        const auto digest = ComputeDigest(asset.bytes);
        std::copy(digest.begin(), digest.end(), toc.begin() + recordOffset + EntryDigestOffset);
        payloadBytes += asset.bytes.size();
    }
    Require(payloadBytes <= limits.archiveBytes, "Archive size limit.");

    // Zero-initialized TOC reserved/identity bytes participate in this digest.
    InitializeHeader(toc, TocMagic, payloadBytes, sorted.size());
    const auto identity = ComputeDigest(toc);
    std::copy(identity.begin(), identity.end(), toc.begin() + HeaderIdentityOffset);
    std::array<std::byte, HeaderBytes> header{};
    InitializeHeader(header, PakMagic, payloadBytes, sorted.size());
    std::copy(identity.begin(), identity.end(), header.begin() + HeaderIdentityOffset);

    auto tocTemp = tocPath;
    tocTemp += L".writing";
    auto pakTemp = payloadPath;
    pakTemp += L".writing";
    // CREATE_NEW refuses concurrent/stale temporary outputs. Never delete files we did not create.
    bool ownToc = false;
    bool ownPak = false;
    try
    {
        {
            File pak(pakTemp, true);
            ownPak = true;
            File tocFile(tocTemp, true);
            ownToc = true;
            pak.WriteAt(0, header);
            std::uint64_t writtenEnd = HeaderBytes;
            std::array<std::byte, 4096> zeros{};
            for (size_t index = 0; index < sorted.size(); ++index)
            {
                auto offset = ReadUnsigned(toc, HeaderBytes + index * EntryBytes + EntryPayloadOffset, 8);
                pak.WriteAt(writtenEnd, std::span<const std::byte>(zeros).first(
                                            static_cast<size_t>(offset - writtenEnd)));
                pak.WriteAt(offset, sorted[index]->bytes);
                writtenEnd = offset + sorted[index]->bytes.size();
            }
            tocFile.WriteAt(0, toc);
            Require(FlushFileBuffers(pak.handle) && FlushFileBuffers(tocFile.handle),
                    "Archive flush failed.");
        }
        // Preserve payload-first publication and the existing failure/cleanup semantics.
        Require(MoveFileExW(pakTemp.c_str(), payloadPath.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
                "Payload publication failed.");
        ownPak = false;
        Require(
            MoveFileExW(tocTemp.c_str(), tocPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
            "TOC publication failed; pair unavailable until retry.");
        ownToc = false;
    }
    catch (...)
    {
        if (ownToc)
        {
            DeleteFileW(tocTemp.c_str());
        }
        if (ownPak)
        {
            DeleteFileW(pakTemp.c_str());
        }
        throw;
    }
}
} // namespace EngineAssetPak
