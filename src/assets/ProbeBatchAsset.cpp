#include "assets/ProbeBatchAsset.h"

#include "Project/ProjectPaths.h"
#include "assets/AssetRegistrySerializer.h"
#include "assets/ProjectAssetResolver.h"

#include <cmath>
#include <cstring>
#include <fstream>

namespace ProbeBatchAssets
{
namespace
{
    constexpr std::array<char, 8> Magic{'E', 'N', 'G', 'P', 'R', 'O', 'B', 'E'};
    constexpr std::size_t HeaderBytes = 8 + 4 + 4 + 8 + 4 + 4 + 8;
    constexpr std::size_t HashBytes = 8;
    constexpr std::size_t ProbeBytes = 16;

    bool Fail(std::string* error, const std::string& message)
    {
        if(error)
        {
            *error = message;
        }

        return false;
    }

    std::uint64_t Hash(std::span<const std::byte> bytes)
    {
        std::uint64_t hash = 14695981039346656037ull;

        for(const std::byte value : bytes)
        {
            hash = (hash ^ static_cast<std::uint64_t>(value)) * 1099511628211ull;
        }

        return hash;
    }

    template<typename T>
    void Put(std::vector<std::byte>& out, std::size_t& at, T value)
    {
        std::memcpy(out.data() + at, &value, sizeof(T));
        at += sizeof(T);
    }

    template<typename T>
    T Get(std::span<const std::byte> bytes, std::size_t& at)
    {
        T value{};
        std::memcpy(&value, bytes.data() + at, sizeof(T));
        at += sizeof(T);
        return value;
    }

    std::filesystem::path TemporaryPath(const std::filesystem::path& path)
    {
        std::filesystem::path temporary = path;
        temporary += ".tmp";
        return temporary;
    }
}

std::vector<std::byte> Encode(const Data& data)
{
    std::vector<std::byte> out(HeaderBytes + data.probes.size() * ProbeBytes + data.batch.size() + HashBytes);
    std::size_t at = 0;
    std::memcpy(out.data(), Magic.data(), Magic.size());
    at += Magic.size();
    Put<std::uint32_t>(out, at, FormatVersion);
    Put<std::uint32_t>(out, at, data.rejectedProbes);
    Put<std::uint64_t>(out, at, data.fingerprint);
    Put<std::uint32_t>(out, at, static_cast<std::uint32_t>(data.probes.size()));
    Put<std::uint32_t>(out, at, data.ambiguousProbes);
    Put<std::uint64_t>(out, at, data.batch.size());

    for(const std::array<float, 4>& probe : data.probes)
    {
        for(const float value : probe)
        {
            Put<float>(out, at, value);
        }
    }

    if(!data.batch.empty())
    {
        std::memcpy(out.data() + at, data.batch.data(), data.batch.size());
        at += data.batch.size();
    }

    Put<std::uint64_t>(out, at, Hash(std::span<const std::byte>(out).first(at)));
    return out;
}

bool Decode(std::span<const std::byte> bytes, Data& data, std::string* error)
{
    if(bytes.size() < HeaderBytes + HashBytes || bytes.size() > MaximumBytes)
    {
        return Fail(error, "the probe batch has an impossible size");
    }

    if(std::memcmp(bytes.data(), Magic.data(), Magic.size()) != 0)
    {
        return Fail(error, "not a probe batch");
    }

    std::size_t at = Magic.size();
    const auto version = Get<std::uint32_t>(bytes, at);

    if(version != FormatVersion)
    {
        return Fail(error, "unsupported probe batch version " + std::to_string(version));
    }

    Data decoded;
    decoded.rejectedProbes = Get<std::uint32_t>(bytes, at);
    decoded.fingerprint = Get<std::uint64_t>(bytes, at);
    const auto count = Get<std::uint32_t>(bytes, at);
    decoded.ambiguousProbes = Get<std::uint32_t>(bytes, at);
    const auto batchBytes = Get<std::uint64_t>(bytes, at);
    const std::uint64_t expected = std::uint64_t{HeaderBytes} + std::uint64_t{count} * ProbeBytes + batchBytes + HashBytes;

    if(batchBytes > MaximumBytes || expected != bytes.size())
    {
        return Fail(error, "the probe batch is truncated or has trailing data");
    }

    const std::size_t hashed = bytes.size() - HashBytes;
    std::size_t hashAt = hashed;

    if(Get<std::uint64_t>(bytes, hashAt) != Hash(bytes.first(hashed)))
    {
        return Fail(error, "the probe batch is corrupt");
    }

    decoded.probes.resize(count);

    for(std::array<float, 4>& probe : decoded.probes)
    {
        for(float& value : probe)
        {
            value = Get<float>(bytes, at);

            if(!std::isfinite(value))
            {
                return Fail(error, "the probe batch has a non-finite probe");
            }
        }
    }

    decoded.batch.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(hashed));

    if(decoded.batch.empty())
    {
        return Fail(error, "the probe batch has no Steam Audio data");
    }

    data = std::move(decoded);
    return true;
}

bool LoadFromFile(const std::filesystem::path& path, Data& data, std::string* error)
{
    std::error_code filesystemError;
    const auto size = std::filesystem::file_size(path, filesystemError);

    if(filesystemError || size > MaximumBytes)
    {
        return Fail(error, "the probe batch file cannot be read");
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream file(path, std::ios::binary);

    if(!file || !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    {
        return Fail(error, "the probe batch file cannot be read");
    }

    return Decode(bytes, data, error);
}

std::optional<Data> Resolve(std::string_view reference, const ProjectPaths& paths, const ProjectAssetRegistry& registry, std::string* error)
{
    const auto id = AssetID::ParseReference(std::string(reference));
    const AssetRecord* record = id ? registry.Find(*id) : nullptr;

    if(!record || record->type != AssetType::ProbeBatch)
    {
        Fail(error, "not a registered probe batch");
        return std::nullopt;
    }

    const AssetResolution resolution = ProjectAssetResolver::Resolve(paths, registry, *id);

    if(resolution.status != AssetAvailability::Available)
    {
        Fail(error, "the probe batch file is missing");
        return std::nullopt;
    }

    Data data;

    if(!LoadFromFile(resolution.resolvedPath, data, error))
    {
        return std::nullopt;
    }

    return data;
}

std::optional<std::string> Store(const ProjectPaths& paths, ProjectAssetRegistry& registry, std::span<const std::byte> encoded, std::string* error)
{
    const std::optional<AssetID> id = AssetID::Generate();

    if(!id)
    {
        Fail(error, "cannot generate an asset ID");
        return std::nullopt;
    }

    if(encoded.size() > MaximumBytes)
    {
        Fail(error, "the probes need " + std::to_string(encoded.size()) + " bytes, more than one asset can hold (" + std::to_string(MaximumBytes) + ")");
        return std::nullopt;
    }

    ProjectAssetRegistry staged = registry;
    const std::filesystem::path relativePath = std::filesystem::path("Content/Audio/Probes") / (id->ToString() + std::string(Extension));

    if(!staged.Register(AssetRecord{*id, AssetType::ProbeBatch, relativePath}))
    {
        Fail(error, "the probe batch cannot be registered");
        return std::nullopt;
    }

    const std::filesystem::path destination = paths.GetRoot() / relativePath;
    const std::filesystem::path temporary = TemporaryPath(destination);
    std::error_code filesystemError;
    std::filesystem::create_directories(destination.parent_path(), filesystemError);

    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));

        if(!file.flush())
        {
            file.close();
            std::filesystem::remove(temporary, filesystemError);
            Fail(error, "the probe batch file cannot be written");
            return std::nullopt;
        }
    }

    std::string registryError;

    if(!AssetRegistrySerializer::SaveToFile(staged, paths.GetAssetRegistryPath(), &registryError))
    {
        std::filesystem::remove(temporary, filesystemError);
        Fail(error, "cannot persist the registration: " + registryError);
        return std::nullopt;
    }

    std::filesystem::rename(temporary, destination, filesystemError);

    if(filesystemError)
    {
        std::filesystem::remove(temporary, filesystemError);
        std::string ignored;
        AssetRegistrySerializer::SaveToFile(registry, paths.GetAssetRegistryPath(), &ignored);
        Fail(error, "the probe batch file cannot be placed");
        return std::nullopt;
    }

    registry = std::move(staged);
    return id->ToReference();
}

bool Remove(const ProjectPaths& paths, ProjectAssetRegistry& registry, std::string_view reference, std::string* error)
{
    const auto id = AssetID::ParseReference(std::string(reference));
    const AssetRecord* record = id ? registry.Find(*id) : nullptr;

    if(!record || record->type != AssetType::ProbeBatch)
    {
        return true;
    }

    const std::filesystem::path file = paths.GetRoot() / record->relativePath;
    ProjectAssetRegistry staged = registry;

    if(!staged.Unregister(*id))
    {
        return Fail(error, "the probe batch cannot be unregistered");
    }

    std::string registryError;

    if(!AssetRegistrySerializer::SaveToFile(staged, paths.GetAssetRegistryPath(), &registryError))
    {
        return Fail(error, "cannot persist the removal: " + registryError);
    }

    registry = std::move(staged);
    std::error_code filesystemError;
    std::filesystem::remove(file, filesystemError);
    return true;
}
}
