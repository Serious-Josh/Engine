#include "assets/DevelopmentCookedMeshProvider.h"
#include "assets/MeshImportUnitsJson.h"
#include "assets/DevelopmentMeshCookRecipe.h"
#include "Renderer/Preview/MaterialThumbnailValidation.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <expected>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace DevelopmentCookedMesh
{
    namespace
    {
        namespace fs = std::filesystem;
        using Json = nlohmann::ordered_json;
        template<class T> using Result = std::expected<T, Outcome>;

        auto Fail(Status status, std::string_view message) { return std::unexpected(Outcome{status, message}); }

        template<class T> T ErrorResult(Outcome error)
        {
            if constexpr(std::is_same_v<T, Outcome>) { return error; }
            else { return {error}; }
        }

        template<class T, class Function>
        T Guard(Function function)
        {
            try { return function(); }
            catch(const std::bad_alloc&) { return ErrorResult<T>({Status::AllocationFailure, "Provider allocation failed."}); }
            catch(const std::length_error&) { return ErrorResult<T>({Status::AdmissionLimit, "Provider container capacity exceeded."}); }
            catch(const fs::filesystem_error&) { return ErrorResult<T>({Status::IOFailure, "Provider filesystem operation failed."}); }
            catch(const Json::exception&) { return ErrorResult<T>({Status::Corrupt, "Invalid development manifest field."}); }
        }

        struct Handle
        {
            HANDLE value = INVALID_HANDLE_VALUE;
            ~Handle() { if(value != INVALID_HANDLE_VALUE) { CloseHandle(value); } }
        };

        Result<std::unique_ptr<Handle>> OpenFile(const fs::path& path, bool activation = false)
        {
            auto handle = std::make_unique<Handle>();
            handle->value = CreateFileW(path.c_str(), GENERIC_READ,
                FILE_SHARE_READ | (activation ? FILE_SHARE_DELETE : 0), nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

            if(handle->value == INVALID_HANDLE_VALUE)
            {
                const auto error = GetLastError();
                return Fail(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? Status::Missing : Status::IOFailure,
                    "Cannot open provider input file.");
            }

            BY_HANDLE_FILE_INFORMATION info{};

            if(!GetFileInformationByHandle(handle->value, &info))
            {
                return Fail(Status::IOFailure, "Cannot inspect provider input file.");
            }

            if(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            {
                return Fail(Status::Corrupt, "Provider input must be an ordinary non-linked file.");
            }

            return handle;
        }

        class DirectoryPins
        {
        public:
            Result<void> Pin(const fs::path& path)
            {
                fs::path prefix;

                for(const auto& part : path)
                {
                    prefix /= part;

                    if(!prefix.has_root_directory()) { continue; }

                    auto handle = std::make_unique<Handle>();
                    handle->value = CreateFileW(prefix.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);

                    if(handle->value == INVALID_HANDLE_VALUE)
                    {
                        const auto error = GetLastError();
                        return Fail(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? Status::Missing : Status::IOFailure,
                            "Cannot pin provider directory.");
                    }

                    BY_HANDLE_FILE_INFORMATION info{};

                    if(!GetFileInformationByHandle(handle->value, &info))
                    {
                        return Fail(Status::IOFailure, "Cannot inspect provider directory.");
                    }

                    if(!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    {
                        return Fail(Status::Corrupt, "Provider directories must be ordinary and non-linked.");
                    }

                    m_handles.push_back(std::move(handle));
                }

                return {};
            }

        private:
            std::vector<std::unique_ptr<Handle>> m_handles;
        };

        Result<std::vector<std::byte>> Read(HANDLE handle, size_t limit, std::optional<std::uint64_t> expectedSize = {})
        {
            LARGE_INTEGER size{};

            if(!GetFileSizeEx(handle, &size) || size.QuadPart < 0)
            {
                return Fail(Status::IOFailure, "Cannot obtain provider file size.");
            }

            const auto length = static_cast<std::uint64_t>(size.QuadPart);

            if(length > limit || length > (std::numeric_limits<size_t>::max)())
            {
                return Fail(Status::AdmissionLimit, "Provider file exceeds bounded read policy.");
            }

            if(expectedSize && length != *expectedSize)
            {
                return Fail(Status::Corrupt, "Payload size differs from manifest evidence.");
            }

            std::vector<std::byte> bytes(static_cast<size_t>(length));
            size_t offset = 0;

            while(offset < bytes.size())
            {
                const auto count = static_cast<DWORD>((std::min)(bytes.size() - offset, size_t(1024 * 1024)));
                DWORD read = 0;

                if(!ReadFile(handle, bytes.data() + offset, count, &read, nullptr) || read != count)
                {
                    return Fail(Status::IOFailure, "Cannot read complete bounded provider file.");
                }

                offset += count;
            }

            return bytes;
        }

        struct ParseLimit {};

        Result<Json> Parse(std::span<const std::byte> bytes)
        {
            bool duplicate = false;
            size_t events = 0;
            std::vector<std::set<std::string>> keys;

            const auto callback = [&](int depth, Json::parse_event_t event, Json& value)
            {
                if(depth > 16 || ++events > 131072 || (value.is_string() && value.get_ref<const std::string&>().size() > 4096))
                {
                    throw ParseLimit{};
                }

                if(event == Json::parse_event_t::object_start) { keys.emplace_back(); }
                if(event == Json::parse_event_t::key)
                {
                    duplicate |= !keys.back().insert(value.get<std::string>()).second;
                }

                if(event == Json::parse_event_t::object_end) { keys.pop_back(); }
                return true;
            };

            try
            {
                const auto* data = reinterpret_cast<const char*>(bytes.data());
                // Avoid null-pointer arithmetic on an empty input span.
                if(bytes.empty()) { return Fail(Status::Corrupt, "Empty provider JSON document."); }

                auto document = Json::parse(data, data + bytes.size(), callback);

                if(duplicate) { return Fail(Status::Corrupt, "Duplicate JSON object key."); }
                return document;
            }
            catch(const ParseLimit&) { return Fail(Status::AdmissionLimit, "Provider JSON depth/value/string limit exceeded."); }
            catch(const Json::exception&) { return Fail(Status::Corrupt, "Malformed provider JSON document."); }
        }

        bool Fields(const Json& object, std::initializer_list<const char*> fields)
        {
            return object.is_object() && object.size() == fields.size() &&
                std::all_of(fields.begin(), fields.end(), [&](const char* field) { return object.contains(field); });
        }

        std::optional<std::uint64_t> Number(const Json& object, const char* field)
        {
            if(!object.contains(field)) { return {}; }
            const auto& value = object.at(field);

            if(value.is_number_unsigned()) { return value.get<std::uint64_t>(); }
            if(value.is_number_integer() && value.get<std::int64_t>() >= 0) { return value.get<std::uint64_t>(); }
            return {};
        }

        bool Text(const Json& object, const char* field)
        {
            return object.contains(field) && object.at(field).is_string();
        }

        bool SHA256(std::string_view text)
        {
            return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c)
            {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            });
        }

        std::array<std::uint8_t, 32> DigestBytes(std::string_view text)
        {
            const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
            std::array<std::uint8_t, 32> bytes{};

            for(size_t i = 0; i < bytes.size(); ++i)
            {
                bytes[i] = static_cast<std::uint8_t>((nibble(text[i * 2]) << 4) | nibble(text[i * 2 + 1]));
            }

            return bytes;
        }

        Result<std::string> Digest(const fs::path& path)
        {
            try
            {
                MaterialThumbnailValidation validation;
                return validation.Digest(path);
            }
            catch(const std::bad_alloc&) { throw; }
            catch(const fs::filesystem_error&) { return Fail(Status::IOFailure, "Cannot inspect manifest for hashing."); }
            catch(const std::exception&) { return Fail(Status::IdentityFailure, "Cannot establish manifest SHA-256."); }
        }

        bool SafeGeneration(std::string_view path)
        {
            constexpr std::string_view prefix = "generations/generation-";

            if(!path.starts_with(prefix) || path.size() > 128) { return false; }
            auto suffix = path.substr(prefix.size());

            for(int part = 0; part < 3; ++part)
            {
                const auto dash = suffix.find('-');
                const auto number = suffix.substr(0, dash);

                if(number.empty() || !std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; }))
                {
                    return false;
                }

                if(part == 2) { return dash == std::string_view::npos; }
                if(dash == std::string_view::npos) { return false; }
                suffix.remove_prefix(dash + 1);
            }

            return false;
        }

        bool SafeSourcePath(std::string_view path)
        {
            if(!path.starts_with("Meshes/") || path.find_first_of("\\:\0", 0, 3) != std::string_view::npos) { return false; }

            while(!path.empty())
            {
                const auto slash = path.find('/');
                const auto part = path.substr(0, slash);

                if(part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') { return false; }
                if(slash == std::string_view::npos) { return true; }
                path.remove_prefix(slash + 1);
            }

            return false;
        }

        struct Activation
        {
            std::string generation;
            std::string manifestDigest;
            bool operator==(const Activation&) const = default;
        };

        Result<Activation> ReadActivation(const fs::path& cooked, const Policy& policy)
        {
            auto file = OpenFile(cooked / "active-mesh-generation.json", true);
            if(!file) { return std::unexpected(file.error()); }
            auto bytes = Read((*file)->value, policy.maxActivationBytes);
            if(!bytes) { return std::unexpected(bytes.error()); }
            auto json = Parse(*bytes);
            if(!json) { return std::unexpected(json.error()); }

            if(!Fields(*json, {"schemaVersion", "generation", "manifestSHA256"}) || !Number(*json, "schemaVersion") ||
                !Text(*json, "generation") || !Text(*json, "manifestSHA256"))
            {
                return Fail(Status::Corrupt, "Invalid activation-pointer structure.");
            }

            if(Number(*json, "schemaVersion") != 1) { return Fail(Status::Unsupported, "Unsupported activation-pointer version."); }
            Activation activation{json->at("generation").get<std::string>(), json->at("manifestSHA256").get<std::string>()};

            if(!SafeGeneration(activation.generation) || !SHA256(activation.manifestDigest))
            {
                return Fail(Status::Corrupt, "Unsafe generation identifier or invalid manifest SHA-256.");
            }

            return activation;
        }

        // Explicit source-independent allowlist for the C.2.1 recipe. No importer
        // headers, source files, Assimp invocation or current registry are needed.
        Json SupportedRecipe()
        {
            return DevelopmentMeshCookRecipe::Describe(3, "6.0.2", 21);
        }

        Outcome DecodeOutcome(CookedMesh::Status status, std::string_view message)
        {
            using S = CookedMesh::Status;

            switch(status)
            {
                case S::Success: return {};
                case S::Corrupt: return {Status::Corrupt, message};
                case S::Unsupported: return {Status::Unsupported, message};
                case S::IncompatibleCook: return {Status::IncompatibleCook, message};
                case S::AdmissionLimit: return {Status::AdmissionLimit, message};
                case S::AllocationFailure: return {Status::AllocationFailure, message};
                case S::IdentityFailure: return {Status::IdentityFailure, message};
            }

            return {Status::Unsupported, "Unknown cooked decoder outcome."};
        }
    }

    struct Provider::State
    {
        fs::path cooked;
        Policy policy;
        MappingEvidence mapping;
        Activation activation;
        std::map<std::string, std::shared_ptr<const AssetEvidence>> assets;
    };

    OpenResult Provider::Open(const fs::path& projectRoot, const Policy& policy)
    {
        return Guard<OpenResult>([&]() -> OpenResult
        {
            if(projectRoot.empty()) { return {{Status::Corrupt, "Provider requires an explicit project root."}}; }
            const auto cooked = fs::absolute(projectRoot).lexically_normal() / "Cooked";
            DirectoryPins pins;
            if(auto result = pins.Pin(cooked); !result) { return {result.error()}; }
            auto activation = ReadActivation(cooked, policy);
            if(!activation) { return {activation.error()}; }

            const auto generation = cooked / activation->generation;
            if(auto result = pins.Pin(generation); !result) { return {result.error()}; }
            const auto manifestPath = generation / "mesh-manifest.json";
            auto file = OpenFile(manifestPath);
            if(!file) { return {file.error()}; }
            auto bytes = Read((*file)->value, policy.maxManifestBytes);
            if(!bytes) { return {bytes.error()}; }
            auto digest = Digest(manifestPath); // Same pinned file/path; no timestamp-only cache.
            if(!digest) { return {digest.error()}; }
            if(*digest != activation->manifestDigest) { return {{Status::Corrupt, "Manifest SHA-256 differs from activation evidence."}}; }
            auto manifest = Parse(*bytes);
            if(!manifest) { return {manifest.error()}; }

            if(!Fields(*manifest, {"schemaVersion", "kind", "recipe", "assets"}) || !Number(*manifest, "schemaVersion") ||
                !Text(*manifest, "kind") || !manifest->at("recipe").is_object() || !manifest->at("assets").is_array())
            {
                return {{Status::Corrupt, "Invalid development manifest structure."}};
            }

            if(Number(*manifest, "schemaVersion") != 1 || manifest->at("kind") != "engine-development-mesh-manifest")
            {
                return {{Status::Unsupported, "Unsupported development manifest version or kind."}};
            }

            const auto recipe = manifest->at("recipe").dump();
            if(recipe != SupportedRecipe().dump()) { return {{Status::Unsupported, "Unsupported development mesh cook recipe."}}; }
            if(manifest->at("assets").size() > policy.maxEntries) { return {{Status::AdmissionLimit, "Manifest entry limit exceeded."}}; }

            auto state = std::make_shared<State>();
            state->cooked = fs::canonical(cooked);
            state->policy = policy;
            state->activation = *activation;
            state->mapping = {state->cooked.generic_string(), activation->generation, *digest, CookedMeshSource::NextMappingGeneration(), fs::canonical(projectRoot).generic_string()};

            if(!state->mapping.mappingGeneration) { return {{Status::AdmissionLimit, "Provider mapping tokens exhausted."}}; }
            std::string previous;

            for(const auto& entry : manifest->at("assets"))
            {
                auto compatibilityEntry = entry;
                MeshImportUnits units;

                if(entry.contains("importUnits"))
                {
                    try
                    {
                        units = DeserializeMeshImportUnits(entry.at("importUnits"));
                    }
                    catch(const std::exception&)
                    {
                        return {{Status::Corrupt, "Invalid mesh import units in development manifest."}};
                    }

                    compatibilityEntry.erase("importUnits");
                }

                if(!Fields(compatibilityEntry, {"assetID", "assetType", "sourcePath", "sourceSHA256", "recipeIdentity", "payloadSHA256",
                    "payloadBytes", "payloadPath", "formatVersion", "cookCompatibility", "vertexLayout"}) ||
                    !Text(entry, "assetID") || !Text(entry, "assetType") || !Text(entry, "sourcePath") || !Text(entry, "sourceSHA256") ||
                    !Text(entry, "recipeIdentity") || !Text(entry, "payloadSHA256") || !Text(entry, "payloadPath") ||
                    !Number(entry, "payloadBytes") || !Number(entry, "formatVersion") || !Number(entry, "cookCompatibility") ||
                    !Number(entry, "vertexLayout"))
                {
                    return {{Status::Corrupt, "Invalid mesh manifest entry structure."}};
                }

                const auto id = AssetID::Parse(entry.at("assetID").get_ref<const std::string&>());
                if(!id || id->ToString() <= previous) { return {{Status::Corrupt, "Noncanonical, duplicate or unordered AssetID."}}; }
                previous = id->ToString();
                if(entry.at("assetType") != "mesh") { return {{Status::Unsupported, "Manifest entry is not a mesh asset."}}; }
                if(Number(entry, "formatVersion") != CookedMesh::V1::Version || Number(entry, "vertexLayout") != CookedMesh::V1::VertexLayout)
                {
                    return {{Status::Unsupported, "Unsupported mesh manifest format or vertex layout."}};
                }

                if(Number(entry, "cookCompatibility") != CookedMesh::V1::CookCompatibility)
                {
                    return {{Status::IncompatibleCook, "Unsupported manifest mesh cook compatibility."}};
                }

                const auto length = *Number(entry, "payloadBytes");
                if(length < CookedMesh::V1::HeaderBytes) { return {{Status::Corrupt, "Manifest payload is smaller than the mesh header."}}; }
                if(length > policy.mesh.maxPayloadBytes || length > (std::numeric_limits<size_t>::max)())
                {
                    return {{Status::AdmissionLimit, "Manifest payload exceeds read admission."}};
                }

                const auto& payloadHash = entry.at("payloadSHA256").get_ref<const std::string&>();
                const auto& sourceHash = entry.at("sourceSHA256").get_ref<const std::string&>();
                const auto& sourcePath = entry.at("sourcePath").get_ref<const std::string&>();

                if(!SHA256(payloadHash) || !SHA256(sourceHash) || !SafeSourcePath(sourcePath) ||
                    entry.at("recipeIdentity") != recipe || entry.at("payloadPath") != "Meshes/" + id->ToString() + ".bin")
                {
                    return {{Status::Corrupt, "Invalid entry path, digest or recipe identity."}};
                }

                const CookedMesh::Evidence evidence{DigestBytes(payloadHash), length, CookedMesh::V1::CookCompatibility, CookedMesh::V1::Version};
                state->assets.emplace(id->ToString(), std::make_shared<const AssetEvidence>(AssetEvidence{
                    *id, AssetType::Mesh, evidence, state->mapping, recipe, sourceHash, sourcePath, 0, units}));
            }

            auto current = ReadActivation(cooked, policy);
            if(!current) { return {current.error()}; }
            if(*current != *activation) { return {{Status::Stale, "Publication changed while opening mapping."}}; }
            return {{}, std::shared_ptr<const Provider>(new Provider(std::move(state)))};
        });
    }

    const MappingEvidence& Provider::Mapping() const noexcept { return m_state->mapping; }
    size_t Provider::EntryCount() const noexcept { return m_state->assets.size(); }

    LookupResult Provider::Lookup(const AssetID& asset) const
    {
        const auto found = m_state->assets.find(asset.ToString());
        if(found == m_state->assets.end()) { return {{Status::Missing, "AssetID is absent from this cooked mapping."}}; }
        return {{}, found->second};
    }

    Outcome Provider::CheckCurrent() const
    {
        return Guard<Outcome>([&]() -> Outcome
        {
            DirectoryPins pins;
            if(auto result = pins.Pin(m_state->cooked); !result) { return result.error(); }
            auto activation = ReadActivation(m_state->cooked, m_state->policy);
            if(!activation) { return activation.error(); }
            if(*activation != m_state->activation) { return {Status::Stale, "Published cooked mapping changed."}; }
            return {};
        });
    }

    PayloadResult Provider::Acquire(const AssetID& asset) const
    {
        return Acquire(asset, m_state->policy.mesh);
    }

    AcceptanceResult Provider::PinAcceptance(const AssetEvidence& expected) const
    {
        return Guard<AcceptanceResult>([&]() -> AcceptanceResult
        {
            struct Pins
            {
                std::shared_ptr<const State> snapshot;
                DirectoryPins directories;
                std::unique_ptr<Handle> activation;
                std::unique_ptr<Handle> manifest;
            };

            const auto lookup = Lookup(expected.assetID);
            if(lookup.status != Status::Success || *lookup.evidence != expected)
            {
                return {{Status::Stale, "Acceptance asset/payload/provider evidence differs."}};
            }

            auto pins = std::make_shared<Pins>();
            pins->snapshot = m_state;
            const auto generation = m_state->cooked / m_state->activation.generation;
            if(auto result = pins->directories.Pin(generation); !result) { return {result.error()}; }

            // Deny writes AND deletion; directory pins exclude ancestor rename.
            auto activation = OpenFile(m_state->cooked / "active-mesh-generation.json");
            if(!activation) { return {activation.error()}; }
            pins->activation = std::move(*activation);
            auto current = ReadActivation(m_state->cooked, m_state->policy);
            if(!current) { return {current.error()}; }
            if(*current != m_state->activation) { return {{Status::Stale, "Acceptance generation changed."}}; }

            const auto manifestPath = generation / "mesh-manifest.json";
            auto manifest = OpenFile(manifestPath);
            if(!manifest) { return {manifest.error()}; }
            pins->manifest = std::move(*manifest);
            auto bytes = Read(pins->manifest->value, m_state->policy.maxManifestBytes);
            if(!bytes) { return {bytes.error()}; }
            auto digest = Digest(manifestPath);
            if(!digest) { return {digest.error()}; }
            if(*digest != m_state->activation.manifestDigest)
            {
                return {{Status::Stale, "Acceptance manifest no longer identifies the prepared payload."}};
            }

            return {{}, std::move(pins)};
        });
    }

    PayloadResult Provider::Acquire(const AssetID& asset, const CookedMesh::Policy& admission) const
    {
        return Guard<PayloadResult>([&]() -> PayloadResult
        {
            auto policy = m_state->policy.mesh;
            policy.maxPayloadBytes = (std::min)(policy.maxPayloadBytes, admission.maxPayloadBytes);
            policy.maxDecodedBytes = (std::min)(policy.maxDecodedBytes, admission.maxDecodedBytes);
            policy.maxSubmeshes = (std::min)(policy.maxSubmeshes, admission.maxSubmeshes);
            policy.maxMaterialSlots = (std::min)(policy.maxMaterialSlots, admission.maxMaterialSlots);
            policy.maxStringTableBytes = (std::min)(policy.maxStringTableBytes, admission.maxStringTableBytes);
            policy.maxStringBytes = (std::min)(policy.maxStringBytes, admission.maxStringBytes);

            const auto lookup = Lookup(asset);
            if(lookup.status != Status::Success) { return {{lookup.status, lookup.message}}; }
            const auto path = m_state->cooked / m_state->activation.generation / "Meshes" / (asset.ToString() + ".bin");
            DirectoryPins pins;

            if(auto result = pins.Pin(path.parent_path()); !result)
            {
                auto error = result.error();
                if(error.status == Status::Missing) { error = {Status::Stale, "Snapshot generation is no longer available."}; }
                return {error};
            }

            auto file = OpenFile(path);
            if(!file)
            {
                auto error = file.error();
                if(error.status == Status::Missing) { error = {Status::Stale, "Mapped payload is no longer available."}; }
                return {error};
            }

            auto bytes = Read((*file)->value, policy.maxPayloadBytes, lookup.evidence->payload.payloadBytes);
            if(!bytes) { return {bytes.error()}; }
            // Verify provider integrity independently of mesh structural decoding.
            // The file and its ancestors remain pinned for this fresh file hash.
            auto digest = Digest(path);
            if(!digest) { return {digest.error()}; }
            if(DigestBytes(*digest) != lookup.evidence->payload.payloadSHA256)
            {
                return {{Status::Corrupt, "Payload SHA-256 differs from manifest evidence."}};
            }

            auto metadata = CookedMesh::DecodeMetadata(*bytes, policy);
            if(metadata.status != CookedMesh::Status::Success) { return {DecodeOutcome(metadata.status, metadata.message)}; }
            if(metadata.snapshot->evidence != lookup.evidence->payload)
            {
                return {{Status::Corrupt, "Cooked payload identity differs from manifest evidence."}};
            }

            auto lease = std::shared_ptr<PayloadLease>(new PayloadLease);
            lease->m_bytes = std::move(*bytes);
            lease->m_evidence = lookup.evidence;
            lease->m_metadata = std::move(metadata.snapshot);
            return {{}, std::move(lease)};
        });
    }

    MetadataResult Provider::ReadMetadata(const AssetID& asset) const
    {
        const auto payload = Acquire(asset);
        if(payload.status != Status::Success) { return {{payload.status, payload.message}}; }
        return {{}, payload.lease->Metadata(), payload.lease->m_evidence};
    }
}
