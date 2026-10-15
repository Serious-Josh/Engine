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
    constexpr size_t Header = 64, Record = 128;
    constexpr char TocMagic[] = "ENGTOC01", PakMagic[] = "ENGPAK01";
    void Require(bool condition, const char* message) { if(!condition) throw std::runtime_error(message); }
    struct File
    {
        HANDLE handle = INVALID_HANDLE_VALUE;
        ~File() { if(handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
        File(const std::filesystem::path& path, bool write = false)
        {
            handle = CreateFileW(path.c_str(), write ? GENERIC_WRITE : GENERIC_READ,
                FILE_SHARE_READ, nullptr, write ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            Require(handle != INVALID_HANDLE_VALUE, "Archive file open failed (missing, locked or temporary file already exists).");
        }
        File(const File&) = delete;
        std::uint64_t Size() const
        {
            LARGE_INTEGER n{};
            Require(GetFileSizeEx(handle, &n) && n.QuadPart >= 0, "Archive size query failed.");
            return static_cast<std::uint64_t>(n.QuadPart);
        }
        void Transfer(std::uint64_t offset, std::span<std::byte> bytes, bool write = false)
        {
            LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
            Require(offset <= INT64_MAX && SetFilePointerEx(handle, position, nullptr, FILE_BEGIN), "Archive seek failed.");
            while(!bytes.empty())
            {
                DWORD done = 0, count = static_cast<DWORD>(std::min<size_t>(bytes.size(), 1024 * 1024));
                Require(write ? WriteFile(handle, bytes.data(), count, &done, nullptr) : ReadFile(handle, bytes.data(), count, &done, nullptr), "Archive IO failed.");
                Require(done == count, "Truncated archive IO."); bytes = bytes.subspan(done);
            }
        }
        void Put(std::uint64_t offset, std::span<const std::byte> bytes)
        { Transfer(offset, {const_cast<std::byte*>(bytes.data()), bytes.size()}, true); }
    };
    Digest Hash(std::span<const std::byte> bytes)
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "SHA-256 unavailable.");
        struct Close { BCRYPT_ALG_HANDLE h; ~Close() { BCryptCloseAlgorithmProvider(h, 0); } } close{algorithm};
        Digest result{};
        Require(bytes.size() <= ULONG_MAX && BCryptHash(algorithm, nullptr, 0,
            reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())), static_cast<ULONG>(bytes.size()),
            reinterpret_cast<PUCHAR>(result.data()), 32) >= 0, "SHA-256 failed.");
        return result;
    }
    void Put(std::span<std::byte> b, size_t o, std::uint64_t v, size_t n)
    { for(size_t i = 0; i < n; ++i) b[o+i] = std::byte((v >> (i*8)) & 255); }
    std::uint64_t Get(std::span<const std::byte> b, size_t o, size_t n)
    { std::uint64_t v = 0; for(size_t i = 0; i < n; ++i) v |= std::uint64_t(std::to_integer<unsigned char>(b[o+i])) << (i*8); return v; }
    bool Alignment(std::uint64_t n) { return n && n <= 4096 && !(n & (n-1)); }
    bool Supported(std::uint64_t n) { return n >= 1 && n <= 6; }
    std::uint64_t Align(std::uint64_t n, std::uint64_t a)
    { Require(n <= UINT64_MAX - (a-1), "Archive offset overflow."); return (n+a-1) & ~(a-1); }
    void CheckHeader(std::span<const std::byte> b, const char* magic)
    {
        Require(std::equal(b.begin(), b.begin()+8, reinterpret_cast<const std::byte*>(magic)), "Invalid archive magic.");
        Require(Get(b,8,4) == 1, "Unsupported archive format version.");
        Require(Get(b,12,4) == Header, "Invalid archive header size.");
    }
    void Init(std::span<std::byte> b, const char* magic, std::uint64_t size, std::uint64_t count)
    {
        std::copy_n(reinterpret_cast<const std::byte*>(magic), 8, b.begin());
        Put(b,8,1,4); Put(b,12,Header,4); Put(b,16,size,8); Put(b,24,count,8);
    }
}
struct Reader::State
{
    explicit State(const std::filesystem::path& path) : payload(path) {}
    File payload;
    Limits limits;
    Digest identity;
    std::vector<Entry> entries;
    mutable std::mutex mutex;
};
Reader Reader::Open(const std::filesystem::path& tocPath, const std::filesystem::path& payloadPath, const Limits& limits)
{
    File toc(tocPath);
    auto state = std::make_shared<State>(payloadPath);
    state->limits = limits;
    auto size = toc.Size(), total = state->payload.Size();
    Require(size >= Header && size <= limits.tocBytes && size <= SIZE_MAX && size <= ULONG_MAX, "TOC size limit or truncation.");
    Require(total >= Header && total <= limits.archiveBytes && total <= INT64_MAX, "Payload archive size limit.");
    std::vector<std::byte> bytes(static_cast<size_t>(size)); toc.Transfer(0,bytes);
    CheckHeader(bytes,TocMagic);
    const auto count = Get(bytes,24,8);
    Require(count <= limits.entries && count <= (size-Header)/Record && size == Header+count*Record, "Invalid TOC entry count or length.");
    Require(Get(bytes,16,8) == total, "Archive length mismatch.");
    std::copy_n(bytes.begin()+32,32,state->identity.begin());
    Require(state->identity != Digest{}, "Invalid archive identity.");
    std::fill_n(bytes.begin()+32,32,std::byte{});
    Require(Hash(bytes) == state->identity, "TOC integrity mismatch.");
    std::array<std::byte,Header> header{}; state->payload.Transfer(0,header);
    CheckHeader(header,PakMagic);
    Require(Get(header,16,8) == total && Get(header,24,8) == count && std::equal(state->identity.begin(),state->identity.end(),header.begin()+32), "TOC/payload pair mismatch.");
    std::uint64_t end = Header;
    for(size_t i = 0; i < count; ++i)
    {
        const size_t o = Header+i*Record;
        auto id = AssetID::Parse(std::string(reinterpret_cast<const char*>(bytes.data()+o),36));
        Require(id.has_value(), "Invalid asset identity.");
        Require(state->entries.empty() || state->entries.back().id.ToString() < id->ToString(), "Duplicate or unsorted asset identity.");
        Require(Supported(Get(bytes,o+36,4)), "Unsupported archive asset type.");
        Entry e{*id,static_cast<Type>(Get(bytes,o+36,4)),static_cast<std::uint32_t>(Get(bytes,o+40,4)),Get(bytes,o+48,8),Get(bytes,o+56,8),Get(bytes,o+64,8),Get(bytes,o+72,8),static_cast<std::uint32_t>(Get(bytes,o+44,4)),{}};
        std::copy_n(bytes.begin()+o+80,32,e.sha256.begin());
        Require(e.payloadVersion && e.generation && e.sha256 != Digest{}, "Invalid payload version, generation or digest.");
        Require(std::all_of(bytes.begin()+o+112,bytes.begin()+o+128,[](auto b){ return b == std::byte{}; }), "Unsupported entry flags/reserved bytes.");
        Require(Alignment(e.alignment) && e.offset == Align(end,e.alignment), "Invalid alignment, overlap or noncanonical offset.");
        Require(e.storedBytes == e.uncompressedBytes && e.storedBytes <= limits.payloadBytes && e.offset <= total && e.storedBytes <= total-e.offset, "Invalid payload range or size.");
        end = e.offset+e.storedBytes; state->entries.push_back(std::move(e));
    }
    Require(end == total, "Invalid archive trailing size.");
    return Reader(std::move(state));
}
std::span<const Entry> Reader::Entries() const noexcept { return m_state->entries; }
const Digest& Reader::Identity() const noexcept { return m_state->identity; }
const Entry* Reader::Lookup(const AssetID& id) const noexcept
{
    const auto& entries = m_state->entries;
    auto i = std::lower_bound(entries.begin(),entries.end(),id.ToString(),[](const Entry& e,const std::string& s){ return e.id.ToString() < s; });
    return i != entries.end() && i->id == id ? &*i : nullptr;
}
std::vector<std::byte> Reader::Read(const AssetID& id) const
{
    const auto* e = Lookup(id); Require(e != nullptr, "Asset is absent from archive.");
    Require(e->storedBytes <= m_state->limits.readBytes && e->storedBytes <= SIZE_MAX, "Payload read allocation limit.");
    std::vector<std::byte> bytes(static_cast<size_t>(e->storedBytes));
    { std::lock_guard lock(m_state->mutex); m_state->payload.Transfer(e->offset,bytes); }
    Require(Hash(bytes) == e->sha256, "Payload SHA-256 mismatch."); return bytes;
}
void Write(const std::filesystem::path& tocPath, const std::filesystem::path& payloadPath, std::span<const PreparedAsset> assets, const Limits& limits)
{
    Require(std::filesystem::absolute(tocPath).lexically_normal() != std::filesystem::absolute(payloadPath).lexically_normal(), "Archive paths must differ.");
    Require(assets.size() <= limits.entries && limits.tocBytes >= Header && assets.size() <= (limits.tocBytes-Header)/Record && assets.size() <= (ULONG_MAX-Header)/Record, "TOC admission limit.");
    std::vector<const PreparedAsset*> sorted; sorted.reserve(assets.size());
    for(const auto& a : assets) sorted.push_back(&a);
    std::sort(sorted.begin(),sorted.end(),[](auto a,auto b){ return a->id.ToString() < b->id.ToString(); });
    std::vector<std::byte> toc(Header+sorted.size()*Record);
    std::uint64_t total = Header;
    for(size_t i = 0; i < sorted.size(); ++i)
    {
        const auto& a = *sorted[i];
        Require(!i || sorted[i-1]->id != a.id, "Duplicate asset identity.");
        Require(Supported(static_cast<std::uint32_t>(a.type)) && a.payloadVersion && a.generation && Alignment(a.alignment), "Invalid prepared asset metadata.");
        Require(a.bytes.size() <= limits.payloadBytes && a.bytes.size() <= ULONG_MAX, "Payload admission limit.");
        total = Align(total,a.alignment);
        Require(total <= limits.archiveBytes && a.bytes.size() <= limits.archiveBytes-total && total <= INT64_MAX-a.bytes.size(), "Archive size limit.");
        size_t o = Header+i*Record;
        std::copy_n(reinterpret_cast<const std::byte*>(a.id.ToString().data()),36,toc.begin()+o);
        Put(toc,o+36,static_cast<std::uint32_t>(a.type),4); Put(toc,o+40,a.payloadVersion,4); Put(toc,o+44,a.alignment,4);
        Put(toc,o+48,a.generation,8); Put(toc,o+56,total,8); Put(toc,o+64,a.bytes.size(),8); Put(toc,o+72,a.bytes.size(),8);
        const auto digest = Hash(a.bytes); std::copy(digest.begin(),digest.end(),toc.begin()+o+80);
        total += a.bytes.size();
    }
    Require(total <= limits.archiveBytes, "Archive size limit.");
    Init(toc,TocMagic,total,sorted.size()); const auto identity = Hash(toc);
    std::copy(identity.begin(),identity.end(),toc.begin()+32);
    std::array<std::byte,Header> header{}; Init(header,PakMagic,total,sorted.size()); std::copy(identity.begin(),identity.end(),header.begin()+32);
    auto tocTemp = tocPath; tocTemp += L".writing";
    auto pakTemp = payloadPath; pakTemp += L".writing";
    // CREATE_NEW refuses concurrent/stale temporary outputs. Never delete files we did not create.
    bool ownToc = false, ownPak = false;
    try
    {
        {
            File pak(pakTemp,true); ownPak = true; File index(tocTemp,true); ownToc = true;
            pak.Put(0,header); std::uint64_t end = Header;
            std::array<std::byte,4096> zeros{};
            for(size_t i = 0; i < sorted.size(); ++i)
            {
                auto offset = Get(toc,Header+i*Record+56,8);
                pak.Put(end,std::span<const std::byte>(zeros).first(static_cast<size_t>(offset-end)));
                pak.Put(offset,sorted[i]->bytes); end = offset+sorted[i]->bytes.size();
            }
            index.Put(0,toc);
            Require(FlushFileBuffers(pak.handle) && FlushFileBuffers(index.handle), "Archive flush failed.");
        }
        Require(MoveFileExW(pakTemp.c_str(),payloadPath.c_str(),MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "Payload publication failed."); ownPak = false;
        Require(MoveFileExW(tocTemp.c_str(),tocPath.c_str(),MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "TOC publication failed; pair unavailable until retry."); ownToc = false;
    }
    catch(...)
    {
        if(ownToc) DeleteFileW(tocTemp.c_str());
        if(ownPak) DeleteFileW(pakTemp.c_str());
        throw;
    }
}
}
