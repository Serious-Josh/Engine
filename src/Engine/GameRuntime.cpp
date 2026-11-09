#include "Engine/ProbeVolumes.h"
#include "Engine/GameRuntime.h"
#include "Engine/BakedReverbRuntime.h"
#include "Engine/PathingBakes.h"
#include "Engine/ReflectionBakes.h"
#include "Engine/AcousticGeometry.h"

#include <map>
#include <utility>
#include "Renderer/Renderer.h"
#include "Core/Log.h"
#include "Core/Input.h"

#include <Windows.h>

#include <algorithm>
#include <map>

namespace
{
std::string DescribeDiagnostic(const Scripting::Diagnostic& diagnostic)
{
    std::string location = diagnostic.section.empty() ? std::string("script") : diagnostic.section;

    if(diagnostic.line > 0)
    {
        location += "(" + std::to_string(diagnostic.line) + "," + std::to_string(diagnostic.column) + ")";
    }

    return location + ": " + diagnostic.severity + ": " + diagnostic.text;
}

// Logs every diagnostic and returns "<context>: <first error>" for the host's status message.
// AngelScript precedes compile errors with informational "Compiling ..." lines; they are logged
// but never chosen as the summary while an error exists.
std::string ReportScriptResult(const Scripting::Result& result, const std::string& context)
{
    std::string first;
    std::string firstError;

    for(const auto& diagnostic : result.diagnostics)
    {
        const auto text = DescribeDiagnostic(diagnostic);
        Log::Error(context + ": " + text);

        if(first.empty() && diagnostic.severity != "Stack")
        {
            first = text;
        }

        if(firstError.empty() && diagnostic.severity == "Error")
        {
            firstError = text;
        }
    }

    const auto& summary = firstError.empty() ? first : firstError;
    return summary.empty() ? context + "." : context + ": " + summary;
}

bool Succeeded(const Scripting::Result& result)
{
    return result.status == Scripting::Status::Finished;
}
}

bool GameRuntime::ObjectTable::IsValid(std::uint32_t slot) const
{
    return Find(slot) != nullptr;
}

const SceneObjectDocument* GameRuntime::ObjectTable::Find(std::uint32_t slot) const
{
    if(!runtime || runtime->GetRuntimeIdentity() != runtimeIdentity || slot >= objectIDs.size())
    {
        return nullptr;
    }

    const auto& objects = runtime->GetFileScene().GetDocument().objects;
    const auto found = std::find_if(objects.begin(), objects.end(),
        [&](const SceneObjectDocument& object) { return object.id == objectIDs[slot]; });
    return found == objects.end() ? nullptr : &*found;
}

std::string GameRuntime::ObjectTable::GetObjectKey(std::uint32_t slot) const
{
    return Find(slot) ? objectIDs[slot] : std::string();
}

std::string GameRuntime::ObjectTable::GetName(std::uint32_t slot) const
{
    const auto* object = Find(slot);
    return object ? object->name : std::string();
}

bool GameRuntime::ObjectTable::GetPosition(std::uint32_t slot, float (&position)[3]) const
{
    if(!Find(slot))
    {
        return false;
    }

    // The accepted presentation pose: the current physics pose during gameplay.
    const auto& mapping = runtime->GetObjectMapping().idToRuntimeIndex;
    const auto found = mapping.find(objectIDs[slot]);
    const auto& objects = runtime->GetScene().GetRenderObjects();

    if(found == mapping.end() || found->second >= objects.size())
    {
        return false;
    }

    const Matrix4 world = objects[found->second].GetPresentationTransform().currentModel;
    position[0] = world.m[0][3];
    position[1] = world.m[1][3];
    position[2] = world.m[2][3];
    return true;
}

bool GameRuntime::ObjectTable::HasCharacterController(std::uint32_t slot) const
{
    // Controllers exist only in the live simulation the session started.
    const auto* object = Find(slot);
    return object && object->characterController && owner && owner->IsActive();
}

bool GameRuntime::ObjectTable::MoveCharacter(std::uint32_t slot, const float (&velocity)[3], std::string& error)
{
    if(!HasCharacterController(slot) || slot >= requests.size())
    {
        error = "CharacterController is not valid.";
        return false;
    }

    if(!std::isfinite(velocity[0]) || !std::isfinite(velocity[1]) || !std::isfinite(velocity[2]))
    {
        error = "CharacterController.Move velocity must be finite.";
        return false;
    }

    if(velocity[1] != 0.0f)
    {
        error = "CharacterController.Move takes a horizontal velocity: y must be 0.";
        return false;
    }

    // The latest request of the frame wins; the native controller enforces its speed limit.
    requests[slot].velocity = {velocity[0], 0.0f, velocity[2]};
    requests[slot].moving = true;
    return true;
}

bool GameRuntime::ObjectTable::JumpCharacter(std::uint32_t slot, std::string& error)
{
    if(!HasCharacterController(slot) || slot >= requests.size())
    {
        error = "CharacterController is not valid.";
        return false;
    }

    requests[slot].jump = true;
    return true;
}

bool GameRuntime::ObjectTable::IsCharacterGrounded(std::uint32_t slot) const
{
    if(!HasCharacterController(slot))
    {
        return false;
    }

    // Published diagnostics describe the presented poses, never a newer unpublished step.
    const auto* diagnostics = owner->m_physics.GetPublishedCharacterDiagnostics();
    const auto* controller = diagnostics && diagnostics->sessionGeneration == owner->m_physicsSession ?
        diagnostics->Find(objectIDs[slot]) : nullptr;
    return controller && controller->ground.grounded;
}

namespace
{
int VirtualKey(Scripting::Key key)
{
    switch(key)
    {
    case Scripting::Key::W:
        return 'W';
    case Scripting::Key::A:
        return 'A';
    case Scripting::Key::S:
        return 'S';
    case Scripting::Key::D:
        return 'D';
    case Scripting::Key::Space:
        return VK_SPACE;
    }

    return 0;
}
}

std::optional<RenderObjectID> GameRuntime::ObjectTable::RuntimeIndex(std::uint32_t slot) const
{
    if(!Find(slot))
    {
        return std::nullopt;
    }

    const auto& mapping = runtime->GetObjectMapping().idToRuntimeIndex;
    const auto found = mapping.find(objectIDs[slot]);

    if(found == mapping.end() || found->second >= runtime->GetScene().GetRenderObjects().size())
    {
        return std::nullopt;
    }

    return found->second;
}

bool GameRuntime::ObjectTable::GetCharacterYaw(std::uint32_t slot, float& yaw) const
{
    if(!HasCharacterController(slot))
    {
        return false;
    }

    const auto current = owner->m_physics.GetCharacterYaw(objectIDs[slot]);
    yaw = current.value_or(0.0f);
    return current.has_value();
}

bool GameRuntime::ObjectTable::SetCharacterYaw(std::uint32_t slot, float yaw, std::string& error)
{
    if(!HasCharacterController(slot))
    {
        error = "CharacterController is not valid.";
        return false;
    }

    // Immediate: the next publication presents it in this frame, with or without a fixed step.
    return owner->m_physics.SetCharacterYaw(owner->m_physicsSession, objectIDs[slot], yaw, error);
}

bool GameRuntime::ObjectTable::FindChild(std::uint32_t slot, std::string_view name, std::uint32_t& child)
{
    const auto* parent = Find(slot);

    if(!parent)
    {
        return false;
    }

    // The first direct child with that exact name, in document order.
    const auto& objects = runtime->GetFileScene().GetDocument().objects;
    const auto found = std::find_if(objects.begin(), objects.end(),
        [&](const SceneObjectDocument& object) { return object.parentID == parent->id && object.name == name; });

    if(found == objects.end())
    {
        return false;
    }

    const auto existing = std::find(objectIDs.begin(), objectIDs.end(), found->id);

    if(existing != objectIDs.end())
    {
        child = static_cast<std::uint32_t>(existing - objectIDs.begin());
        return true;
    }

    // Handles to further objects join this session's table; the table dies with the session.
    if(objectIDs.size() >= 65536)
    {
        return false;
    }

    child = static_cast<std::uint32_t>(objectIDs.size());
    objectIDs.push_back(found->id);
    requests.emplace_back();
    return true;
}

bool GameRuntime::ObjectTable::GetLocalRotation(std::uint32_t slot, float (&rotation)[3]) const
{
    const auto index = RuntimeIndex(slot);

    if(!index)
    {
        return false;
    }

    const auto& scene = runtime->GetScene();
    const auto& runtimeLocal = scene.GetRuntimeLocalTransform(*index);
    const Vector3 value = runtimeLocal ? runtimeLocal->rotation : scene.GetRenderObjects()[*index].transform.rotation;
    rotation[0] = value.x;
    rotation[1] = value.y;
    rotation[2] = value.z;
    return true;
}

bool GameRuntime::ObjectTable::SetLocalRotation(std::uint32_t slot, const float (&rotation)[3], std::string& error)
{
    const auto* object = Find(slot);
    const auto index = RuntimeIndex(slot);

    if(!object || !index)
    {
        error = "SceneObject is not valid.";
        return false;
    }

    if(object->physics || object->characterController)
    {
        error = "A physics-driven object cannot be rotated with SetLocalRotation; use CharacterController.SetYaw.";
        return false;
    }

    if(!std::isfinite(rotation[0]) || !std::isfinite(rotation[1]) || !std::isfinite(rotation[2]))
    {
        error = "SetLocalRotation requires finite Euler angles.";
        return false;
    }

    // Runtime-only local transform: position and scale stay as authored (or as last set).
    auto& scene = runtime->GetScene();
    Transform local = scene.GetRuntimeLocalTransform(*index).value_or(scene.GetRenderObjects()[*index].transform);
    local.rotation = {rotation[0], rotation[1], rotation[2]};
    return scene.SetRuntimeLocalTransform(*index, local, &error);
}

bool GameRuntime::ObjectTable::GetForward(std::uint32_t slot, float (&forward)[3]) const
{
    const auto index = RuntimeIndex(slot);

    if(!index)
    {
        return false;
    }

    // Local +Z of the presented world transform (the camera view direction convention).
    const Matrix4 world = runtime->GetScene().GetRenderObjects()[*index].GetPresentationTransform().currentModel;
    const Vector3 axis{world.m[0][2], world.m[1][2], world.m[2][2]};
    const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);

    if(!(length > 0.0f) || !std::isfinite(length))
    {
        return false;
    }

    forward[0] = axis.x / length;
    forward[1] = axis.y / length;
    forward[2] = axis.z / length;
    return true;
}

void GameRuntime::GameplayInput::GetMouseDelta(float (&delta)[2]) const
{
    // Raw pixels, never scaled by frame time; only while the host granted the cursor lock.
    const bool available = input && cursorCaptured;
    delta[0] = available ? static_cast<float>(input->GetRawMouseDeltaX()) : 0.0f;
    delta[1] = available ? static_cast<float>(input->GetRawMouseDeltaY()) : 0.0f;
}

void GameRuntime::GameplayInput::SetCursorLocked(bool locked)
{
    if(cursorLockRequest)
    {
        *cursorLockRequest = locked;
    }
}

bool GameRuntime::GameplayInput::IsKeyDown(Scripting::Key key) const
{
    return input && input->IsKeyDown(VirtualKey(key));
}

bool GameRuntime::GameplayInput::WasKeyPressed(Scripting::Key key) const
{
    return input && input->WasKeyPressed(VirtualKey(key));
}

bool GameRuntime::Start(FileSceneRuntime& runtime, Renderer& renderer, bool requireMainCamera,
    std::uint32_t viewWidth, std::uint32_t viewHeight, std::string& error, const ScriptModuleSource& scripts)
{
    if(IsActive())
    {
        error = "Game Play is already running.";
        return false;
    }

    // A session whose simulation was ended outside Stop is stale; Engine teardown paths call Stop
    // first, so only direct controller use reaches here. Its scripts are released without OnStop.
    ReleaseScripts();
    m_audio.End();
    m_running = false;

    m_physicsSession = 0;
    m_scriptFault.clear();
    m_cursorLockRequested = false;
    RenderView view;

    // The camera is checked first so a missing Main Camera never starts a simulation.
    if(requireMainCamera && !ResolveMainCameraView(runtime, viewWidth, viewHeight, view, &error))
    {
        error = "No usable Main Camera: " + error;
        return false;
    }

    // Scripts compile and validate before anything starts; an invalid script never starts physics.
    std::vector<PendingScript> pending;

    if(!PrepareScripts(runtime, scripts, pending, error))
    {
        ReleaseScripts();
        return false;
    }

    const auto started = m_physics.Start(runtime, renderer);

    if(started.status != PhysicsSimulationController::Status::Started)
    {
        ReleaseScripts();
        error = started.message;
        return false;
    }

    // Gameplay audio is available from the first script code (constructors) on. The session ends with the scripts
    // on every failure below, so a failed Start leaves no sound behind.
    // The camera fallback of the listener is known before the session places it.
    m_mainCamera = requireMainCamera;
    m_audio.Begin();
    BeginSceneAudio(runtime);

    if(m_scripts)
    {
        m_scripts->SetAudioService(&m_audio);
    }

    // Constructors run script code; a failure discards every instance and the simulation.
    m_objects.runtime = &runtime;

    if(!CreateScriptInstances(pending, error))
    {
        m_objects.runtime = nullptr;
        m_mainCamera = false;
        ReleaseScripts();
        m_audio.End();
        m_physics.Stop(runtime, renderer);
        return false;
    }

    m_physicsSession = m_physics.GetSessionGeneration();
    m_runtimeIdentity = runtime.GetRuntimeIdentity();
    m_running = true;
    RunScriptCallbacks(runtime, Scripting::Lifecycle::Start);
    // Autoplay after OnStart (Architecture D5): scripts set up their state before scene sounds begin.
    StartSceneAudio(runtime);
    error.clear();
    return true;
}

namespace
{
    // An object's presented world pose this frame (the same transform the renderer and camera use).
    std::optional<GameAudioSession::ObjectPose> ResolveAudioPose(const FileSceneRuntime& runtime,
        std::string_view objectID)
    {
        const auto& mapping = runtime.GetObjectMapping().idToRuntimeIndex;
        const auto found = mapping.find(std::string(objectID));
        const auto& objects = runtime.GetScene().GetRenderObjects();

        if(found == mapping.end() || found->second >= objects.size())
        {
            return std::nullopt;
        }

        return GameAudioSession::PoseFromWorld(objects[found->second].GetPresentationTransform().currentModel);
    }
}

namespace
{
    Audio::AcousticTransform ToAcousticTransform(const Matrix4& world)
    {
        Audio::AcousticTransform transform{};

        for(std::size_t row = 0; row < 4; ++row)
        {
            for(std::size_t column = 0; column < 4; ++column)
            {
                transform[row][column] = world.m[row][column];
            }
        }

        return transform;
    }
}

// M4: geometry is immutable once built, and objects with the same mesh and materials share it. The work happens only at Play;
// afterwards only changed transforms travel.
void GameRuntime::PublishAcousticScene(FileSceneRuntime& runtime)
{
    std::vector<GameAudioSession::AcousticObject> published;
    std::map<std::pair<const ImportedMesh*, std::map<std::string, std::string>>, std::shared_ptr<const Audio::AcousticMeshData>> shared;
    const auto& mapping = runtime.GetObjectMapping().idToRuntimeIndex;
    const auto& renderObjects = runtime.GetScene().GetRenderObjects();
    std::size_t enabled = 0;
    std::size_t skipped = 0;
    std::uint64_t triangles = 0;

    for(const auto& object : runtime.GetFileScene().GetDocument().objects)
    {
        if(!object.acoustics || !object.acoustics->enabled)
        {
            continue;
        }

        ++enabled;
        const auto found = mapping.find(object.id);
        const RenderObject* renderObject = found != mapping.end() && found->second < renderObjects.size() ?
            &renderObjects[found->second] : nullptr;
        const ImportedMesh* geometry = nullptr;

        if(m_acousticAssets.meshGeometryByReference)
        {
            geometry = m_acousticAssets.meshGeometryByReference(object.meshReference);
        }
        else if(renderObject && renderObject->mesh && m_acousticAssets.meshGeometry)
        {
            geometry = m_acousticAssets.meshGeometry(*renderObject->mesh);
        }

        if(!geometry || !m_acousticAssets.material)
        {
            Log::Warning("Acoustics: '" + object.name + "' has no available mesh geometry; it contributes no acoustic surface.");
            ++skipped;
            continue;
        }

        auto& mesh = shared[{geometry, object.acoustics->submeshMaterials}];

        if(!mesh)
        {
            AcousticGeometry::BuildReport report;
            mesh = AcousticGeometry::Build(*geometry, *object.acoustics, m_acousticAssets.material, report);

            for(const std::string& warning : report.warnings)
            {
                Log::Warning("Acoustics: '" + object.name + "': " + warning + ".");
            }

            if(!mesh)
            {
                Log::Warning("Acoustics: '" + object.name + "' contributes no acoustic surface: " + report.error + ".");
            }
        }

        if(!mesh)
        {
            ++skipped;
            continue;
        }

        triangles += mesh->triangles.size();
        published.push_back({object.id, mesh, ToAcousticTransform(renderObject->GetPresentationTransform().currentModel)});
    }

    if(enabled > 0)
    {
        Log::Info("Acoustics: " + std::to_string(published.size()) + " of " + std::to_string(enabled) + " enabled objects, " +
            std::to_string(shared.size()) + " distinct geometries, " + std::to_string(triangles) + " triangles.");
    }

    m_audio.PublishAcousticScene(std::move(published));
}

// M7: stale probes are refused rather than loaded, so what M8 consumes always describes the scene that is playing.
void GameRuntime::LoadProbeVolumes(FileSceneRuntime& runtime, bool listenerBaked)
{
    const SceneDocument& document = runtime.GetFileScene().GetDocument();
    const auto& mapping = runtime.GetObjectMapping().idToRuntimeIndex;
    const auto& renderObjects = runtime.GetScene().GetRenderObjects();

    ProbeVolumes::SceneAccess access;
    // Validity reads authored worlds: physics has already started, and a body presented through its pose differs from its authored
    // transform in the last bits, which would make current probes and bakes look stale. The live simulation keeps the presented worlds
    // (PublishAcousticScene).
    std::vector<Matrix4> authoredWorlds;
    std::string authoredError;

    if(!runtime.GetScene().ComputeAuthoredWorlds(authoredWorlds, &authoredError))
    {
        Log::Warning("Probe Volumes: the authored scene could not be evaluated (" + authoredError + "); no probes or baked reflections are loaded.");
        authoredWorlds.clear();
    }

    access.world = [&](const SceneObjectDocument& object) -> std::optional<Audio::AcousticTransform>
    {
        const auto found = mapping.find(object.id);

        if(found == mapping.end() || found->second >= authoredWorlds.size())
        {
            return std::nullopt;
        }

        return ToAcousticTransform(authoredWorlds[found->second]);
    };
    access.mesh = [&](const SceneObjectDocument& object) -> const ImportedMesh*
    {
        if(m_acousticAssets.meshGeometryByReference)
        {
            return m_acousticAssets.meshGeometryByReference(object.meshReference);
        }

        const auto found = mapping.find(object.id);
        const bool hasMesh = found != mapping.end() && found->second < renderObjects.size() && renderObjects[found->second].mesh;
        return hasMesh && m_acousticAssets.meshGeometry ? m_acousticAssets.meshGeometry(*renderObjects[found->second].mesh) : nullptr;
    };

    for(const SceneObjectDocument& object : document.objects)
    {
        if(!object.probeVolume || !object.probeVolume->enabled)
        {
            continue;
        }

        if(object.probeVolume->probeBatch.empty() || !m_acousticAssets.probeBatch)
        {
            Log::Warning("Probe Volume '" + object.name + "' has no generated probes.");
            continue;
        }

        std::string error;
        const auto stored = m_acousticAssets.probeBatch(object.probeVolume->probeBatch, &error);

        if(!stored)
        {
            Log::Warning("Probe Volume '" + object.name + "': " + error + ".");
            continue;
        }

        if(stored->fingerprint != ProbeVolumes::Fingerprint(document, object, access))
        {
            Log::Warning("Probe Volume '" + object.name + "' is stale: regenerate it. Its probes were not loaded.");
            continue;
        }

        m_audio.LoadProbeBatch(object.name, stored->batch, stored->probes.size());
    }

    // M9.5: what opted-in Audio Sources may path through. Decided once, here, from the scene as it starts, independently of baked reflections.
    const bool pathingWanted = std::any_of(document.objects.begin(), document.objects.end(), [](const SceneObjectDocument& object)
    {
        return object.audioSource && object.audioSource->enabled && object.audioSource->pathing;
    });

    if(pathingWanted)
    {
        PathingBakes::Loaders pathingLoaders;
        pathingLoaders.probes = m_acousticAssets.probeBatch;
        pathingLoaders.pathing = m_acousticAssets.bakedPathing;
        const PathingBakes::Selection pathing = PathingBakes::Select(document, access, m_acousticAssets.material, pathingLoaders);

        for(const PathingBakes::VolumeReport& report : pathing.reports)
        {
            if(report.eligible)
            {
                Log::Info("Probe Volume '" + report.name + "': baked pathing is current.");
            }
            else
            {
                Log::Warning("Probe Volume '" + report.name + "': its baked pathing is not used (" + report.reason + ").");
            }
        }

        m_audio.SetPathingVolumes(pathing.volumes);
    }

    // M8.4 and M8.5: what the listener-centric reverb and static sources may use. Decided once, here, from the scene as it starts.
    const bool sourcesBaked = std::any_of(document.objects.begin(), document.objects.end(), [](const SceneObjectDocument& object)
    {
        return object.audioSource && object.audioSource->enabled && object.audioSource->sourceCentricReflections && object.audioSource->bakedReflections;
    });

    if(!listenerBaked && !sourcesBaked)
    {
        m_audio.SetListenerBakedReflections(false);
        return;
    }

    BakedReverbRuntime::Loaders loaders;
    loaders.probes = m_acousticAssets.probeBatch;
    loaders.baked = m_acousticAssets.bakedReflections;
    const BakedReverbRuntime::Selection selection = BakedReverbRuntime::Select(document, access, m_acousticAssets.material, loaders);

    for(const BakedReverbRuntime::VolumeReport& report : selection.reports)
    {
        if(report.eligible)
        {
            Log::Info("Probe Volume '" + report.name + "': baked reverb is current.");
        }
        else
        {
            Log::Warning("Probe Volume '" + report.name + "': its baked reverb is not used (" + report.reason + ").");
        }
    }

    std::vector<GameAudioSession::BakedSource> sources;

    for(const BakedReverbRuntime::SourceReport& report : selection.sourceReports)
    {
        if(!report.eligible)
        {
            Log::Warning("Audio Source '" + report.objectId + "': its baked reflections are not used (" + report.reason + ").");
        }
    }

    for(const BakedReverbRuntime::BakedSource& source : selection.sources)
    {
        sources.push_back({source.objectId, source.identifier, source.position, ReflectionBakes::StaticSourceTolerance});
    }

    m_audio.SetBakedReflections(listenerBaked, selection.volumes, std::move(sources));
}

void GameRuntime::BeginSceneAudio(FileSceneRuntime& runtime)
{
    std::vector<GameAudioSession::SceneSource> sources;
    std::size_t listeners = 0;
    std::string listenerName;
    bool listenerBaked = false;
    m_listenerObjectID.clear();

    for(const auto& object : runtime.GetFileScene().GetDocument().objects)
    {
        // Deterministic: the first enabled listener in scene order is the active one.
        if(object.audioListener && object.audioListener->enabled && listeners++ == 0)
        {
            m_listenerObjectID = object.id;
            listenerName = object.name;
            listenerBaked = object.audioListener->bakedReflections;
        }

        if(object.audioSource)
        {
            const auto& source = *object.audioSource;
            sources.push_back({object.id, object.name, source.eventGuid, source.enabled, source.autoplay, source.sourceCentricReflections,
                source.bakedReflections, source.pathing});
        }
    }

    if(listeners > 1)
    {
        Log::Warning("Gameplay audio: " + std::to_string(listeners) + " enabled Audio Listeners; '" + listenerName +
            "' (first in scene order) is the active one.");
    }

    // The runtime outlives the session: Stop and every failed Start end the session before the scene goes.
    m_audio.SetPoseResolver([&runtime](std::string_view objectID)
    {
        return ResolveAudioPose(runtime, objectID);
    });
    m_audio.RegisterSceneSources(std::move(sources));
    PublishAcousticScene(runtime);
    LoadProbeVolumes(runtime, listenerBaked);
    // The listener is placed before any script code, so events scripts start in OnStart are heard from it.
    UpdateSceneAudio(runtime, 0.0);
}

void GameRuntime::StartSceneAudio(FileSceneRuntime& runtime)
{
    // OnStart may have rotated objects; a zero delta re-places everything without a velocity.
    UpdateSceneAudio(runtime, 0.0);
    m_audio.StartAutoplaySources();
}

void GameRuntime::UpdateSceneAudio(const FileSceneRuntime& runtime, double deltaSeconds)
{
    std::optional<GameAudioSession::ObjectPose> listener;
    std::string listenerKey;

    if(!m_listenerObjectID.empty())
    {
        listener = ResolveAudioPose(runtime, m_listenerObjectID);
        listenerKey = m_listenerObjectID;
    }

    // Without an Audio Listener (or once its object is gone) the gameplay Main Camera hears.
    if(!listener && m_mainCamera)
    {
        RenderView view;

        if(ResolveMainCameraView(runtime, 1, 1, view, nullptr))
        {
            listener = GameAudioSession::ObjectPose{{view.position.x, view.position.y, view.position.z},
                {view.forward.x, view.forward.y, view.forward.z}, {view.up.x, view.up.y, view.up.z}};
            listenerKey = "<main camera>";
        }
    }

    m_audio.UpdateSpatial(deltaSeconds, listenerKey, listener, [&runtime](std::string_view objectID)
    {
        return ResolveAudioPose(runtime, objectID);
    });

    m_audio.UpdateAcousticTransforms([&runtime](std::string_view objectID) -> std::optional<Audio::AcousticTransform>
    {
        const auto& mapping = runtime.GetObjectMapping().idToRuntimeIndex;
        const auto found = mapping.find(std::string(objectID));
        const auto& objects = runtime.GetScene().GetRenderObjects();

        if(found == mapping.end() || found->second >= objects.size())
        {
            return std::nullopt;
        }

        return ToAcousticTransform(objects[found->second].GetPresentationTransform().currentModel);
    });
}

bool GameRuntime::PrepareScripts(FileSceneRuntime& runtime, const ScriptModuleSource& scripts,
    std::vector<PendingScript>& pending, std::string& error)
{
    const auto& objects = runtime.GetFileScene().GetDocument().objects;
    m_objects = {};
    m_objects.owner = this;
    m_objects.runtimeIdentity = runtime.GetRuntimeIdentity();
    // Each Script asset compiles once per session, shared by every object that uses it.
    std::map<std::string, std::pair<Scripting::ModuleID, std::vector<std::string>>> modules;

    for(const auto& object : objects)
    {
        if(!object.script || !object.script->enabled)
        {
            continue;
        }

        const auto& script = *object.script;
        const std::string label = "'" + object.name + "'";
        const auto assetID = AssetID::ParseReference(script.scriptReference);

        if(!assetID)
        {
            error = "Object " + label + " has a Script component without a script asset.";
            return false;
        }

        if(script.className.empty())
        {
            error = "Object " + label + " has a Script component without a selected class.";
            return false;
        }

        if(!scripts)
        {
            error = "Scripts are unavailable in this runtime.";
            return false;
        }

        if(!m_scripts)
        {
            m_scripts = std::make_unique<Scripting::ScriptHost>();
            const auto initialized = m_scripts->Initialize();

            if(initialized.status != Scripting::Status::Ready)
            {
                error = ReportScriptResult(initialized, "Script engine initialization failed");
                return false;
            }
        }

        auto module = modules.find(script.scriptReference);

        if(module == modules.end())
        {
            Scripting::ModuleID built = 0;
            const auto result = scripts(*m_scripts, *assetID, built);

            if(result.status != Scripting::Status::Compiled)
            {
                const std::string asset = result.snapshot ? result.snapshot->rootSection : script.scriptReference;
                error = ReportScriptResult(result, "Script " + asset + " failed to compile");
                return false;
            }

            module = modules.emplace(script.scriptReference, std::make_pair(built, result.classes)).first;
        }

        const auto& classes = module->second.second;

        if(std::find(classes.begin(), classes.end(), script.className) == classes.end())
        {
            std::string available;

            for(const auto& name : classes)
            {
                available += (available.empty() ? "" : ", ") + name;
            }

            error = "Object " + label + ": '" + script.className + "' is not an EAScript class of its script" +
                (available.empty() ? " (it defines none)." : " (available: " + available + ").");
            return false;
        }

        const auto slot = static_cast<std::uint32_t>(m_objects.objectIDs.size());
        m_objects.objectIDs.push_back(object.id);
        m_objects.requests.emplace_back();
        pending.push_back({slot, module->second.first, script.className, label + " (" + script.className + ")"});
    }

    return true;
}

bool GameRuntime::CreateScriptInstances(const std::vector<PendingScript>& pending, std::string& error)
{
    if(pending.empty())
    {
        return true;
    }

    m_scripts->SetObjectResolver(&m_objects);

    for(const auto& script : pending)
    {
        Scripting::InstanceID instance = 0;
        const auto result = m_scripts->CreateInstance(script.module, script.className, script.slot, instance);

        if(!Succeeded(result))
        {
            error = ReportScriptResult(result, "Script " + script.label + " failed to construct");
            return false;
        }

        m_scriptInstances.push_back({instance, script.slot, script.label, false, false});
    }

    return true;
}

void GameRuntime::RunScriptCallbacks(FileSceneRuntime& runtime, Scripting::Lifecycle callback)
{
    if(!m_scripts || m_scriptInstances.empty())
    {
        return;
    }

    // Owner handles resolve only during a dispatch, against the runtime passed for it. Lifecycle
    // callbacks see no frame input but may request the cursor lock.
    m_objects.runtime = &runtime;
    m_input.input = nullptr;
    m_input.cursorCaptured = false;
    m_input.cursorLockRequest = &m_cursorLockRequested;
    m_scripts->SetInputSource(&m_input);
    const bool starting = callback == Scripting::Lifecycle::Start;

    for(auto& script : m_scriptInstances)
    {
        // OnStop runs once for, and only for, each instance whose OnStart completed.
        if(!starting && !script.started)
        {
            continue;
        }

        const auto result = m_scripts->Invoke(script.instance, callback);
        const bool succeeded = Succeeded(result);

        if(starting)
        {
            script.started = succeeded;
            script.updating = succeeded;
        }

        if(!succeeded)
        {
            // A faulted instance is disabled; the rest of the game keeps running.
            m_objects.requests[script.slot] = {};
            ReportScriptFault(ReportScriptResult(result, "Script " + script.label +
                (starting ? " OnStart failed" : " OnStop failed")));
        }
    }

    m_scripts->SetInputSource(nullptr);
    m_objects.runtime = nullptr;
}

void GameRuntime::ReportScriptFault(const std::string& message)
{
    if(m_scriptFault.empty())
    {
        m_scriptFault = message;
    }
}

void GameRuntime::RunScriptUpdates(FileSceneRuntime& runtime, const Frame& frame)
{
    // Without gameplay focus, requests left from focused frames are dropped (no delayed jump);
    // scripts still run and may request movement that does not depend on input.
    if(!frame.input)
    {
        for(auto& request : m_objects.requests)
        {
            request = {};
        }
    }

    if(!m_scripts || m_scriptInstances.empty())
    {
        return;
    }

    m_objects.runtime = &runtime;
    m_input.input = frame.input;
    m_input.cursorCaptured = frame.input && frame.cursorCaptured;
    m_input.cursorLockRequest = &m_cursorLockRequested;
    m_scripts->SetInputSource(&m_input);
    const float deltaTime = static_cast<float>(frame.deltaSeconds);

    for(auto& script : m_scriptInstances)
    {
        if(!script.updating)
        {
            continue;
        }

        const auto result = m_scripts->Update(script.instance, deltaTime);

        if(!Succeeded(result))
        {
            // Exception isolation: this instance stops updating and its requests are discarded.
            script.updating = false;
            m_objects.requests[script.slot] = {};
            ReportScriptFault(ReportScriptResult(result, "Script " + script.label + " OnUpdate failed"));
        }
    }

    m_scripts->SetInputSource(nullptr);
    m_input.input = nullptr;
    m_objects.runtime = nullptr;
}

void GameRuntime::SubmitCharacterRequests(double elapsedSeconds)
{
    // Exactly the steps the following Update will run (same due-tick arithmetic and step cap).
    const std::size_t dueSteps = m_physics.PredictStepsForUpdate(elapsedSeconds);
    const std::uint64_t firstStep = m_physics.GetNextFixedStep();

    for(std::size_t slot = 0; slot < m_objects.requests.size(); ++slot)
    {
        auto& request = m_objects.requests[slot];
        const auto& objectID = m_objects.objectIDs[slot];
        std::string error;

        if(dueSteps != 0 && request.moving && (request.velocity.x != 0.0f || request.velocity.z != 0.0f))
        {
            // Held velocity applies to every due step, catch-up steps included.
            for(std::size_t offset = 0; offset < dueSteps; ++offset)
            {
                if(!m_physics.SubmitCharacterMovement(m_physicsSession, objectID, firstStep + offset,
                    request.velocity, error))
                {
                    Log::Warning("Character movement request for " + objectID + " rejected: " + error);
                    break;
                }
            }
        }

        // A jump waits for the first frame that runs a step, then is consumed by that step.
        if(dueSteps != 0 && request.jump)
        {
            if(!m_physics.SubmitCharacterJump(m_physicsSession, objectID, firstStep, error))
            {
                Log::Warning("Character jump request for " + objectID + " rejected: " + error);
            }

            request.jump = false;
        }

        // Movement is per frame: a held key re-requests it from the next OnUpdate.
        request.moving = false;
        request.velocity = {};
    }
}

void GameRuntime::ReleaseScripts() noexcept
{
    // Instances, modules and the engine die together; nothing survives into the next session.
    m_scriptInstances.clear();
    m_scripts.reset();
    m_objects = {};
}

bool GameRuntime::Update(FileSceneRuntime& runtime, Renderer& renderer, const Frame& frame, std::string& error)
{
    if(!IsActive() || runtime.GetRuntimeIdentity() != m_runtimeIdentity || renderer.IsCommandRecordingActive())
    {
        error = "Game Play lost its runtime scene or idle renderer boundary.";
        return false;
    }

    // Scripts run first, so their requests address the steps this frame is about to simulate.
    RunScriptUpdates(runtime, frame);
    m_audio.Update();
    SubmitCharacterRequests(frame.deltaSeconds);

    // The shared controller owns the only clock and command consumption order.
    // A deferred publication never causes the elapsed interval to be submitted twice.
    const auto updated = m_physics.Update(runtime, renderer, frame.deltaSeconds);

    if(updated.status != PhysicsSimulationController::Status::Updated)
    {
        error = updated.message;
        return false;
    }

    const auto published = m_physics.PublishAtBoundary(runtime, renderer);

    if(published.status != PhysicsSimulationController::Status::Published &&
        published.status != PhysicsSimulationController::Status::Pending)
    {
        error = published.message;
        return false;
    }

    // Audio follows the poses this frame presents (after scripts and physics).
    UpdateSceneAudio(runtime, frame.deltaSeconds);
    error.clear();
    return true;
}

bool GameRuntime::ResolveView(const FileSceneRuntime& runtime, std::uint32_t width, std::uint32_t height,
    std::optional<RenderView>& view, std::string& error) const
{
    view.reset();

    if(!m_mainCamera)
    {
        return true;
    }

    RenderView resolved;

    if(!ResolveMainCameraView(runtime, width, height, resolved, &error))
    {
        return false;
    }

    view = std::move(resolved);
    return true;
}

bool GameRuntime::Stop(FileSceneRuntime& runtime, Renderer& renderer)
{
    // Never stops a physics session it did not start (the Editor's Physics Simulation tool).
    const bool owned = IsActive();

    if(m_running && runtime.GetRuntimeIdentity() == m_runtimeIdentity)
    {
        RunScriptCallbacks(runtime, Scripting::Lifecycle::Stop);
        // Runtime-only child rotations end with the session; the authored transforms return.
        runtime.GetScene().ClearRuntimeLocalTransforms();
    }

    // After OnStop (scripts may stop or release their own sounds there), before the scripts are destroyed: every
    // remaining gameplay sound stops now and the mixer returns to its pre-Play state, even if physics cleanup must
    // be retried below.
    m_audio.End();
    ReleaseScripts();
    m_cursorLockRequested = false;
    m_running = false;
    m_physicsSession = 0;
    m_mainCamera = false;

    if(!owned)
    {
        return true;
    }

    m_physics.Stop(runtime, renderer);
    return m_physics.GetState() == PhysicsSimulationController::State::Inactive;
}
