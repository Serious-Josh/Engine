#include "Engine/ProbeVolumes.h"

#include "Engine/AcousticGeometry.h"

#include <bit>
#include <cmath>
#include <map>
#include <memory>
#include <string_view>

namespace ProbeVolumes
{
namespace
{
    class Hasher
    {
    public:
        void Add(std::uint64_t value) noexcept
        {
            // Whole values, not bytes: this runs over every vertex of a scene while a volume is selected.
            m_hash = (m_hash ^ value) * 0x9e3779b97f4a7c15ull;
            m_hash ^= m_hash >> 29;
        }

        void Add(float value) noexcept
        {
            // -0 and 0 are the same placement.
            const float canonical = value == 0.0f ? 0.0f : value;
            Add(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(canonical)));
        }

        void Add(std::string_view text) noexcept
        {
            Add(static_cast<std::uint64_t>(text.size()));

            for(const char character : text)
            {
                Add(static_cast<std::uint64_t>(static_cast<unsigned char>(character)));
            }
        }

        void Add(const Audio::AcousticTransform& matrix) noexcept
        {
            for(const auto& row : matrix)
            {
                for(const float value : row)
                {
                    Add(value);
                }
            }
        }

        std::uint64_t Value() const noexcept { return m_hash; }

    private:
        std::uint64_t m_hash = 0xcbf29ce484222325ull;
    };

    bool ReadsGeometry(const ProbeVolumeComponent& settings) noexcept
    {
        return settings.placement == ProbePlacement::UniformFloor || (settings.placement == ProbePlacement::Uniform3D && settings.filterInsideSolids);
    }

    Audio::ProbePlacementMode ToMode(ProbePlacement placement) noexcept
    {
        switch(placement)
        {
            case ProbePlacement::Centroid:
                return Audio::ProbePlacementMode::Centroid;
            case ProbePlacement::Uniform3D:
                return Audio::ProbePlacementMode::Uniform3D;
            case ProbePlacement::UniformFloor:
            default:
                return Audio::ProbePlacementMode::UniformFloor;
        }
    }
}

std::uint64_t Fingerprint(const SceneDocument& document, const SceneObjectDocument& volume, const SceneAccess& scene)
{
    Hasher hash;
    hash.Add(std::uint64_t{ProbeBatchAssets::FormatVersion});

    if(!volume.probeVolume)
    {
        return hash.Value();
    }

    const ProbeVolumeComponent& settings = *volume.probeVolume;
    hash.Add(static_cast<std::uint64_t>(settings.placement));

    for(const float value : settings.size)
    {
        hash.Add(value);
    }

    hash.Add(settings.probeSpacing);
    hash.Add(settings.heightAboveFloor);

    // Only when on, so unfiltered data generated before the setting existed keeps its fingerprint.
    if(settings.filterInsideSolids)
    {
        hash.Add(std::string_view("filter inside solids"));
    }

    hash.Add(static_cast<std::uint64_t>(settings.manualProbes.size()));

    for(const auto& probe : settings.manualProbes)
    {
        for(const float value : probe)
        {
            hash.Add(value);
        }
    }

    if(const auto world = scene.world(volume))
    {
        hash.Add(*world);
    }

    if(!ReadsGeometry(settings))
    {
        return hash.Value();
    }

    for(const SceneObjectDocument& object : document.objects)
    {
        if(!object.acoustics || !object.acoustics->enabled)
        {
            continue;
        }

        hash.Add(std::string_view(object.id));
        hash.Add(std::string_view(object.meshReference));

        if(const auto world = scene.world(object))
        {
            hash.Add(*world);
        }

        const ImportedMesh* mesh = scene.mesh(object);

        if(!mesh)
        {
            hash.Add(std::uint64_t{0});
            continue;
        }

        hash.Add(static_cast<std::uint64_t>(mesh->vertices.size()));
        hash.Add(static_cast<std::uint64_t>(mesh->indices.size()));

        for(const Vertex& vertex : mesh->vertices)
        {
            hash.Add(vertex.position[0]);
            hash.Add(vertex.position[1]);
            hash.Add(vertex.position[2]);
        }

        for(const auto index : mesh->indices)
        {
            hash.Add(static_cast<std::uint64_t>(index));
        }
    }

    return hash.Value();
}

const char* ToString(Status status) noexcept
{
    switch(status)
    {
        case Status::NotGenerated:
            return "Not generated";
        case Status::Valid:
            return "Valid";
        case Status::Stale:
            return "Stale";
        case Status::Missing:
            return "Missing";
        case Status::Invalid:
            return "Invalid";
    }

    return "Unknown";
}

GenerationEstimate Estimate(const ProbeVolumeComponent& volume, const Audio::AcousticTransform& world) noexcept
{
    GenerationEstimate estimate;
    double total = 1.0;

    if(volume.placement != ProbePlacement::Centroid)
    {
        if(volume.probeSpacing <= 0.0f)
        {
            return {};
        }

        for(const std::size_t axis : {std::size_t{0}, std::size_t{1}, std::size_t{2}})
        {
            // A floor grid has no vertical axis: each column gets a probe per surface it crosses.
            if(axis == 1 && volume.placement == ProbePlacement::UniformFloor)
            {
                continue;
            }

            const double length = std::sqrt(double(world[0][axis]) * world[0][axis] + double(world[1][axis]) * world[1][axis] +
                double(world[2][axis]) * world[2][axis]) * volume.size[axis];
            total *= std::floor(length / volume.probeSpacing) + 1.0;
        }
    }

    total += static_cast<double>(volume.manualProbes.size());
    estimate.probes = total >= 1e18 ? static_cast<std::uint64_t>(1e18) : static_cast<std::uint64_t>(total);
    estimate.exact = volume.placement == ProbePlacement::Centroid || (volume.placement == ProbePlacement::Uniform3D && !volume.filterInsideSolids);
    estimate.needsConfirmation = estimate.probes >= ConfirmationProbes;
    // Each stored probe takes its 16-byte record and at least as much again in the SDK's batch.
    estimate.fitsAsset = estimate.probes <= ProbeBatchAssets::MaximumBytes / 32;
    return estimate;
}

std::optional<Audio::ProbeGenerationRequest> BuildRequest(const SceneDocument& document, const SceneObjectDocument& volume, const SceneAccess& scene,
    std::vector<std::string>& skippedObjects, std::string& error)
{
    if(!volume.probeVolume)
    {
        error = "the object has no Probe Volume";
        return std::nullopt;
    }

    const auto world = scene.world(volume);

    if(!world)
    {
        error = "the volume's object is not in the running scene";
        return std::nullopt;
    }

    const ProbeVolumeComponent& settings = *volume.probeVolume;
    Audio::ProbeGenerationRequest request;
    request.placement = ToMode(settings.placement);
    request.world = *world;
    request.size = settings.size;
    request.spacing = settings.probeSpacing;
    request.height = settings.heightAboveFloor;
    request.manualProbes = settings.manualProbes;
    request.filterInsideSolids = settings.filterInsideSolids;

    if(ReadsGeometry(settings))
    {
        std::map<const ImportedMesh*, std::shared_ptr<const Audio::AcousticMeshData>> shared;

        for(const SceneObjectDocument& object : document.objects)
        {
            if(!object.acoustics || !object.acoustics->enabled)
            {
                continue;
            }

            const ImportedMesh* mesh = scene.mesh(object);
            const auto objectWorld = scene.world(object);

            if(!mesh || !objectWorld)
            {
                skippedObjects.push_back(object.name);
                continue;
            }

            auto& built = shared[mesh];

            if(!built)
            {
                // Placement and filtering read only where surfaces are, so the geometry needs no material assignments: none are resolved.
                AcousticGeometry::BuildReport report;
                built = AcousticGeometry::Build(*mesh, AcousticSettings{true, {}}, {}, report);
            }

            if(!built)
            {
                skippedObjects.push_back(object.name);
                continue;
            }

            request.geometry.push_back({built, *objectWorld});
        }
    }

    return request;
}

GenerationOutcome FinishGeneration(Audio::ProbeGenerationResult result, std::uint64_t fingerprint, std::vector<std::string> skippedObjects)
{
    GenerationOutcome outcome;
    outcome.skippedObjects = std::move(skippedObjects);
    outcome.cancelled = result.cancelled;
    outcome.closedPieces = result.closedPieces;
    outcome.openPieces = result.openPieces;
    outcome.enclosedByOneShell = result.enclosedByOneShell;

    if(!result.success)
    {
        outcome.error = std::move(result.error);
        return outcome;
    }

    outcome.success = true;
    outcome.duplicatesRemoved = result.duplicatesRemoved;
    outcome.data.fingerprint = fingerprint;
    outcome.data.rejectedProbes = static_cast<std::uint32_t>(result.rejectedProbes);
    outcome.data.ambiguousProbes = static_cast<std::uint32_t>(result.ambiguousProbes);
    outcome.data.batch = std::move(result.batch);
    outcome.data.probes.reserve(result.probes.size());

    for(const Audio::ProbeSphere& probe : result.probes)
    {
        outcome.data.probes.push_back({probe.center[0], probe.center[1], probe.center[2], probe.radius});
    }

    return outcome;
}

GenerationOutcome Generate(const Audio::AudioSystem& audio, const SceneDocument& document, const SceneObjectDocument& volume, const SceneAccess& scene)
{
    std::vector<std::string> skipped;
    std::string error;
    const auto request = BuildRequest(document, volume, scene, skipped, error);

    if(!request)
    {
        GenerationOutcome outcome;
        outcome.error = std::move(error);
        return outcome;
    }

    return FinishGeneration(audio.GenerateProbes(*request), Fingerprint(document, volume, scene), std::move(skipped));
}
}
