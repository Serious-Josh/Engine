#include "Audio/SteamAudioProbes.h"

#if defined(ENGINE_HAS_STEAMAUDIO)
#include "Audio/ProbeSolidClassifier.h"
#include "Audio/SteamAudioCoordinates.h"
#include "Audio/SteamAudioRuntime.h"
#include "assets/BakedReflectionsAsset.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <new>

// M8.2: baked reflection assets record the SDK and the simulator dimensions they were baked for.
static_assert(BakedReflectionsAssets::SteamAudioVersion == STEAMAUDIO_VERSION, "Baked reflection assets name another Steam Audio version.");
static_assert(BakedReflectionsAssets::SimulatorOrder == static_cast<std::uint32_t>(Audio::Internal::SteamAudioReflectionMaxOrder),
    "Bakes must use the simulator's reflection order.");
static_assert(BakedReflectionsAssets::SimulatorDuration == Audio::Internal::SteamAudioReflectionMaxDuration,
    "Bakes must use the simulator's reflection duration.");

namespace Audio::Internal
{
namespace
{
    constexpr float DuplicateDistance = 0.001f;

    std::array<float, 3> Transformed(const AcousticTransform& world, const std::array<float, 3>& point) noexcept
    {
        std::array<float, 3> result{};

        for(std::size_t row = 0; row < 3; ++row)
        {
            result[row] = world[row][0] * point[0] + world[row][1] * point[1] + world[row][2] * point[2] + world[row][3];
        }

        return result;
    }

    IPLVector3 ToSteamVector(const std::array<float, 3>& engine) noexcept
    {
        const SteamAudio::Vector converted = SteamAudio::ToSteamAudio(engine);
        return IPLVector3{converted[0], converted[1], converted[2]};
    }

    float ColumnLength(const AcousticTransform& matrix, std::size_t column) noexcept
    {
        return std::sqrt(matrix[0][column] * matrix[0][column] + matrix[1][column] * matrix[1][column] + matrix[2][column] * matrix[2][column]);
    }

    // The object's transform scaled by the volume's size: maps the SDK's cube, centred on the origin, onto the volume.
    AcousticTransform VolumeTransform(const ProbeGenerationRequest& request) noexcept
    {
        AcousticTransform volume = request.world;

        for(std::size_t row = 0; row < 3; ++row)
        {
            for(std::size_t column = 0; column < 3; ++column)
            {
                volume[row][column] *= request.size[column];
            }
        }

        return volume;
    }

    IPLMatrix4x4 ToIPLMatrix(const AcousticTransform& engine) noexcept
    {
        const AcousticTransform converted = SteamAudio::ToSteamAudioTransform(engine);
        IPLMatrix4x4 matrix{};

        for(std::size_t row = 0; row < 4; ++row)
        {
            for(std::size_t column = 0; column < 4; ++column)
            {
                matrix.elements[row][column] = converted[row][column];
            }
        }

        return matrix;
    }

    bool IsValid(const ProbeGenerationRequest& request, std::string& error)
    {
        const auto finite = [](float value)
        {
            return std::isfinite(value);
        };

        bool valid = finite(request.spacing) && finite(request.height);

        for(const auto& row : request.world)
        {
            for(const float value : row)
            {
                valid = valid && finite(value);
            }
        }

        for(const float value : request.size)
        {
            valid = valid && finite(value) && value > 0.0f;
        }

        if(!valid)
        {
            error = "the volume's size or transform is invalid";
            return false;
        }

        if(request.placement != ProbePlacementMode::Centroid && request.spacing <= 0.0f)
        {
            error = "the probe spacing must be positive";
            return false;
        }

        const AcousticTransform volume = VolumeTransform(request);

        if(ColumnLength(volume, 0) <= 0.0f || ColumnLength(volume, 1) <= 0.0f || ColumnLength(volume, 2) <= 0.0f)
        {
            error = "the volume has a zero scale";
            return false;
        }

        return true;
    }

    // RAII for the SDK's reference-counted handles, on every exit path.
    struct Handles
    {
        IPLScene scene = nullptr;
        std::vector<IPLStaticMesh> meshes;
        IPLProbeArray array = nullptr;
        IPLProbeBatch batch = nullptr;
        IPLSerializedObject serialized = nullptr;

        ~Handles()
        {
            if(serialized)
            {
                iplSerializedObjectRelease(&serialized);
            }

            if(batch)
            {
                iplProbeBatchRelease(&batch);
            }

            if(array)
            {
                iplProbeArrayRelease(&array);
            }

            for(IPLStaticMesh& mesh : meshes)
            {
                iplStaticMeshRelease(&mesh);
            }

            if(scene)
            {
                iplSceneRelease(&scene);
            }
        }
    };

    bool IsCancelled(const ProbeGenerationJob* job) noexcept
    {
        return job && job->IsCancelRequested();
    }

    void SetStage(ProbeGenerationJob* job, ProbeGenerationStage stage, std::uint64_t total = 0) noexcept
    {
        if(job)
        {
            job->SetStage(stage, total);
        }
    }

    constexpr const char* CancelledMessage = "generation was cancelled";

    bool BuildScene(IPLContext context, const ProbeGenerationRequest& request, Handles& handles, std::string& error, ProbeGenerationJob* job)
    {
        IPLSceneSettings sceneSettings{};
        sceneSettings.type = IPL_SCENETYPE_DEFAULT;

        if(iplSceneCreate(context, &sceneSettings, &handles.scene) != IPL_STATUS_SUCCESS)
        {
            error = "Steam Audio could not create the generation scene";
            return false;
        }

        for(const ProbeGeometryObject& object : request.geometry)
        {
            if(IsCancelled(job))
            {
                error = CancelledMessage;
                return false;
            }

            std::vector<IPLVector3> vertices;
            std::vector<IPLTriangle> triangles;
            std::vector<IPLint32> materialIndices(object.mesh->triangles.size(), 0);
            IPLMaterial material{};
            vertices.reserve(object.mesh->positions.size());
            triangles.reserve(object.mesh->triangles.size());

            for(const std::array<float, 3>& position : object.mesh->positions)
            {
                vertices.push_back(ToSteamVector(Transformed(object.world, position)));
            }

            for(const std::array<std::uint32_t, 3>& triangle : object.mesh->triangles)
            {
                // The Z mirror reverses winding, as in the simulation's own geometry.
                IPLTriangle converted{};
                converted.indices[0] = static_cast<IPLint32>(triangle[0]);
                converted.indices[1] = static_cast<IPLint32>(triangle[2]);
                converted.indices[2] = static_cast<IPLint32>(triangle[1]);
                triangles.push_back(converted);
            }

            IPLStaticMeshSettings meshSettings{};
            meshSettings.numVertices = static_cast<IPLint32>(vertices.size());
            meshSettings.numTriangles = static_cast<IPLint32>(triangles.size());
            meshSettings.numMaterials = 1;
            meshSettings.vertices = vertices.data();
            meshSettings.triangles = triangles.data();
            meshSettings.materialIndices = materialIndices.data();
            meshSettings.materials = &material;
            IPLStaticMesh mesh = nullptr;

            if(iplStaticMeshCreate(handles.scene, &meshSettings, &mesh) != IPL_STATUS_SUCCESS)
            {
                error = "Steam Audio could not accept the acoustic geometry";
                return false;
            }

            handles.meshes.push_back(mesh);
            iplStaticMeshAdd(mesh, handles.scene);

            // M8.3: scene construction reports real work units, one per object.
            if(job)
            {
                job->SetDone(handles.meshes.size());
            }
        }

        iplSceneCommit(handles.scene);
        return true;
    }

    // Not an SDK placement: the SDK offers only floor and centroid. Same counts and centring as its floor grid, on all three axes,
    // with no regard for geometry.
    std::vector<ProbeSphere> UniformGrid(const ProbeGenerationRequest& request, std::string& error, ProbeGenerationJob* job)
    {
        const AcousticTransform volume = VolumeTransform(request);
        std::array<float, 3> lengths{};
        std::array<std::array<float, 3>, 3> axes{};
        std::array<std::size_t, 3> counts{};

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            lengths[axis] = ColumnLength(volume, axis);
            counts[axis] = static_cast<std::size_t>(std::floor(lengths[axis] / request.spacing)) + 1;

            for(std::size_t row = 0; row < 3; ++row)
            {
                axes[axis][row] = volume[row][axis] / lengths[axis];
            }
        }

        // The batch indexes probes with an int32. Refused before allocating, whatever memory the machine has.
        const double total = static_cast<double>(counts[0]) * static_cast<double>(counts[1]) * static_cast<double>(counts[2]);

        if(total > static_cast<double>((std::numeric_limits<IPLint32>::max)()))
        {
            error = "this grid needs " + std::to_string(total) + " probes, more than Steam Audio can index";
            return {};
        }

        std::vector<ProbeSphere> probes;
        probes.reserve(counts[0] * counts[1] * counts[2]);

        for(std::size_t x = 0; x < counts[0]; ++x)
        {
            if(IsCancelled(job))
            {
                error = CancelledMessage;
                return {};
            }

            for(std::size_t y = 0; y < counts[1]; ++y)
            {
                for(std::size_t z = 0; z < counts[2]; ++z)
                {
                    const std::array<std::size_t, 3> at{x, y, z};
                    std::array<float, 3> center{volume[0][3], volume[1][3], volume[2][3]};

                    for(std::size_t axis = 0; axis < 3; ++axis)
                    {
                        const float offset = (static_cast<float>(at[axis]) - static_cast<float>(counts[axis] - 1) * 0.5f) * request.spacing;

                        for(std::size_t row = 0; row < 3; ++row)
                        {
                            center[row] += axes[axis][row] * offset;
                        }
                    }

                    probes.push_back({center, request.spacing});
                }
            }
        }

        return probes;
    }

    bool IsDuplicate(const std::vector<ProbeSphere>& probes, const std::array<float, 3>& center) noexcept
    {
        for(const ProbeSphere& probe : probes)
        {
            const float x = probe.center[0] - center[0];
            const float y = probe.center[1] - center[1];
            const float z = probe.center[2] - center[2];

            if(x * x + y * y + z * z <= DuplicateDistance * DuplicateDistance)
            {
                return true;
            }
        }

        return false;
    }

    std::vector<ProbeSphere> GeneratedBySdk(IPLContext context, const ProbeGenerationRequest& request, Handles& handles, std::string& error)
    {
        if(iplProbeArrayCreate(context, &handles.array) != IPL_STATUS_SUCCESS)
        {
            error = "Steam Audio could not create a probe array";
            return {};
        }

        IPLProbeGenerationParams params{};
        params.type = request.placement == ProbePlacementMode::Centroid ? IPL_PROBEGENERATIONTYPE_CENTROID : IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR;
        params.spacing = request.spacing;
        params.height = request.height;
        params.transform = ToIPLMatrix(VolumeTransform(request));
        iplProbeArrayGenerateProbes(handles.array, handles.scene, &params);
        const IPLint32 count = iplProbeArrayGetNumProbes(handles.array);
        std::vector<ProbeSphere> probes;
        probes.reserve(static_cast<std::size_t>(count));

        for(IPLint32 index = 0; index < count; ++index)
        {
            const IPLSphere sphere = iplProbeArrayGetProbe(handles.array, index);
            const SteamAudio::Vector engine = SteamAudio::FromSteamAudio({sphere.center.x, sphere.center.y, sphere.center.z});
            probes.push_back({engine, sphere.radius});
        }

        return probes;
    }
}

namespace
{
    // False when cancelled. Manual probes are added later, so the filter never sees them.
    bool FilterInsideSolids(const ProbeGenerationRequest& request, ProbeGenerationResult& result, ProbeGenerationJob* job)
    {
        SetStage(job, ProbeGenerationStage::Filtering, result.probes.size());
        const ProbeSolidClassifier classifier(request.geometry);
        result.closedPieces = classifier.GetClosedPieces();
        result.openPieces = classifier.GetOpenPieces();
        std::vector<ProbeSolidClassifier::Verdict> verdicts;
        verdicts.reserve(result.probes.size());
        std::size_t inside = 0;
        std::size_t ambiguous = 0;

        for(std::size_t index = 0; index < result.probes.size(); ++index)
        {
            if(IsCancelled(job))
            {
                return false;
            }

            verdicts.push_back(classifier.Classify(result.probes[index].center));
            inside += verdicts.back() == ProbeSolidClassifier::Verdict::Inside ? 1 : 0;
            ambiguous += verdicts.back() == ProbeSolidClassifier::Verdict::Ambiguous ? 1 : 0;

            if(job)
            {
                job->SetDone(index + 1);
            }
        }

        result.ambiguousProbes = ambiguous;

        // The whole box inside closed geometry: a solid that size leaves nothing useful to fill, whereas a room made from an
        // outward-facing box is common. They read identically, so nothing is rejected and the caller is told.
        if(inside > 0 && inside == result.probes.size())
        {
            const AcousticTransform volume = VolumeTransform(request);
            bool cornersInside = true;

            for(int corner = 0; corner < 8; ++corner)
            {
                const std::array<float, 3> local{corner & 1 ? 0.5f : -0.5f, corner & 2 ? 0.5f : -0.5f, corner & 4 ? 0.5f : -0.5f};
                cornersInside = cornersInside && classifier.Classify(Transformed(volume, local)) == ProbeSolidClassifier::Verdict::Inside;
            }

            if(cornersInside)
            {
                result.enclosedByOneShell = true;
                result.ambiguousProbes = result.probes.size();
                return true;
            }
        }

        std::vector<ProbeSphere> kept;
        kept.reserve(result.probes.size() - inside);

        for(std::size_t index = 0; index < result.probes.size(); ++index)
        {
            if(verdicts[index] != ProbeSolidClassifier::Verdict::Inside)
            {
                kept.push_back(result.probes[index]);
            }
        }

        result.rejectedProbes = inside;
        result.probes = std::move(kept);
        return true;
    }

    void GenerateProbeBatchUnchecked(IPLContext context, const ProbeGenerationRequest& request, ProbeGenerationJob* job, ProbeGenerationResult& result)
    {
        if(!IsValid(request, result.error))
        {
            return;
        }

        const auto cancelled = [&]
        {
            result.cancelled = IsCancelled(job);

            if(result.cancelled)
            {
                result.error = CancelledMessage;
                result.probes.clear();
            }

            return result.cancelled;
        };
        Handles handles;
        SetStage(job, ProbeGenerationStage::BuildingScene, request.geometry.size());

        if(request.placement == ProbePlacementMode::UniformFloor && !request.geometry.empty() && !BuildScene(context, request, handles, result.error, job))
        {
            cancelled();
            return;
        }

        if(cancelled())
        {
            return;
        }

        SetStage(job, ProbeGenerationStage::Placing);

        if(request.placement == ProbePlacementMode::Uniform3D)
        {
            result.probes = UniformGrid(request, result.error, job);

            if(!result.error.empty())
            {
                cancelled();
                return;
            }
        }
        else if(request.placement == ProbePlacementMode::Centroid || handles.scene)
        {
            // Centroid never reads the scene, but the SDK takes one.
            if(!handles.scene && !BuildScene(context, request, handles, result.error, job))
            {
                cancelled();
                return;
            }

            result.probes = GeneratedBySdk(context, request, handles, result.error);

            if(!result.error.empty())
            {
                result.probes.clear();
                return;
            }
        }

        if(cancelled())
        {
            return;
        }

        const std::size_t placed = result.probes.size();

        if(request.filterInsideSolids && request.placement != ProbePlacementMode::Centroid && !request.geometry.empty() && !FilterInsideSolids(request, result, job))
        {
            cancelled();
            return;
        }

        for(const std::array<float, 3>& local : request.manualProbes)
        {
            const std::array<float, 3> center = Transformed(request.world, local);

            if(IsDuplicate(result.probes, center))
            {
                ++result.duplicatesRemoved;
                continue;
            }

            result.probes.push_back({center, request.spacing > 0.0f ? request.spacing : 1.0f});
        }

        if(result.probes.empty())
        {
            if(placed > 0)
            {
                result.error = "all " + std::to_string(placed) + " probes were inside solid geometry. Move the volume, or turn off the inside-solid filter to keep them";
            }
            else
            {
                result.error = request.placement == ProbePlacementMode::UniformFloor ?
                    (request.geometry.empty() ? "no object has acoustics enabled, so there is no floor to place probes on" :
                    "no acoustic surface lies below the volume's top within its height") : "the placement produced no probes";
            }

            return;
        }

        if(result.probes.size() > static_cast<std::size_t>((std::numeric_limits<IPLint32>::max)()))
        {
            result.probes.clear();
            result.error = "this placement produced more probes than Steam Audio can index";
            return;
        }

        SetStage(job, ProbeGenerationStage::Packing, result.probes.size());

        if(iplProbeBatchCreate(context, &handles.batch) != IPL_STATUS_SUCCESS)
        {
            result.probes.clear();
            result.error = "Steam Audio could not create a probe batch";
            return;
        }

        // Everything goes in one at a time, so the batch and the reported list share one order.
        for(std::size_t index = 0; index < result.probes.size(); ++index)
        {
            if((index & 0xfff) == 0)
            {
                if(cancelled())
                {
                    return;
                }

                if(job)
                {
                    job->SetDone(index);
                }
            }

            const ProbeSphere& probe = result.probes[index];
            iplProbeBatchAddProbe(handles.batch, IPLSphere{ToSteamVector(probe.center), probe.radius});
        }

        iplProbeBatchCommit(handles.batch);
        IPLSerializedObjectSettings serializedSettings{};

        if(iplSerializedObjectCreate(context, &serializedSettings, &handles.serialized) != IPL_STATUS_SUCCESS)
        {
            result.probes.clear();
            result.error = "Steam Audio could not create a serialized object";
            return;
        }

        iplProbeBatchSave(handles.batch, handles.serialized);
        const IPLsize size = iplSerializedObjectGetSize(handles.serialized);
        const IPLbyte* data = iplSerializedObjectGetData(handles.serialized);

        if(size == 0 || !data)
        {
            result.probes.clear();
            result.error = "Steam Audio produced an empty probe batch";
            return;
        }

        result.batch.resize(size);
        std::memcpy(result.batch.data(), data, size);
        result.success = true;
    }
}

ProbeGenerationResult GenerateProbeBatch(IPLContext context, const ProbeGenerationRequest& request, ProbeGenerationJob* job) noexcept
{
    ProbeGenerationResult result;

    try
    {
        GenerateProbeBatchUnchecked(context, request, job, result);
    }
    catch(const std::bad_alloc&)
    {
        result = {};
        result.error = "there is not enough memory for this many probes";
    }
    catch(const std::exception& exception)
    {
        result = {};
        result.error = exception.what();
    }

    return result;
}

SteamAudioProbeBatches::~SteamAudioProbeBatches()
{
    Clear();
}

IPLProbeBatch LoadCommittedProbeBatch(IPLContext context, std::span<const std::byte> batch, std::size_t expectedProbes, std::string& error) noexcept
{
    try
    {
        std::vector<IPLbyte> bytes(batch.size());
        std::memcpy(bytes.data(), batch.data(), batch.size());
        IPLSerializedObjectSettings settings{};
        settings.data = bytes.data();
        settings.size = bytes.size();
        IPLSerializedObject serialized = nullptr;
        IPLProbeBatch loaded = nullptr;

        if(iplSerializedObjectCreate(context, &settings, &serialized) != IPL_STATUS_SUCCESS)
        {
            error = "Steam Audio could not read the probe batch";
            return nullptr;
        }

        const IPLerror status = iplProbeBatchLoad(context, serialized, &loaded);
        iplSerializedObjectRelease(&serialized);

        if(status != IPL_STATUS_SUCCESS || !loaded || static_cast<std::size_t>(iplProbeBatchGetNumProbes(loaded)) != expectedProbes)
        {
            if(loaded)
            {
                iplProbeBatchRelease(&loaded);
            }

            error = "Steam Audio could not load the probe batch";
            return nullptr;
        }

        iplProbeBatchCommit(loaded);
        return loaded;
    }
    catch(const std::exception&)
    {
        error = "there is not enough memory to load the probe batch";
        return nullptr;
    }
}

bool SteamAudioProbeBatches::Load(IPLContext context, std::span<const std::byte> batch, std::size_t expectedProbes, std::string& error)
{
    IPLProbeBatch loaded = LoadCommittedProbeBatch(context, batch, expectedProbes, error);

    if(!loaded)
    {
        ++m_stats.loadFailures;
        return false;
    }

    m_batches.push_back(loaded);
    ++m_stats.batchesCreated;
    m_stats.loadedBatches = m_batches.size();
    m_stats.loadedProbes += expectedProbes;
    return true;
}

void SteamAudioProbeBatches::Clear() noexcept
{
    for(IPLProbeBatch& batch : m_batches)
    {
        iplProbeBatchRelease(&batch);
        ++m_stats.batchesReleased;
    }

    m_batches.clear();
    m_stats.loadedBatches = 0;
    m_stats.loadedProbes = 0;
}

ProbeBatchStats SteamAudioProbeBatches::GetStats() const noexcept
{
    return m_stats;
}
}
#endif
