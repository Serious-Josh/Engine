#include "assets/ArchiveCookedMeshProvider.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <map>
#include <stdexcept>
#include <system_error>

namespace CookedMeshSource
{
namespace
{
    template<class T, class F> T Guard(F function)
    {
        try { return function(); }
        catch(const std::bad_alloc&) { return {{Status::AllocationFailure, "Archive provider allocation failed."}}; }
        catch(const std::length_error&) { return {{Status::AdmissionLimit, "Archive provider allocation limit."}}; }
        catch(const std::system_error&) { return {{Status::IOFailure, "Archive provider filesystem/handle operation failed."}}; }
        catch(const std::runtime_error&) { return {{Status::Corrupt, "Archive structure, pair or payload integrity validation failed."}}; }
    }
    struct TocPin
    {
        HANDLE handle = INVALID_HANDLE_VALUE;
        explicit TocPin(const std::filesystem::path& path)
        {
            handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if(handle == INVALID_HANDLE_VALUE) throw std::system_error(GetLastError(),std::system_category());
        }
        ~TocPin() { if(handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
        TocPin(const TocPin&) = delete;
    };
    std::string Hex(const EngineAssetPak::Digest& digest)
    {
        std::string result; result.reserve(64);
        constexpr char hex[] = "0123456789abcdef";
        for(auto b : digest) { auto v = std::to_integer<unsigned char>(b); result += hex[v>>4]; result += hex[v&15]; }
        return result;
    }
    Outcome Decoded(CookedMesh::Status status, std::string_view message)
    {
        switch(status)
        {
            case CookedMesh::Status::Success: return {};
            case CookedMesh::Status::Corrupt: return {Status::Corrupt,message};
            case CookedMesh::Status::Unsupported: return {Status::Unsupported,message};
            case CookedMesh::Status::IncompatibleCook: return {Status::IncompatibleCook,message};
            case CookedMesh::Status::AdmissionLimit: return {Status::AdmissionLimit,message};
            case CookedMesh::Status::AllocationFailure: return {Status::AllocationFailure,message};
            case CookedMesh::Status::IdentityFailure: return {Status::IdentityFailure,message};
        }
        return {Status::Corrupt,"Unknown mesh validation outcome."};
    }
}
struct ArchiveProvider::State
{
    State(const std::filesystem::path& tocPath, const std::filesystem::path& payloadPath, const ArchivePolicy& p)
        : toc(tocPath), reader(EngineAssetPak::Reader::Open(tocPath,payloadPath,p.archive)), policy(p) {}
    TocPin toc; // Established before Reader::Open; excludes TOC replacement races.
    EngineAssetPak::Reader reader;
    ArchivePolicy policy;
    MappingEvidence mapping;
    std::map<std::string,std::shared_ptr<const AssetEvidence>> assets;
};
ArchiveOpenResult ArchiveProvider::Open(const std::filesystem::path& projectRoot,
    const std::filesystem::path& tocPath, const std::filesystem::path& payloadPath, const ArchivePolicy& policy)
{
    return Guard<ArchiveOpenResult>([&]() -> ArchiveOpenResult
    {
        if(tocPath.empty() || payloadPath.empty()) return {{Status::IOFailure,"Archive source requires explicit project and pair paths."}};
        if(!std::filesystem::exists(tocPath) || !std::filesystem::exists(payloadPath)) return {{Status::Missing,"Archive pair is missing."}};
        auto state = std::make_shared<State>(tocPath,payloadPath,policy);
        const auto identity = Hex(state->reader.Identity());
        state->mapping = {std::filesystem::canonical(tocPath).generic_string(),identity,identity,NextMappingGeneration(),
            projectRoot.empty() ? std::string{} : std::filesystem::canonical(projectRoot).generic_string()};
        if(!state->mapping.mappingGeneration) return {{Status::AdmissionLimit,"Provider mapping tokens exhausted."}};
        for(const auto& entry : state->reader.Entries())
        {
            if(entry.type != EngineAssetPak::Type::Mesh || entry.payloadVersion != CookedMesh::V1::Version) continue;
            if(entry.storedBytes < CookedMesh::V1::HeaderBytes) return {{Status::Corrupt,"Archived mesh is smaller than its header."}};
            if(entry.storedBytes > policy.mesh.maxPayloadBytes || entry.storedBytes > policy.archive.readBytes)
                return {{Status::AdmissionLimit,"Archived mesh exceeds provider read admission."}};
            CookedMesh::Evidence payload{};
            for(size_t i = 0; i < 32; ++i) payload.payloadSHA256[i] = std::to_integer<std::uint8_t>(entry.sha256[i]);
            payload.payloadBytes = entry.storedBytes;
            // Expected v1 compatibility, proved against actual mesh header on Acquire.
            payload.formatVersion = CookedMesh::V1::Version; payload.cookCompatibility = CookedMesh::V1::CookCompatibility;
            auto evidence = std::make_shared<AssetEvidence>(AssetEvidence{entry.id,AssetType::Mesh,payload,state->mapping,
                "engine-archive-cooked-mesh-v1",{},{},entry.generation});
            state->assets.emplace(entry.id.ToString(),std::move(evidence));
        }
        return {{},std::shared_ptr<const ArchiveProvider>(new ArchiveProvider(std::move(state)))};
    });
}
ArchiveOpenResult ArchiveProvider::OpenArchive(const std::filesystem::path& toc, const std::filesystem::path& payload, const ArchivePolicy& policy)
{ return Open({},toc,payload,policy); }
const EngineAssetPak::Entry* ArchiveProvider::ArchiveEntry(const AssetID& id) const noexcept { return m_state->reader.Lookup(id); }
std::vector<std::byte> ArchiveProvider::ReadEntry(const AssetID& id, EngineAssetPak::Type type, std::uint32_t version, size_t maxBytes) const
{
    const auto* entry = ArchiveEntry(id);
    if(!entry || entry->type != type || entry->payloadVersion != version || entry->storedBytes > maxBytes)
        throw std::runtime_error("Archive entry missing, unsupported or exceeds consumer admission.");
    return m_state->reader.Read(id);
}
const MappingEvidence& ArchiveProvider::Mapping() const noexcept { return m_state->mapping; }
size_t ArchiveProvider::EntryCount() const noexcept { return m_state->assets.size(); }
std::span<const EngineAssetPak::Entry> ArchiveProvider::Entries() const noexcept { return m_state->reader.Entries(); }
LookupResult ArchiveProvider::Lookup(const AssetID& asset) const
{
    const auto* entry = m_state->reader.Lookup(asset);
    if(!entry) return {{Status::Missing,"Mesh AssetID is absent from the archive."}};
    if(entry->type != EngineAssetPak::Type::Mesh || entry->payloadVersion != CookedMesh::V1::Version)
        return {{Status::Unsupported,"Archive entry is not a supported CookedMesh v1 payload."}};
    return {{},m_state->assets.at(asset.ToString())};
}
Outcome ArchiveProvider::CheckCurrent() const
{
    // Snapshot semantics: both files remain write/delete pinned. A new source
    // requires explicitly opening a new provider and a fresh catalog/session.
    return {};
}
AcceptanceResult ArchiveProvider::PinAcceptance(const AssetEvidence& expected) const
{
    const auto lookup = Lookup(expected.assetID);
    if(lookup.status != Status::Success || *lookup.evidence != expected)
        return {{Status::Stale,"Archive acceptance belongs to a different asset, generation or source snapshot."}};
    return {{},m_state};
}
PayloadResult ArchiveProvider::Acquire(const AssetID& asset) const { return Acquire(asset,m_state->policy.mesh); }
PayloadResult ArchiveProvider::Acquire(const AssetID& asset, const CookedMesh::Policy& admission) const
{
    return Guard<PayloadResult>([&]() -> PayloadResult
    {
        const auto lookup = Lookup(asset);
        if(lookup.status != Status::Success) return {{lookup.status,lookup.message}};
        auto policy = m_state->policy.mesh;
        policy.maxPayloadBytes = (std::min)(policy.maxPayloadBytes,admission.maxPayloadBytes);
        policy.maxDecodedBytes = (std::min)(policy.maxDecodedBytes,admission.maxDecodedBytes);
        policy.maxSubmeshes = (std::min)(policy.maxSubmeshes,admission.maxSubmeshes);
        policy.maxMaterialSlots = (std::min)(policy.maxMaterialSlots,admission.maxMaterialSlots);
        policy.maxStringTableBytes = (std::min)(policy.maxStringTableBytes,admission.maxStringTableBytes);
        policy.maxStringBytes = (std::min)(policy.maxStringBytes,admission.maxStringBytes);
        if(lookup.evidence->payload.payloadBytes > policy.maxPayloadBytes) return {{Status::AdmissionLimit,"Archived mesh exceeds consumer read admission."}};
        auto bytes = m_state->reader.Read(asset); // Entire single payload is integrity checked before metadata access.
        auto metadata = CookedMesh::DecodeMetadata(bytes,policy);
        if(metadata.status != CookedMesh::Status::Success) return {Decoded(metadata.status,metadata.message)};
        if(metadata.snapshot->evidence != lookup.evidence->payload) return {{Status::Corrupt,"Archive TOC evidence differs from actual CookedMesh header/content."}};
        auto lease = std::shared_ptr<PayloadLease>(new PayloadLease);
        lease->m_bytes = std::move(bytes); lease->m_metadata = std::move(metadata.snapshot);
        lease->m_evidence = lookup.evidence; lease->m_source = m_state;
        return {{},std::move(lease)};
    });
}
MetadataResult ArchiveProvider::ReadMetadata(const AssetID& asset) const
{
    const auto payload = Acquire(asset);
    if(payload.status != Status::Success) return {{payload.status,payload.message}};
    return {{},payload.lease->Metadata(),payload.lease->m_evidence};
}
}
