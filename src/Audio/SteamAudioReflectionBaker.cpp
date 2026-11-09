#include "Audio/SteamAudioProbes.h"

#if defined(ENGINE_HAS_STEAMAUDIO)
#include "Audio/SteamAudioCoordinates.h"
#include "Scene/AudioComponents.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <thread>

// M8.3: offline reflection baking. Every SDK object here is private to one call, so nothing is shared with the simulation but the
// context. M8.1's findings shape it:
// * iplReflectionsBakerBake returns nothing and fails silently, so the inputs are checked first and every layer's data afterwards.
// * Cancelling leaves a partial layer behind, so the bake runs on a batch loaded from the request's bytes and is discarded whole.
// * iplReflectionsBakerCancelBake is context-wide; called from another thread it can reach the next bake. It is called only from
//   the progress callback, while this bake is the one running, and at most once per layer.
// * A loaded batch is committed before use.
// * The ray tracer is the session's (SteamAudioSimulation::SelectRayTracer): Embree only when the project asked for it and the device
//   passed its startup check, otherwise the default. The baked data is the same format either way, so assets are interchangeable.
//
// Interruptible: between objects while building the scene, at each probe while baking, and between layers. Not interruptible:
// one static mesh creation, the scene commit, the batch load, one serialization and the read-back check. Each is bounded by the
// size of one object or one batch.
namespace Audio::Internal
{
namespace
{
    constexpr const char* CancelledMessage = "baking was cancelled";

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

    IPLBakedDataIdentifier ToIdentifier(const ReflectionLayerIdentifier& layer) noexcept
    {
        IPLBakedDataIdentifier identifier{};
        identifier.type = IPL_BAKEDDATATYPE_REFLECTIONS;

        if(layer.kind == ReflectionLayerKind::Reverb)
        {
            // All zeros, whatever the request holds: the SDK compares the sphere even for this variation (M8.1).
            identifier.variation = IPL_BAKEDDATAVARIATION_REVERB;
            return identifier;
        }

        identifier.variation = IPL_BAKEDDATAVARIATION_STATICSOURCE;
        identifier.endpointInfluence = IPLSphere{IPLVector3{layer.centre[0], layer.centre[1], layer.centre[2]}, layer.radius};
        return identifier;
    }

    struct Handles
    {
        IPLScene scene = nullptr;
        std::vector<IPLStaticMesh> meshes;
        IPLProbeBatch batch = nullptr;
        IPLProbeBatch check = nullptr;
        IPLSerializedObject serialized = nullptr;

        ~Handles()
        {
            if(serialized)
            {
                iplSerializedObjectRelease(&serialized);
            }

            if(check)
            {
                iplProbeBatchRelease(&check);
            }

            if(batch)
            {
                iplProbeBatchRelease(&batch);
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

    bool IsCancelled(const AcousticJob* job) noexcept
    {
        return job && job->IsCancelRequested();
    }

    void SetStage(ReflectionBakeJob* job, ReflectionBakeStage stage, std::uint64_t total = 0) noexcept
    {
        if(job)
        {
            job->SetStage(stage, total);
        }
    }

    bool Finite(float value) noexcept
    {
        return std::isfinite(value);
    }

    bool ValidGeometry(const std::vector<ProbeGeometryObject>& geometry, std::string& error);

    bool IsValid(const ReflectionBakeRequest& request, std::string& error)
    {
        if(request.batch.empty() || request.expectedProbes == 0)
        {
            error = "there are no probes to bake";
            return false;
        }

        if(request.expectedProbes > static_cast<std::size_t>((std::numeric_limits<IPLint32>::max)()))
        {
            error = "the batch holds more probes than Steam Audio can index";
            return false;
        }

        // The SDK bakes a silent layer from zero rays or samples without complaint (M8.1).
        if(request.rays == 0 || request.diffuseSamples == 0 || request.bounces == 0 || request.order == 0 ||
            !Finite(request.irradianceMinimumDistance) || request.irradianceMinimumDistance <= 0.0f || !Finite(request.duration) ||
            request.duration <= 0.0f)
        {
            error = "the bake settings are invalid";
            return false;
        }

        const auto limit = static_cast<std::uint32_t>((std::numeric_limits<IPLint32>::max)());

        if(request.rays > limit || request.diffuseSamples > limit || request.bounces > limit || request.order > limit)
        {
            error = "the bake settings are out of range";
            return false;
        }

        if(request.bake.empty() && request.keep.empty())
        {
            error = "the bake has no layers";
            return false;
        }

        for(const auto* list : {&request.remove, &request.bake, &request.keep})
        {
            for(const ReflectionLayerIdentifier& layer : *list)
            {
                if(layer.kind == ReflectionLayerKind::StaticSource && (!Finite(layer.centre[0]) || !Finite(layer.centre[1]) ||
                    !Finite(layer.centre[2]) || !Finite(layer.radius) || layer.radius <= 0.0f))
                {
                    error = "a static source's position is invalid";
                    return false;
                }
            }
        }

        return ValidGeometry(request.geometry, error);
    }

    // Shared by reflection and pathing bakes (M9.4).
    bool ValidGeometry(const std::vector<ProbeGeometryObject>& geometry, std::string& error)
    {
        for(const ProbeGeometryObject& object : geometry)
        {
            if(!object.mesh)
            {
                error = "an acoustic object has no geometry";
                return false;
            }

            const AcousticMeshData& mesh = *object.mesh;

            if(mesh.triangleMaterials.size() != mesh.triangles.size() || mesh.materials.empty() ||
                mesh.positions.size() > static_cast<std::size_t>((std::numeric_limits<IPLint32>::max)()) ||
                mesh.triangles.size() > static_cast<std::size_t>((std::numeric_limits<IPLint32>::max)()))
            {
                error = "an acoustic mesh is malformed";
                return false;
            }

            for(const std::int32_t material : mesh.triangleMaterials)
            {
                if(material < 0 || static_cast<std::size_t>(material) >= mesh.materials.size())
                {
                    error = "an acoustic mesh names a material it does not have";
                    return false;
                }
            }

            for(const auto& triangle : mesh.triangles)
            {
                for(const std::uint32_t index : triangle)
                {
                    if(index >= mesh.positions.size())
                    {
                        error = "an acoustic mesh names a vertex it does not have";
                        return false;
                    }
                }
            }
        }

        return true;
    }

    // World-space vertices in one default-ray-tracer scene, each mesh with its own materials. Same conversion and winding as the
    // simulation's geometry.
    IPLSceneType SceneType(IPLEmbreeDevice embree) noexcept
    {
        return embree ? IPL_SCENETYPE_EMBREE : IPL_SCENETYPE_DEFAULT;
    }

    // Any acoustic job with SetDone: reflection and pathing bakes build the same scene (M9.4).
    template<typename Job>
    bool BuildScene(IPLContext context, IPLEmbreeDevice embree, const std::vector<ProbeGeometryObject>& geometry, Handles& handles,
        std::string& error, Job* job)
    {
        IPLSceneSettings sceneSettings{};
        sceneSettings.type = SceneType(embree);
        sceneSettings.embreeDevice = embree;

        if(iplSceneCreate(context, &sceneSettings, &handles.scene) != IPL_STATUS_SUCCESS)
        {
            handles.scene = nullptr;
            error = "Steam Audio could not create the baking scene";
            return false;
        }

        for(std::size_t index = 0; index < geometry.size(); ++index)
        {
            if(IsCancelled(job))
            {
                error = CancelledMessage;
                return false;
            }

            const ProbeGeometryObject& object = geometry[index];
            const AcousticMeshData& mesh = *object.mesh;
            std::vector<IPLVector3> vertices;
            std::vector<IPLTriangle> triangles;
            std::vector<IPLMaterial> materials;
            std::vector<IPLint32> materialIndices(mesh.triangleMaterials.begin(), mesh.triangleMaterials.end());
            vertices.reserve(mesh.positions.size());
            triangles.reserve(mesh.triangles.size());
            materials.reserve(mesh.materials.size());

            for(const std::array<float, 3>& position : mesh.positions)
            {
                vertices.push_back(ToSteamVector(Transformed(object.world, position)));
            }

            for(const std::array<std::uint32_t, 3>& triangle : mesh.triangles)
            {
                IPLTriangle converted{};
                converted.indices[0] = static_cast<IPLint32>(triangle[0]);
                converted.indices[1] = static_cast<IPLint32>(triangle[2]);
                converted.indices[2] = static_cast<IPLint32>(triangle[1]);
                triangles.push_back(converted);
            }

            for(const AcousticMaterialValues& values : mesh.materials)
            {
                IPLMaterial material{};

                for(int band = 0; band < 3; ++band)
                {
                    material.absorption[band] = values.absorption[band];
                    material.transmission[band] = values.transmission[band];
                }

                material.scattering = values.scattering;
                materials.push_back(material);
            }

            IPLStaticMeshSettings meshSettings{};
            meshSettings.numVertices = static_cast<IPLint32>(vertices.size());
            meshSettings.numTriangles = static_cast<IPLint32>(triangles.size());
            meshSettings.numMaterials = static_cast<IPLint32>(materials.size());
            meshSettings.vertices = vertices.data();
            meshSettings.triangles = triangles.data();
            meshSettings.materialIndices = materialIndices.data();
            meshSettings.materials = materials.data();
            IPLStaticMesh created = nullptr;

            if(iplStaticMeshCreate(handles.scene, &meshSettings, &created) != IPL_STATUS_SUCCESS)
            {
                error = "Steam Audio could not accept the acoustic geometry";
                return false;
            }

            handles.meshes.push_back(created);
            iplStaticMeshAdd(created, handles.scene);

            if(job)
            {
                job->SetDone(index + 1);
            }
        }

        iplSceneCommit(handles.scene);
        return true;
    }

    bool Load(IPLContext context, std::span<const std::byte> bytes, IPLProbeBatch& batch)
    {
        // The SDK takes a mutable pointer; it gets a copy.
        std::vector<IPLbyte> copy(bytes.size());
        std::memcpy(copy.data(), bytes.data(), bytes.size());
        IPLSerializedObjectSettings settings{};
        settings.data = copy.data();
        settings.size = copy.size();
        IPLSerializedObject serialized = nullptr;

        if(iplSerializedObjectCreate(context, &settings, &serialized) != IPL_STATUS_SUCCESS)
        {
            return false;
        }

        const IPLerror status = iplProbeBatchLoad(context, serialized, &batch);
        iplSerializedObjectRelease(&serialized);

        if(status != IPL_STATUS_SUCCESS || !batch)
        {
            batch = nullptr;
            return false;
        }

        iplProbeBatchCommit(batch);
        return true;
    }

    struct ProgressState
    {
        IPLContext context = nullptr;
        ReflectionBakeJob* job = nullptr;
        std::uint64_t probes = 0;
        bool cancelIssued = false;
    };

    // Runs on the baking thread, between probes. The SDK's cancel takes effect when this returns.
    void IPLCALL BakeProgress(IPLfloat32 progress, void* userData)
    {
        auto& state = *static_cast<ProgressState*>(userData);

        if(!state.job)
        {
            return;
        }

        const double fraction = std::clamp(static_cast<double>(progress), 0.0, 1.0);
        state.job->SetDone(static_cast<std::uint64_t>(std::llround(fraction * static_cast<double>(state.probes))));

        if(!state.cancelIssued && state.job->IsCancelRequested())
        {
            state.cancelIssued = true;
            iplReflectionsBakerCancelBake(state.context);
        }
    }

    std::uint32_t ThreadCount(std::uint32_t requested) noexcept
    {
        if(requested > 0)
        {
            return requested;
        }

        // Half the machine, so the Editor stays responsive while it bakes.
        return (std::max)(1u, std::thread::hardware_concurrency() / 2);
    }

    void BakeUnchecked(IPLContext context, IPLEmbreeDevice embree, const ReflectionBakeRequest& request, ReflectionBakeJob* job,
        ReflectionBakeResult& result)
    {
        if(!IsValid(request, result.error))
        {
            return;
        }

        const auto cancelled = [&]
        {
            if(!IsCancelled(job))
            {
                return false;
            }

            result = {};
            result.cancelled = true;
            result.error = CancelledMessage;
            return true;
        };
        Handles handles;
        SetStage(job, ReflectionBakeStage::BuildingScene, request.geometry.size());

        if(!BuildScene(context, embree, request.geometry, handles, result.error, job))
        {
            cancelled();
            return;
        }

        if(cancelled())
        {
            return;
        }

        SetStage(job, ReflectionBakeStage::LoadingProbes);

        if(!Load(context, request.batch, handles.batch) ||
            static_cast<std::size_t>(iplProbeBatchGetNumProbes(handles.batch)) != request.expectedProbes)
        {
            result.error = "Steam Audio could not load the probes to bake";
            return;
        }

        for(const ReflectionLayerIdentifier& layer : request.remove)
        {
            IPLBakedDataIdentifier identifier = ToIdentifier(layer);
            iplProbeBatchRemoveData(handles.batch, &identifier);
        }

        for(const ReflectionLayerIdentifier& layer : request.keep)
        {
            IPLBakedDataIdentifier identifier = ToIdentifier(layer);

            if(iplProbeBatchGetDataSize(handles.batch, &identifier) == 0)
            {
                result.error = "a layer to keep is not in the existing bake";
                return;
            }
        }

        ProgressState progress;
        progress.context = context;
        progress.job = job;
        progress.probes = request.expectedProbes;

        for(std::size_t index = 0; index < request.bake.size(); ++index)
        {
            if(cancelled())
            {
                return;
            }

            if(job)
            {
                job->BeginLayer(index, request.expectedProbes);
            }

            IPLReflectionsBakeParams params{};
            params.scene = handles.scene;
            params.probeBatch = handles.batch;
            params.sceneType = SceneType(embree);
            params.identifier = ToIdentifier(request.bake[index]);
            params.bakeFlags = IPL_REFLECTIONSBAKEFLAGS_BAKECONVOLUTION;
            params.numRays = static_cast<IPLint32>(request.rays);
            params.numDiffuseSamples = static_cast<IPLint32>(request.diffuseSamples);
            params.numBounces = static_cast<IPLint32>(request.bounces);
            params.simulatedDuration = request.duration;
            params.savedDuration = request.duration;
            params.order = static_cast<IPLint32>(request.order);
            params.numThreads = static_cast<IPLint32>(ThreadCount(request.threads));
            params.irradianceMinDistance = request.irradianceMinimumDistance;
            params.bakeBatchSize = 1;
            // The baker overwrites a layer in place, but a stale one was removed above, so a short bake can't leave old data mixed in.
            iplProbeBatchRemoveData(handles.batch, &params.identifier);
            progress.cancelIssued = false;
            iplReflectionsBakerBake(context, &params, &BakeProgress, &progress);

            // Whether or not the SDK saw the cancel in time, a layer baked across the request is not trusted to be whole.
            if(cancelled())
            {
                return;
            }

            const IPLsize bytes = iplProbeBatchGetDataSize(handles.batch, &params.identifier);

            if(bytes == 0)
            {
                result.error = "Steam Audio produced no data for layer " + std::to_string(index + 1) + " of " +
                    std::to_string(request.bake.size());
                return;
            }

            result.bakedBytes.push_back(bytes);

            if(job)
            {
                job->SetDone(request.expectedProbes);
                job->EndLayer();
            }
        }

        if(cancelled())
        {
            return;
        }

        SetStage(job, ReflectionBakeStage::Validating, request.keep.size());

        for(const ReflectionLayerIdentifier& layer : request.keep)
        {
            IPLBakedDataIdentifier identifier = ToIdentifier(layer);
            const IPLsize bytes = iplProbeBatchGetDataSize(handles.batch, &identifier);

            if(bytes == 0)
            {
                result.error = "a kept layer was lost while baking";
                return;
            }

            result.keptBytes.push_back(bytes);
        }

        if(cancelled())
        {
            return;
        }

        SetStage(job, ReflectionBakeStage::Serializing);
        IPLSerializedObjectSettings serializedSettings{};

        if(iplSerializedObjectCreate(context, &serializedSettings, &handles.serialized) != IPL_STATUS_SUCCESS)
        {
            result.error = "Steam Audio could not create a serialized object";
            return;
        }

        iplProbeBatchSave(handles.batch, handles.serialized);
        const IPLsize size = iplSerializedObjectGetSize(handles.serialized);
        const IPLbyte* data = iplSerializedObjectGetData(handles.serialized);

        if(size == 0 || !data)
        {
            result.error = "Steam Audio produced an empty baked batch";
            return;
        }

        result.batch.resize(size);
        std::memcpy(result.batch.data(), data, size);

        // Read back what will be stored: a batch that saves but does not load must never be published.
        if(!Load(context, result.batch, handles.check) ||
            static_cast<std::size_t>(iplProbeBatchGetNumProbes(handles.check)) != request.expectedProbes)
        {
            result.batch.clear();
            result.error = "the baked batch does not load back";
            return;
        }

        const auto matches = [&](const std::vector<ReflectionLayerIdentifier>& layers, const std::vector<std::uint64_t>& sizes)
        {
            for(std::size_t index = 0; index < layers.size(); ++index)
            {
                IPLBakedDataIdentifier identifier = ToIdentifier(layers[index]);

                if(iplProbeBatchGetDataSize(handles.check, &identifier) != sizes[index])
                {
                    return false;
                }
            }

            return true;
        };

        if(!matches(request.bake, result.bakedBytes) || !matches(request.keep, result.keptBytes))
        {
            result.batch.clear();
            result.error = "the baked batch reads back with different layers";
            return;
        }

        if(cancelled())
        {
            return;
        }

        result.success = true;
    }
}

ReflectionBakeResult BakeReflectionLayers(IPLContext context, IPLEmbreeDevice embree, const ReflectionBakeRequest& request,
    ReflectionBakeJob* job) noexcept
{
    ReflectionBakeResult result;

    try
    {
        BakeUnchecked(context, embree, request, job, result);
    }
    catch(const std::bad_alloc&)
    {
        result = {};
        result.error = "there is not enough memory to bake these reflections";
    }
    catch(const std::exception& exception)
    {
        result = {};
        result.error = exception.what();
    }

    if(!result.success)
    {
        result.batch.clear();
        result.bakedBytes.clear();
        result.keptBytes.clear();
    }

    result.rayTracer = embree ? SteamAudioRayTracer::Embree : SteamAudioRayTracer::Default;
    return result;
}

// M9.4: pathing. Same discipline as reflections: the bake runs on a private copy of the probe batch, a cancelled bake is discarded
// whole, and only a batch that saves, loads back and still holds the layer is returned.
namespace
{
    IPLBakedDataIdentifier PathingIdentifier() noexcept
    {
        IPLBakedDataIdentifier identifier{};
        identifier.type = IPL_BAKEDDATATYPE_PATHING;
        identifier.variation = IPL_BAKEDDATAVARIATION_DYNAMIC;
        return identifier;
    }

    bool IsValid(const PathingBakeRequest& request, std::string& error)
    {
        if(request.batch.empty() || request.expectedProbes == 0)
        {
            error = "there are no probes to bake";
            return false;
        }

        if(request.expectedProbes > static_cast<std::size_t>((std::numeric_limits<IPLint32>::max)()))
        {
            error = "the batch holds more probes than Steam Audio can index";
            return false;
        }

        PathingBakeSettings settings;
        settings.samples = request.samples;
        settings.radius = request.radius;
        settings.threshold = request.threshold;
        settings.visibilityRange = request.visibilityRange;
        settings.pathRange = request.pathRange;

        if(!ValidatePathingBakeSettings(settings, &error))
        {
            return false;
        }

        return ValidGeometry(request.geometry, error);
    }

    // On the baking thread: progress only. Never cancels. Steam Audio 4.8.1 crashes any pathing bake that iplPathBakerCancelBake reaches,
    // from this callback or another thread, on both ray tracers (M9.4: access violation on Default, abort on Embree). So the bake is one
    // uninterruptible SDK call, and a cancel requested during it is honoured when it returns, by discarding the result.
    void IPLCALL PathingBakeProgress(IPLfloat32 progress, void* userData)
    {
        if(auto* const job = static_cast<PathingBakeJob*>(userData))
        {
            job->SetDone(static_cast<std::uint64_t>(std::llround(std::clamp(static_cast<double>(progress), 0.0, 1.0) * 1000.0)));
        }
    }

    void BakePathingUnchecked(IPLContext context, IPLEmbreeDevice embree, const PathingBakeRequest& request, PathingBakeJob* job,
        PathingBakeResult& result)
    {
        if(!IsValid(request, result.error))
        {
            return;
        }

        const auto cancelled = [&]
        {
            if(!IsCancelled(job))
            {
                return false;
            }

            result = {};
            result.cancelled = true;
            result.error = CancelledMessage;
            return true;
        };
        const auto stage = [&](PathingBakeStage value, std::uint64_t total = 0)
        {
            if(job)
            {
                job->SetStage(value, total);
            }
        };
        Handles handles;
        stage(PathingBakeStage::BuildingScene, request.geometry.size());

        if(!BuildScene(context, embree, request.geometry, handles, result.error, job))
        {
            cancelled();
            return;
        }

        if(cancelled())
        {
            return;
        }

        stage(PathingBakeStage::LoadingProbes);

        if(!Load(context, request.batch, handles.batch) ||
            static_cast<std::size_t>(iplProbeBatchGetNumProbes(handles.batch)) != request.expectedProbes)
        {
            result.error = "Steam Audio could not load the probes to bake";
            return;
        }

        IPLBakedDataIdentifier identifier = PathingIdentifier();
        // Whatever the copy held under this identifier is replaced, never mixed with the new bake.
        iplProbeBatchRemoveData(handles.batch, &identifier);
        // The last chance to stop before the uninterruptible call.
        if(cancelled())
        {
            return;
        }

        stage(PathingBakeStage::Baking, 1000);
        IPLPathBakeParams params{};
        params.scene = handles.scene;
        params.probeBatch = handles.batch;
        params.identifier = identifier;
        params.numSamples = static_cast<IPLint32>(request.samples);
        params.radius = request.radius;
        params.threshold = request.threshold;
        params.visRange = request.visibilityRange;
        params.pathRange = request.pathRange;
        params.numThreads = static_cast<IPLint32>(ThreadCount(request.threads));
        // A null progress callback crashes the bake (M9.0).
        iplPathBakerBake(context, &params, &PathingBakeProgress, job);

        // A cancel requested while it ran: the finished layer is discarded with the copy.
        if(cancelled())
        {
            return;
        }

        result.layerBytes = iplProbeBatchGetDataSize(handles.batch, &identifier);

        if(result.layerBytes == 0)
        {
            result.error = "Steam Audio produced no pathing data";
            return;
        }

        stage(PathingBakeStage::Serializing);
        IPLSerializedObjectSettings serializedSettings{};

        if(iplSerializedObjectCreate(context, &serializedSettings, &handles.serialized) != IPL_STATUS_SUCCESS)
        {
            result.error = "Steam Audio could not create a serialized object";
            return;
        }

        iplProbeBatchSave(handles.batch, handles.serialized);
        const IPLsize size = iplSerializedObjectGetSize(handles.serialized);
        const IPLbyte* data = iplSerializedObjectGetData(handles.serialized);

        if(size == 0 || !data)
        {
            result.error = "Steam Audio produced an empty baked batch";
            return;
        }

        result.batch.resize(size);
        std::memcpy(result.batch.data(), data, size);

        if(!Load(context, result.batch, handles.check) ||
            static_cast<std::size_t>(iplProbeBatchGetNumProbes(handles.check)) != request.expectedProbes ||
            iplProbeBatchGetDataSize(handles.check, &identifier) != result.layerBytes)
        {
            result.batch.clear();
            result.error = "the baked batch does not load back with its pathing layer";
            return;
        }

        if(cancelled())
        {
            return;
        }

        result.success = true;
    }
}

PathingBakeResult BakePathingLayer(IPLContext context, IPLEmbreeDevice embree, const PathingBakeRequest& request, PathingBakeJob* job) noexcept
{
    PathingBakeResult result;

    try
    {
        BakePathingUnchecked(context, embree, request, job, result);
    }
    catch(const std::bad_alloc&)
    {
        result = {};
        result.error = "there is not enough memory to bake pathing";
    }
    catch(const std::exception& exception)
    {
        result = {};
        result.error = exception.what();
    }

    if(!result.success)
    {
        result.batch.clear();
        result.layerBytes = 0;
    }

    result.rayTracer = embree ? SteamAudioRayTracer::Embree : SteamAudioRayTracer::Default;
    return result;
}
}
#endif
