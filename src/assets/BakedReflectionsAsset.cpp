#include "assets/BakedReflectionsAsset.h"

#include "Project/ProjectPaths.h"
#include "Scene/AudioComponents.h"
#include "assets/AssetRegistrySerializer.h"
#include "assets/ProjectAssetResolver.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>
#include <tuple>

namespace BakedReflectionsAssets
{
namespace
{
    constexpr std::array<char, 8> Magic{'E', 'N', 'G', 'B', 'A', 'K', 'E', 'D'};
    constexpr std::size_t HashBytes = 8;
    constexpr std::uint32_t MaximumOrder = 3;
    constexpr float MaximumDuration = 10.0f;

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

    bool IsPositiveZero(float value) noexcept
    {
        return std::bit_cast<std::uint32_t>(value) == 0u;
    }

    bool Finite(const std::array<float, 3>& values) noexcept
    {
        return std::isfinite(values[0]) && std::isfinite(values[1]) && std::isfinite(values[2]);
    }

    // Bit identity of an identifier: what the SDK compares, and what must not repeat within one batch.
    auto IdentifierKey(const Layer& layer)
    {
        return std::make_tuple(static_cast<std::uint32_t>(layer.kind), std::bit_cast<std::uint32_t>(layer.centre[0]),
            std::bit_cast<std::uint32_t>(layer.centre[1]), std::bit_cast<std::uint32_t>(layer.centre[2]),
            std::bit_cast<std::uint32_t>(layer.radius));
    }

    // Everything Decode requires beyond the byte layout. Shared by Encode, so nothing Decode would refuse is ever written.
    std::string Validate(const Data& data)
    {
        const BakeParameters& parameters = data.parameters;
        using Limits = ReflectionBakeSettings;

        if(parameters.rays < Limits::MinimumRays || parameters.rays > Limits::MaximumRays ||
            parameters.diffuseSamples < Limits::MinimumDiffuseSamples || parameters.diffuseSamples > Limits::MaximumDiffuseSamples ||
            parameters.bounces < Limits::MinimumBounces || parameters.bounces > Limits::MaximumBounces)
        {
            return "the bake settings are out of range";
        }

        if(!std::isfinite(parameters.irradianceMinimumDistance) ||
            parameters.irradianceMinimumDistance < Limits::MinimumIrradianceDistance ||
            parameters.irradianceMinimumDistance > Limits::MaximumIrradianceDistance)
        {
            return "the irradiance minimum is out of range";
        }

        if(parameters.order > MaximumOrder || !std::isfinite(parameters.duration) || parameters.duration <= 0.0f ||
            parameters.duration > MaximumDuration)
        {
            return "the order or duration is out of range";
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

        if(data.layers.empty() || data.layers.size() > MaximumLayers)
        {
            return "the layer count is invalid";
        }

        if(data.batch.empty())
        {
            return "there is no Steam Audio batch";
        }

        std::set<decltype(IdentifierKey(data.layers.front()))> identifiers;
        std::set<std::string> sources;

        for(const Layer& layer : data.layers)
        {
            if(layer.dataBytes == 0 || layer.dataBytes > data.batch.size())
            {
                return "a layer has no data, or more than the batch holds";
            }

            if(layer.kind == LayerKind::Reverb)
            {
                const bool zeroSphere = IsPositiveZero(layer.centre[0]) && IsPositiveZero(layer.centre[1]) &&
                    IsPositiveZero(layer.centre[2]) && IsPositiveZero(layer.radius);

                if(!zeroSphere || !layer.sourceObjectId.empty())
                {
                    return "a listener-centric layer must have an all-zero sphere and no source";
                }
            }
            else if(layer.kind == LayerKind::StaticSource)
            {
                if(!Finite(layer.centre) || !std::isfinite(layer.radius) || layer.radius <= 0.0f)
                {
                    return "a static-source layer needs a finite centre and a positive radius";
                }

                if(layer.sourceObjectId.empty() || layer.sourceObjectId.size() > MaximumTextBytes)
                {
                    return "a static-source layer needs a source object id";
                }

                if(!sources.insert(layer.sourceObjectId).second)
                {
                    return "two layers belong to the same source";
                }
            }
            else
            {
                return "unknown layer kind";
            }

            if(!identifiers.insert(IdentifierKey(layer)).second)
            {
                return "two layers have the same identifier";
            }
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

std::uint64_t HashProbes(std::span<const std::array<float, 4>> probes) noexcept
{
    return Fnv(std::as_bytes(probes));
}

std::vector<std::byte> Encode(const Data& data)
{
    if(data.steamAudioVersion != SteamAudioVersion)
    {
        throw std::length_error("baked reflections: unsupported Steam Audio version");
    }

    if(const std::string problem = Validate(data); !problem.empty())
    {
        throw std::length_error("baked reflections: " + problem);
    }

    Writer out;
    const auto* magic = reinterpret_cast<const std::byte*>(Magic.data());
    out.bytes.insert(out.bytes.end(), magic, magic + Magic.size());
    out.Put<std::uint32_t>(FormatVersion);
    out.Put<std::uint32_t>(data.steamAudioVersion);
    out.Put<std::uint32_t>(data.parameters.rays);
    out.Put<std::uint32_t>(data.parameters.diffuseSamples);
    out.Put<std::uint32_t>(data.parameters.bounces);
    out.Put<float>(data.parameters.irradianceMinimumDistance);
    out.Put<std::uint32_t>(data.parameters.order);
    out.Put<float>(data.parameters.duration);
    out.Put<std::uint64_t>(data.sourceProbeFingerprint);
    out.Put<std::uint64_t>(data.sourceProbesHash);
    out.PutText(data.sourceProbeBatch);
    out.Put<std::uint32_t>(static_cast<std::uint32_t>(data.probes.size()));
    out.Put<std::uint32_t>(static_cast<std::uint32_t>(data.layers.size()));
    out.Put<std::uint64_t>(data.batch.size());

    for(const std::array<float, 4>& probe : data.probes)
    {
        for(const float value : probe)
        {
            out.Put<float>(value);
        }
    }

    for(const Layer& layer : data.layers)
    {
        out.Put<std::uint32_t>(static_cast<std::uint32_t>(layer.kind));

        for(const float value : layer.centre)
        {
            out.Put<float>(value);
        }

        out.Put<float>(layer.radius);
        out.PutText(layer.sourceObjectId);
        out.Put<std::uint64_t>(layer.probesHash);
        out.Put<std::uint64_t>(layer.geometryHash);
        out.Put<std::uint64_t>(layer.materialsHash);
        out.Put<std::uint64_t>(layer.settingsHash);
        out.Put<std::uint64_t>(layer.fingerprint);
        out.Put<std::uint64_t>(layer.dataBytes);
    }

    out.bytes.insert(out.bytes.end(), data.batch.begin(), data.batch.end());

    if(out.bytes.size() + HashBytes > MaximumBytes)
    {
        throw std::length_error("baked reflections: larger than one asset can hold");
    }

    out.Put<std::uint64_t>(Fnv(out.bytes));
    return std::move(out.bytes);
}

DecodeResult Decode(std::span<const std::byte> bytes, Data& data)
{
    if(bytes.size() < Magic.size() + 8 + HashBytes || bytes.size() > MaximumBytes)
    {
        return Failure(DecodeError::NotBakedReflections, "the baked reflections have an impossible size");
    }

    if(std::memcmp(bytes.data(), Magic.data(), Magic.size()) != 0)
    {
        return Failure(DecodeError::NotBakedReflections, "not baked reflections");
    }

    // The checksum covers everything before it, so check it before trusting any count or size.
    const std::span<const std::byte> covered = bytes.first(bytes.size() - HashBytes);
    std::uint64_t stored = 0;
    std::memcpy(&stored, bytes.data() + covered.size(), HashBytes);
    Reader in(covered);
    in.Get<std::array<char, 8>>();
    const auto version = in.Get<std::uint32_t>();

    if(version != FormatVersion)
    {
        return Failure(DecodeError::UnsupportedVersion, "unsupported baked reflections version " + std::to_string(version));
    }

    if(stored != Fnv(covered))
    {
        return Failure(DecodeError::Corrupt, "the baked reflections are corrupt");
    }

    Data decoded;
    decoded.steamAudioVersion = in.Get<std::uint32_t>();

    if(in.ok && decoded.steamAudioVersion != SteamAudioVersion)
    {
        return Failure(DecodeError::UnsupportedSteamAudioVersion, "baked with an unsupported Steam Audio version");
    }

    decoded.parameters.rays = in.Get<std::uint32_t>();
    decoded.parameters.diffuseSamples = in.Get<std::uint32_t>();
    decoded.parameters.bounces = in.Get<std::uint32_t>();
    decoded.parameters.irradianceMinimumDistance = in.Get<float>();
    decoded.parameters.order = in.Get<std::uint32_t>();
    decoded.parameters.duration = in.Get<float>();
    decoded.sourceProbeFingerprint = in.Get<std::uint64_t>();
    decoded.sourceProbesHash = in.Get<std::uint64_t>();
    decoded.sourceProbeBatch = in.GetText();
    const auto probeCount = in.Get<std::uint32_t>();
    const auto layerCount = in.Get<std::uint32_t>();
    const auto batchBytes = in.Get<std::uint64_t>();

    // Bound the counts by what the remaining bytes could possibly hold before allocating anything.
    constexpr std::size_t MinimumLayerBytes = 4 + 16 + 4 + 48;

    if(!in.ok || layerCount > MaximumLayers || std::uint64_t{probeCount} * 16 > in.Remaining() ||
        std::uint64_t{layerCount} * MinimumLayerBytes > in.Remaining() || batchBytes > in.Remaining())
    {
        return Failure(DecodeError::Invalid, "the baked reflections are truncated or their counts are impossible");
    }

    decoded.probes.resize(probeCount);

    for(std::array<float, 4>& probe : decoded.probes)
    {
        for(float& value : probe)
        {
            value = in.Get<float>();
        }
    }

    decoded.layers.resize(layerCount);

    for(Layer& layer : decoded.layers)
    {
        layer.kind = static_cast<LayerKind>(in.Get<std::uint32_t>());

        for(float& value : layer.centre)
        {
            value = in.Get<float>();
        }

        layer.radius = in.Get<float>();
        layer.sourceObjectId = in.GetText();
        layer.probesHash = in.Get<std::uint64_t>();
        layer.geometryHash = in.Get<std::uint64_t>();
        layer.materialsHash = in.Get<std::uint64_t>();
        layer.settingsHash = in.Get<std::uint64_t>();
        layer.fingerprint = in.Get<std::uint64_t>();
        layer.dataBytes = in.Get<std::uint64_t>();
    }

    if(!in.ok || in.Remaining() != batchBytes)
    {
        return Failure(DecodeError::Invalid, "the baked reflections are truncated or have trailing data");
    }

    decoded.batch.assign(covered.begin() + static_cast<std::ptrdiff_t>(in.Position()), covered.end());

    if(const std::string problem = Validate(decoded); !problem.empty())
    {
        return Failure(DecodeError::Invalid, "the baked reflections are invalid: " + problem);
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
        return Failure(DecodeError::NotBakedReflections, "the baked reflections file cannot be read");
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream file(path, std::ios::binary);

    if(!file || !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    {
        return Failure(DecodeError::NotBakedReflections, "the baked reflections file cannot be read");
    }

    return Decode(bytes, data);
}

Resolution Resolve(std::string_view reference, const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    Resolution resolution;
    const auto id = AssetID::ParseReference(std::string(reference));
    const AssetRecord* record = id ? registry.Find(*id) : nullptr;

    if(!record || record->type != AssetType::BakedReflections)
    {
        resolution.missing = true;
        resolution.decode = Failure(DecodeError::NotBakedReflections, "not registered baked reflections");
        return resolution;
    }

    const AssetResolution file = ProjectAssetResolver::Resolve(paths, registry, *id);

    if(file.status != AssetAvailability::Available)
    {
        resolution.missing = true;
        resolution.decode = Failure(DecodeError::NotBakedReflections, "the baked reflections file is missing");
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
        return fail("refusing to store invalid baked reflections: " + decoded.message);
    }

    const std::optional<AssetID> id = AssetID::Generate();

    if(!id)
    {
        return fail("cannot generate an asset ID");
    }

    ProjectAssetRegistry staged = registry;
    const std::filesystem::path relativePath = std::filesystem::path("Content/Audio/Probes") / (id->ToString() + std::string(Extension));

    if(!staged.Register(AssetRecord{*id, AssetType::BakedReflections, relativePath}))
    {
        return fail("the baked reflections cannot be registered");
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
            return fail("the baked reflections file cannot be written");
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
        return fail("the baked reflections file cannot be placed");
    }

    registry = std::move(staged);
    return id->ToReference();
}

bool Remove(const ProjectPaths& paths, ProjectAssetRegistry& registry, std::string_view reference, std::string* error)
{
    const auto id = AssetID::ParseReference(std::string(reference));
    const AssetRecord* record = id ? registry.Find(*id) : nullptr;

    if(!record || record->type != AssetType::BakedReflections)
    {
        return true;
    }

    const std::filesystem::path file = paths.GetRoot() / record->relativePath;
    ProjectAssetRegistry staged = registry;

    if(!staged.Unregister(*id))
    {
        if(error)
        {
            *error = "the baked reflections cannot be unregistered";
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
