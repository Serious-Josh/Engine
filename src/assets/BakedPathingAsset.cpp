#include "assets/BakedPathingAsset.h"

#include "Project/ProjectPaths.h"
#include "assets/AssetRegistrySerializer.h"
#include "assets/ProjectAssetResolver.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace BakedPathingAssets
{
namespace
{
    constexpr std::array<char, 8> Magic{'E', 'N', 'G', 'P', 'A', 'T', 'H', 'B'};
    constexpr std::size_t HashBytes = 8;

    std::uint64_t Fnv(std::span<const std::byte> bytes) noexcept
    {
        std::uint64_t hash = 14695981039346656037ull;

        for(const std::byte value : bytes)
        {
            hash = (hash ^ static_cast<std::uint64_t>(value)) * 1099511628211ull;
        }

        return hash;
    }

    class Writer
    {
    public:
        template<typename T>
        void Put(T value)
        {
            const auto* raw = reinterpret_cast<const std::byte*>(&value);
            bytes.insert(bytes.end(), raw, raw + sizeof(T));
        }

        void PutText(const std::string& text)
        {
            Put<std::uint32_t>(static_cast<std::uint32_t>(text.size()));
            const auto* raw = reinterpret_cast<const std::byte*>(text.data());
            bytes.insert(bytes.end(), raw, raw + text.size());
        }

        std::vector<std::byte> bytes;
    };

    // Every read is bounds-checked; a failed read leaves `ok` false and returns zero.
    class Reader
    {
    public:
        explicit Reader(std::span<const std::byte> bytes) :
            m_bytes(bytes)
        {
        }

        template<typename T>
        T Get()
        {
            T value{};

            if(!ok || m_bytes.size() - m_at < sizeof(T))
            {
                ok = false;
                return value;
            }

            std::memcpy(&value, m_bytes.data() + m_at, sizeof(T));
            m_at += sizeof(T);
            return value;
        }

        std::string GetText()
        {
            const auto length = Get<std::uint32_t>();

            if(!ok || length > MaximumTextBytes || m_bytes.size() - m_at < length)
            {
                ok = false;
                return {};
            }

            std::string text(reinterpret_cast<const char*>(m_bytes.data() + m_at), length);
            m_at += length;
            return text;
        }

        std::size_t Remaining() const noexcept
        {
            return m_bytes.size() - m_at;
        }

        std::size_t Position() const noexcept
        {
            return m_at;
        }

        bool ok = true;

    private:
        std::span<const std::byte> m_bytes;
        std::size_t m_at = 0;
    };

    // Everything Decode requires beyond the byte layout. Shared by Encode, so nothing Decode would refuse is ever written.
    std::string Validate(const Data& data)
    {
        if(std::string problem; !ValidatePathingBakeSettings(data.settings, &problem))
        {
            return problem;
        }

        if(data.sourceProbeBatch.empty() || data.sourceProbeBatch.size() > MaximumTextBytes)
        {
            return "the source probe reference is invalid";
        }

        if(data.probes.empty() || data.probes.size() > MaximumBytes / 16)
        {
            return "the probe count is invalid";
        }

        for(const std::array<float, 4>& probe : data.probes)
        {
            if(!std::isfinite(probe[0]) || !std::isfinite(probe[1]) || !std::isfinite(probe[2]) || !std::isfinite(probe[3]) ||
                probe[3] <= 0.0f)
            {
                return "a probe is not finite or has no radius";
            }
        }

        if(data.batch.empty())
        {
            return "there is no Steam Audio batch";
        }

        if(data.layerBytes == 0 || data.layerBytes > data.batch.size())
        {
            return "the pathing layer has no data, or more than the batch holds";
        }

        return {};
    }

    DecodeResult Failure(DecodeError error, std::string message)
    {
        return DecodeResult{error, std::move(message)};
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
    if(data.steamAudioVersion != SteamAudioVersion)
    {
        throw std::length_error("baked pathing: unsupported Steam Audio version");
    }

    if(const std::string problem = Validate(data); !problem.empty())
    {
        throw std::length_error("baked pathing: " + problem);
    }

    Writer out;
    const auto* magic = reinterpret_cast<const std::byte*>(Magic.data());
    out.bytes.insert(out.bytes.end(), magic, magic + Magic.size());
    out.Put<std::uint32_t>(FormatVersion);
    out.Put<std::uint32_t>(data.steamAudioVersion);
    out.Put<std::uint32_t>(data.settings.samples);
    out.Put<float>(data.settings.radius);
    out.Put<float>(data.settings.threshold);
    out.Put<float>(data.settings.visibilityRange);
    out.Put<float>(data.settings.pathRange);
    out.Put<std::uint64_t>(data.sourceProbeFingerprint);
    out.Put<std::uint64_t>(data.sourceProbesHash);
    out.PutText(data.sourceProbeBatch);
    out.Put<std::uint64_t>(data.geometryHash);
    out.Put<std::uint64_t>(data.settingsHash);
    out.Put<std::uint64_t>(data.fingerprint);
    out.Put<std::uint32_t>(static_cast<std::uint32_t>(data.probes.size()));
    out.Put<std::uint64_t>(data.layerBytes);
    out.Put<std::uint64_t>(data.batch.size());

    for(const std::array<float, 4>& probe : data.probes)
    {
        for(const float value : probe)
        {
            out.Put<float>(value);
        }
    }

    out.bytes.insert(out.bytes.end(), data.batch.begin(), data.batch.end());

    if(out.bytes.size() + HashBytes > MaximumBytes)
    {
        throw std::length_error("baked pathing: larger than one asset can hold");
    }

    out.Put<std::uint64_t>(Fnv(out.bytes));
    return std::move(out.bytes);
}

DecodeResult Decode(std::span<const std::byte> bytes, Data& data)
{
    if(bytes.size() < Magic.size() + 8 + HashBytes || bytes.size() > MaximumBytes)
    {
        return Failure(DecodeError::NotBakedPathing, "the baked pathing has an impossible size");
    }

    if(std::memcmp(bytes.data(), Magic.data(), Magic.size()) != 0)
    {
        return Failure(DecodeError::NotBakedPathing, "not baked pathing");
    }

    // The checksum covers everything before it, so it is checked before any count or size is trusted.
    const std::span<const std::byte> covered = bytes.first(bytes.size() - HashBytes);
    std::uint64_t stored = 0;
    std::memcpy(&stored, bytes.data() + covered.size(), HashBytes);
    Reader in(covered);
    in.Get<std::array<char, 8>>();
    const auto version = in.Get<std::uint32_t>();

    if(version != FormatVersion)
    {
        return Failure(DecodeError::UnsupportedVersion, "unsupported baked pathing version " + std::to_string(version));
    }

    if(stored != Fnv(covered))
    {
        return Failure(DecodeError::Corrupt, "the baked pathing is corrupt");
    }

    Data decoded;
    decoded.steamAudioVersion = in.Get<std::uint32_t>();

    if(in.ok && decoded.steamAudioVersion != SteamAudioVersion)
    {
        return Failure(DecodeError::UnsupportedSteamAudioVersion, "baked with an unsupported Steam Audio version");
    }

    decoded.settings.samples = in.Get<std::uint32_t>();
    decoded.settings.radius = in.Get<float>();
    decoded.settings.threshold = in.Get<float>();
    decoded.settings.visibilityRange = in.Get<float>();
    decoded.settings.pathRange = in.Get<float>();
    decoded.sourceProbeFingerprint = in.Get<std::uint64_t>();
    decoded.sourceProbesHash = in.Get<std::uint64_t>();
    decoded.sourceProbeBatch = in.GetText();
    decoded.geometryHash = in.Get<std::uint64_t>();
    decoded.settingsHash = in.Get<std::uint64_t>();
    decoded.fingerprint = in.Get<std::uint64_t>();
    const auto probeCount = in.Get<std::uint32_t>();
    decoded.layerBytes = in.Get<std::uint64_t>();
    const auto batchBytes = in.Get<std::uint64_t>();

    // The counts are bounded by what the remaining bytes could hold before anything is allocated.
    if(!in.ok || std::uint64_t{probeCount} * 16 > in.Remaining() || batchBytes > in.Remaining())
    {
        return Failure(DecodeError::Invalid, "the baked pathing is truncated or its counts are impossible");
    }

    decoded.probes.resize(probeCount);

    for(std::array<float, 4>& probe : decoded.probes)
    {
        for(float& value : probe)
        {
            value = in.Get<float>();
        }
    }

    if(!in.ok || in.Remaining() != batchBytes)
    {
        return Failure(DecodeError::Invalid, "the baked pathing is truncated or has trailing data");
    }

    decoded.batch.assign(covered.begin() + static_cast<std::ptrdiff_t>(in.Position()), covered.end());

    if(const std::string problem = Validate(decoded); !problem.empty())
    {
        return Failure(DecodeError::Invalid, "the baked pathing is invalid: " + problem);
    }

    data = std::move(decoded);
    return {};
}

DecodeResult LoadFromFile(const std::filesystem::path& path, Data& data)
{
    std::error_code filesystemError;
    const auto size = std::filesystem::file_size(path, filesystemError);

    if(filesystemError || size > MaximumBytes)
    {
        return Failure(DecodeError::NotBakedPathing, "the baked pathing file cannot be read");
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream file(path, std::ios::binary);

    if(!file || !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    {
        return Failure(DecodeError::NotBakedPathing, "the baked pathing file cannot be read");
    }

    return Decode(bytes, data);
}

Resolution Resolve(std::string_view reference, const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    Resolution resolution;
    const auto id = AssetID::ParseReference(std::string(reference));
    const AssetRecord* record = id ? registry.Find(*id) : nullptr;

    if(!record || record->type != AssetType::BakedPathing)
    {
        resolution.missing = true;
        resolution.decode = Failure(DecodeError::NotBakedPathing, "not registered baked pathing");
        return resolution;
    }

    const AssetResolution file = ProjectAssetResolver::Resolve(paths, registry, *id);

    if(file.status != AssetAvailability::Available)
    {
        resolution.missing = true;
        resolution.decode = Failure(DecodeError::NotBakedPathing, "the baked pathing file is missing");
        return resolution;
    }

    Data data;
    resolution.decode = LoadFromFile(file.resolvedPath, data);

    if(resolution.decode)
    {
        resolution.data = std::move(data);
    }

    return resolution;
}

std::optional<std::string> Store(const ProjectPaths& paths, ProjectAssetRegistry& registry, std::span<const std::byte> encoded,
    std::string* error)
{
    const auto fail = [error](const std::string& message) -> std::optional<std::string>
    {
        if(error)
        {
            *error = message;
        }

        return std::nullopt;
    };

    // Only what Decode accepts is ever published.
    Data check;

    if(const DecodeResult decoded = Decode(encoded, check); !decoded)
    {
        return fail("refusing to store invalid baked pathing: " + decoded.message);
    }

    const std::optional<AssetID> id = AssetID::Generate();

    if(!id)
    {
        return fail("cannot generate an asset ID");
    }

    ProjectAssetRegistry staged = registry;
    const std::filesystem::path relativePath = std::filesystem::path("Content/Audio/Probes") / (id->ToString() + std::string(Extension));

    if(!staged.Register(AssetRecord{*id, AssetType::BakedPathing, relativePath}))
    {
        return fail("the baked pathing cannot be registered");
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
            return fail("the baked pathing file cannot be written");
        }
    }

    std::string registryError;

    if(!AssetRegistrySerializer::SaveToFile(staged, paths.GetAssetRegistryPath(), &registryError))
    {
        std::filesystem::remove(temporary, filesystemError);
        return fail("cannot persist the registration: " + registryError);
    }

    std::filesystem::rename(temporary, destination, filesystemError);

    if(filesystemError)
    {
        std::filesystem::remove(temporary, filesystemError);
        std::string ignored;
        AssetRegistrySerializer::SaveToFile(registry, paths.GetAssetRegistryPath(), &ignored);
        return fail("the baked pathing file cannot be placed");
    }

    registry = std::move(staged);
    return id->ToReference();
}

bool Remove(const ProjectPaths& paths, ProjectAssetRegistry& registry, std::string_view reference, std::string* error)
{
    const auto id = AssetID::ParseReference(std::string(reference));
    const AssetRecord* record = id ? registry.Find(*id) : nullptr;

    if(!record || record->type != AssetType::BakedPathing)
    {
        return true;
    }

    const std::filesystem::path file = paths.GetRoot() / record->relativePath;
    ProjectAssetRegistry staged = registry;

    if(!staged.Unregister(*id))
    {
        if(error)
        {
            *error = "the baked pathing cannot be unregistered";
        }

        return false;
    }

    std::string registryError;

    if(!AssetRegistrySerializer::SaveToFile(staged, paths.GetAssetRegistryPath(), &registryError))
    {
        if(error)
        {
            *error = "cannot persist the removal: " + registryError;
        }

        return false;
    }

    registry = std::move(staged);
    std::error_code filesystemError;
    std::filesystem::remove(file, filesystemError);
    return true;
}
}
