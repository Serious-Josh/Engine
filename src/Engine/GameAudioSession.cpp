#include "Engine/GameAudioSession.h"
#include "Core/Log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>

namespace
{
    // Session identities are unique per process, so a handle kept across Stop/Play never matches a later session.
    std::atomic<std::uint64_t> g_sessionCounter{0};

    // Bounds the per-session warning memory; later distinct warnings are dropped silently.
    constexpr std::size_t MaximumDistinctWarnings = 64;

    bool IsSounding(Audio::PlaybackState state)
    {
        return state == Audio::PlaybackState::Starting || state == Audio::PlaybackState::Playing ||
            state == Audio::PlaybackState::Sustaining || state == Audio::PlaybackState::Stopping;
    }

    using Vector = std::array<float, 3>;

    float Dot(const Vector& left, const Vector& right)
    {
        return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
    }

    std::optional<Vector> Normalized(const Vector& vector)
    {
        const float length = std::sqrt(Dot(vector, vector));

        if(!(length > 1.0e-6f) || !std::isfinite(length))
        {
            return std::nullopt;
        }

        return Vector{vector[0] / length, vector[1] / length, vector[2] / length};
    }

    // Frame-to-frame velocity; zero without a previous position, for a non-positive delta and for implausible jumps
    // (teleports, cuts), so a discontinuity never becomes a Doppler spike.
    Vector Velocity(const std::optional<Vector>& previous, const Vector& current, double deltaSeconds)
    {
        if(!previous || !(deltaSeconds > 0.0))
        {
            return {0.0f, 0.0f, 0.0f};
        }

        const float inverse = static_cast<float>(1.0 / deltaSeconds);
        const Vector velocity{(current[0] - (*previous)[0]) * inverse, (current[1] - (*previous)[1]) * inverse,
            (current[2] - (*previous)[2]) * inverse};

        if(!(std::sqrt(Dot(velocity, velocity)) <= GameAudioSession::MaximumPlausibleSpeed))
        {
            return {0.0f, 0.0f, 0.0f};
        }

        return velocity;
    }

    Audio::Attributes3D Attributes(const GameAudioSession::ObjectPose& pose, const Vector& velocity)
    {
        return Audio::Attributes3D{pose.position, velocity, pose.forward, pose.up};
    }
}

std::optional<GameAudioSession::ObjectPose> GameAudioSession::PoseFromWorld(const Matrix4& world)
{
    const Vector position{world.m[0][3], world.m[1][3], world.m[2][3]};
    const auto forward = Normalized({world.m[0][2], world.m[1][2], world.m[2][2]});
    const Vector rawUp{world.m[0][1], world.m[1][1], world.m[2][1]};

    if(!forward || !std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2]))
    {
        return std::nullopt;
    }

    // Gram-Schmidt: FMOD requires perpendicular axes; a sheared world matrix (non-uniform scale under rotation) has
    // slightly skewed columns.
    const float along = Dot(rawUp, *forward);
    const auto up = Normalized({rawUp[0] - along * (*forward)[0], rawUp[1] - along * (*forward)[1],
        rawUp[2] - along * (*forward)[2]});

    if(!up)
    {
        return std::nullopt;
    }

    return ObjectPose{position, *forward, *up};
}

const char* GameAudioSession::MixerTargetName(MixerTarget target) noexcept
{
    switch(target)
    {
    case MixerTarget::Bus:
        return "bus";
    case MixerTarget::VCA:
        return "VCA";
    case MixerTarget::GlobalParameter:
        return "global parameter";
    }

    return "mixer value";
}

void GameAudioSession::SetAudioSystem(Audio::AudioSystem* audio) noexcept
{
    m_audio = audio;
}

void GameAudioSession::Begin()
{
    End();
    m_session = ++g_sessionCounter;
    m_active = true;
}

bool GameAudioSession::Running() const noexcept
{
    if(!m_active || !m_audio)
    {
        return false;
    }

    const Audio::Status status = m_audio->GetStatus();
    return status == Audio::Status::Active || status == Audio::Status::NoOutput;
}

GameAudioSession::OwnedEvent* GameAudioSession::Find(const Scripting::AudioEventHandle& handle)
{
    return const_cast<OwnedEvent*>(static_cast<const GameAudioSession*>(this)->Find(handle));
}

const GameAudioSession::OwnedEvent* GameAudioSession::Find(const Scripting::AudioEventHandle& handle) const
{
    if(!Running() || handle.session != m_session || handle.generation == 0)
    {
        return nullptr;
    }

    const auto owned = std::find_if(m_events.begin(), m_events.end(), [&handle](const OwnedEvent& event)
    {
        return event.handle.index == handle.index && event.handle.generation == handle.generation;
    });

    return owned == m_events.end() ? nullptr : &*owned;
}

void GameAudioSession::Forget(const OwnedEvent& event)
{
    m_events.erase(m_events.begin() + (&event - m_events.data()));
}

// Before FMOD first reports an event as sounding, a just-started event counts as playing for a short grace period.
bool GameAudioSession::HasStarted(const OwnedEvent& event, Audio::PlaybackState state) const
{
    return event.seenPlaying || IsSounding(state) ||
        m_audio->GetUpdateCount() - event.playedAtUpdate >= StartGraceUpdates;
}

void GameAudioSession::Update()
{
    if(!Running())
    {
        return;
    }

    for(std::size_t index = m_events.size(); index-- > 0;)
    {
        OwnedEvent& event = m_events[index];
        const Audio::PlaybackState state = m_audio->GetPlaybackState(event.handle);

        if(state == Audio::PlaybackState::Invalid)
        {
            // Destroyed by FMOD (its bank was unloaded); nothing left to release.
            m_events.erase(m_events.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }

        if(IsSounding(state))
        {
            event.seenPlaying = true;
            continue;
        }

        // Stopped: finished on its own or stopped by the script. Released, so its handle becomes invalid.
        if(HasStarted(event, state))
        {
            m_audio->ReleaseEvent(event.handle);
            m_events.erase(m_events.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }
}

void GameAudioSession::End() noexcept
{
    if(!m_active)
    {
        return;
    }

    // Audio may already be shut down (Engine shutdown order): then FMOD released everything itself and the handles
    // are stale, so only the bookkeeping is cleared.
    const bool running = Running();

    try
    {
        if(running)
        {
            for(const OwnedEvent& event : m_events)
            {
                m_audio->StopEvent(event.handle, false);
                m_audio->ReleaseEvent(event.handle);
            }

            std::string ignored;

            for(auto restore = m_restores.rbegin(); restore != m_restores.rend(); ++restore)
            {
                switch(restore->target)
                {
                case MixerTarget::Bus:
                    m_audio->SetBusVolume(restore->name, restore->original, ignored);
                    break;
                case MixerTarget::VCA:
                    m_audio->SetVCAVolume(restore->name, restore->original, ignored);
                    break;
                case MixerTarget::GlobalParameter:
                    m_audio->SetGlobalParameter(restore->name, restore->original, ignored);
                    break;
                }
            }
        }

        if(!m_events.empty() || !m_restores.empty())
        {
            Log::Info("Gameplay audio: session ended; " + std::to_string(m_events.size()) + " events stopped, " +
                std::to_string(m_restores.size()) + " mixer values restored.");
        }
    }
    catch(...)
    {
        // Logging allocation failure only; the session still ends.
    }

    m_events.clear();
    m_restores.clear();
    m_sources.clear();
    m_resolve = nullptr;
    m_warnings.clear();
    m_listenerKey.clear();
    m_listenerLastPosition.reset();
    m_active = false;
}

void GameAudioSession::SetPoseResolver(PoseResolver resolve)
{
    m_resolve = std::move(resolve);
}

void GameAudioSession::RegisterSceneSources(std::vector<SceneSource> sources)
{
    m_sources.clear();
    m_sources.reserve(sources.size());

    for(SceneSource& source : sources)
    {
        SourceState state;
        state.enabled = source.enabled;
        state.authored = std::move(source);
        m_sources.push_back(std::move(state));
    }
}

void GameAudioSession::StartAutoplaySources()
{
    std::size_t pending = 0;

    for(SourceState& source : m_sources)
    {
        // A script that started, stopped or disabled the source during startup has decided for it.
        const bool autoplay = source.authored.autoplay && source.enabled && !source.autoplayHandled;
        source.autoplayHandled = true;

        if(!autoplay)
        {
            continue;
        }

        if(source.authored.eventGuid.empty())
        {
            Warn("Audio Source on '" + source.authored.label + "' has no event; it does not play.");
            continue;
        }

        if(!Running())
        {
            ++pending;
            continue;
        }

        // Already playing (a script started it in OnStart): autoplay never adds a second instance.
        const OwnedEvent* current = source.event ? FindOwned(*source.event) : nullptr;

        if(current && IsEventSounding(*current))
        {
            continue;
        }

        StartSource(source);
    }

    if(pending > 0)
    {
        Warn("audio is not available; " + std::to_string(pending) + " scene Audio Sources do not play.");
    }
}

void GameAudioSession::StartSceneSources(const std::vector<SceneSource>& sources, const PoseResolver& resolve)
{
    SetPoseResolver(resolve);
    RegisterSceneSources(sources);
    StartAutoplaySources();
}

std::optional<Audio::EventHandle> GameAudioSession::StartPlaced(const Audio::Guid& guid,
    const std::optional<ObjectPose>& pose, const std::string& context)
{
    if(m_events.size() >= MaximumEvents)
    {
        Warn("more than " + std::to_string(MaximumEvents) + " gameplay events at once; " + context + " is skipped.");
        return std::nullopt;
    }

    std::string error;
    const auto created = m_audio->CreateEventInstance(guid, error);

    if(!created)
    {
        Warn(context + ": " + error);
        return std::nullopt;
    }

    // Positioned before it starts, so the first mixed block is already in place (no pan jump from the origin).
    const bool placed = !pose || m_audio->SetEventAttributes(*created, Attributes(*pose, {0.0f, 0.0f, 0.0f}), error);

    if(!placed || !m_audio->StartEvent(*created))
    {
        m_audio->ReleaseEvent(*created);
        Warn(context + " cannot start: " + error);
        return std::nullopt;
    }

    OwnedEvent owned{*created, m_audio->GetUpdateCount(), false};

    if(pose)
    {
        owned.lastPosition = pose->position;
    }

    m_events.push_back(std::move(owned));
    return created;
}

bool GameAudioSession::StartSource(SourceState& source)
{
    const std::string context = "Audio Source on '" + source.authored.label + "'";
    const auto guid = Audio::ParseGuid(source.authored.eventGuid);

    if(!guid || !m_audio->HasEvent(*guid))
    {
        Warn(context + ": event " + source.authored.eventGuid + " is not in a loaded bank.");
        return false;
    }

    const auto pose = m_resolve ? m_resolve(source.authored.objectID) : std::nullopt;

    if(!pose)
    {
        Warn(context + ": the object has no world pose.");
        return false;
    }

    const auto started = StartPlaced(*guid, pose, context);

    if(!started)
    {
        return false;
    }

    OwnedEvent& owned = m_events.back();
    owned.objectID = source.authored.objectID;
    owned.sceneSource = true;
    source.event = *started;
    return true;
}

void GameAudioSession::StopAndRelease(std::size_t index)
{
    m_audio->StopEvent(m_events[index].handle, false);
    m_audio->ReleaseEvent(m_events[index].handle);
    m_events.erase(m_events.begin() + static_cast<std::ptrdiff_t>(index));
}

void GameAudioSession::UpdateSpatial(double deltaSeconds, std::string_view listenerKey,
    const std::optional<ObjectPose>& listener, const PoseResolver& resolve)
{
    if(!Running())
    {
        return;
    }

    std::string error;

    if(listener)
    {
        // A different listener (object switch, or camera fallback) is a cut: no velocity from the old position.
        const bool sameListener = m_listenerKey == listenerKey;
        const Vector velocity = Velocity(sameListener ? m_listenerLastPosition : std::nullopt, listener->position,
            deltaSeconds);

        if(!m_audio->SetListenerAttributes(Attributes(*listener, velocity), error))
        {
            Warn("listener: " + error);
        }

        m_listenerKey = listenerKey;
        m_listenerLastPosition = listener->position;
    }

    for(std::size_t index = m_events.size(); index-- > 0;)
    {
        OwnedEvent& event = m_events[index];

        if(event.objectID.empty())
        {
            continue;
        }

        const auto pose = resolve(event.objectID);

        if(!pose)
        {
            // The object left the running scene: its sounds, component or attached, end with it.
            StopAndRelease(index);
            continue;
        }

        const Vector velocity = Velocity(event.lastPosition, pose->position, deltaSeconds);
        const bool placed = m_audio->SetEventAttributes(event.handle, Attributes(*pose, velocity), error);

        if(!placed && m_audio->GetPlaybackState(event.handle) != Audio::PlaybackState::Invalid)
        {
            Warn("Audio Source on object " + event.objectID + ": " + error);
        }

        event.lastPosition = pose->position;
    }
}

std::string GameAudioSession::GetEventObject(const Scripting::AudioEventHandle& handle) const
{
    const OwnedEvent* event = Find(handle);
    return event ? event->objectID : std::string();
}

std::vector<Scripting::AudioEventHandle> GameAudioSession::GetSceneEvents() const
{
    std::vector<Scripting::AudioEventHandle> handles;

    for(const OwnedEvent& event : m_events)
    {
        if(event.sceneSource)
        {
            handles.push_back({m_session, event.handle.index, event.handle.generation});
        }
    }

    return handles;
}

std::vector<Scripting::AudioEventHandle> GameAudioSession::GetEvents() const
{
    std::vector<Scripting::AudioEventHandle> handles;

    for(const OwnedEvent& event : m_events)
    {
        handles.push_back({m_session, event.handle.index, event.handle.generation});
    }

    return handles;
}

void GameAudioSession::Warn(const std::string& message)
{
    if(m_warnings.size() >= MaximumDistinctWarnings || !m_warnings.insert(message).second)
    {
        return;
    }

    Log::Warning("Gameplay audio: " + message);
}

bool GameAudioSession::PlayEvent(std::string_view event, Scripting::AudioEventHandle& handle)
{
    handle = {};

    if(!Running())
    {
        Warn("audio is not available; Audio::Play(\"" + std::string(event) + "\") is ignored.");
        return false;
    }

    if(m_events.size() >= MaximumEvents)
    {
        Warn("more than " + std::to_string(MaximumEvents) + " gameplay events at once; further Play calls fail.");
        return false;
    }

    const auto guid = m_audio->ResolveEventReference(event);

    if(!guid)
    {
        Warn("event \"" + std::string(event) + "\" is not in a loaded bank (unknown GUID, missing bank, or a path "
            "without the strings bank).");
        return false;
    }

    std::string error;
    const auto created = m_audio->CreateEventInstance(*guid, error);

    if(!created)
    {
        Warn("cannot play \"" + std::string(event) + "\": " + error);
        return false;
    }

    if(!m_audio->StartEvent(*created))
    {
        m_audio->ReleaseEvent(*created);
        Warn("cannot start \"" + std::string(event) + "\".");
        return false;
    }

    m_events.push_back({*created, m_audio->GetUpdateCount(), false});
    handle = {m_session, created->index, created->generation};
    return true;
}

bool GameAudioSession::IsEventValid(const Scripting::AudioEventHandle& handle) const
{
    const OwnedEvent* event = Find(handle);
    return event && m_audio->GetPlaybackState(event->handle) != Audio::PlaybackState::Invalid;
}

bool GameAudioSession::IsEventPlaying(const Scripting::AudioEventHandle& handle) const
{
    const OwnedEvent* event = Find(handle);

    if(!event)
    {
        return false;
    }

    const Audio::PlaybackState state = m_audio->GetPlaybackState(event->handle);
    return IsSounding(state) || (state == Audio::PlaybackState::Stopped && !HasStarted(*event, state));
}

bool GameAudioSession::StopEvent(const Scripting::AudioEventHandle& handle, bool allowFadeOut)
{
    const OwnedEvent* event = Find(handle);
    return event && m_audio->StopEvent(event->handle, allowFadeOut);
}

bool GameAudioSession::SetEventParameter(const Scripting::AudioEventHandle& handle, std::string_view name, float value)
{
    const OwnedEvent* event = Find(handle);

    if(!event)
    {
        return false;
    }

    std::string error;

    if(!m_audio->SetEventParameter(event->handle, name, value, error))
    {
        Warn("SetParameter(\"" + std::string(name) + "\"): " + error);
        return false;
    }

    return true;
}

bool GameAudioSession::SetEventVolume(const Scripting::AudioEventHandle& handle, float volume)
{
    const OwnedEvent* event = Find(handle);

    if(!event)
    {
        return false;
    }

    std::string error;

    if(!m_audio->SetEventVolume(event->handle, volume, error))
    {
        Warn("SetVolume: " + error);
        return false;
    }

    return true;
}

bool GameAudioSession::ReleaseEvent(const Scripting::AudioEventHandle& handle)
{
    const OwnedEvent* event = Find(handle);

    if(!event)
    {
        return false;
    }

    m_audio->StopEvent(event->handle, true);
    m_audio->ReleaseEvent(event->handle);
    Forget(*event);
    return true;
}

bool GameAudioSession::PlayEventAt(std::string_view event, const float (&position)[3],
    Scripting::AudioEventHandle& handle)
{
    handle = {};

    if(!Running())
    {
        Warn("audio is not available; Audio::PlayAt(\"" + std::string(event) + "\") is ignored.");
        return false;
    }

    if(!std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2]))
    {
        Warn("Audio::PlayAt(\"" + std::string(event) + "\"): the position must be finite.");
        return false;
    }

    const auto guid = m_audio->ResolveEventReference(event);

    if(!guid)
    {
        Warn("event \"" + std::string(event) + "\" is not in a loaded bank (unknown GUID, missing bank, or a path "
            "without the strings bank).");
        return false;
    }

    // Default orientation (+Z forward, +Y up): a point source has no facing of its own.
    const ObjectPose pose{{position[0], position[1], position[2]}};
    const auto started = StartPlaced(*guid, pose, "Audio::PlayAt(\"" + std::string(event) + "\")");

    if(!started)
    {
        return false;
    }

    handle = {m_session, started->index, started->generation};
    return true;
}

bool GameAudioSession::SetEventPosition(const Scripting::AudioEventHandle& handle, const float (&position)[3])
{
    OwnedEvent* event = Find(handle);

    if(!event)
    {
        Warn("AudioEvent.SetPosition: the event is not valid (finished, released, or from an earlier Play).");
        return false;
    }

    if(!std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2]))
    {
        Warn("AudioEvent.SetPosition: the position must be finite.");
        return false;
    }

    // A script-placed position has no frame history: it is a cut, with zero velocity (no Doppler).
    const ObjectPose pose{{position[0], position[1], position[2]}};
    std::string error;

    if(!m_audio->SetEventAttributes(event->handle, Attributes(pose, {0.0f, 0.0f, 0.0f}), error))
    {
        Warn("AudioEvent.SetPosition: " + error);
        return false;
    }

    event->objectID.clear();
    event->sceneSource = false;
    event->lastPosition = pose.position;
    return true;
}

bool GameAudioSession::AttachEvent(const Scripting::AudioEventHandle& handle, std::string_view objectKey)
{
    OwnedEvent* event = Find(handle);

    if(!event)
    {
        Warn("AudioEvent.AttachTo: the event is not valid (finished, released, or from an earlier Play).");
        return false;
    }

    const auto pose = m_resolve && !objectKey.empty() ? m_resolve(objectKey) : std::nullopt;

    if(!pose)
    {
        Warn("AudioEvent.AttachTo: the object is not valid in the running scene.");
        return false;
    }

    // Placed at once, so the event does not sound from its old position until the next frame.
    std::string error;

    if(!m_audio->SetEventAttributes(event->handle, Attributes(*pose, {0.0f, 0.0f, 0.0f}), error))
    {
        Warn("AudioEvent.AttachTo: " + error);
        return false;
    }

    event->objectID = std::string(objectKey);
    event->lastPosition = pose->position;
    return true;
}

bool GameAudioSession::DetachEvent(const Scripting::AudioEventHandle& handle)
{
    OwnedEvent* event = Find(handle);

    if(!event)
    {
        Warn("AudioEvent.Detach: the event is not valid (finished, released, or from an earlier Play).");
        return false;
    }

    if(event->sceneSource)
    {
        Warn("AudioEvent.Detach: an Audio Source's event always follows its object.");
        return false;
    }

    // It stays where the object last placed it; with no further movement its velocity becomes zero.
    if(const auto attributes = m_audio->GetEventAttributes(event->handle))
    {
        auto still = *attributes;
        still.velocity = {0.0f, 0.0f, 0.0f};
        std::string ignored;
        m_audio->SetEventAttributes(event->handle, still, ignored);
    }

    event->objectID.clear();
    return true;
}

const GameAudioSession::OwnedEvent* GameAudioSession::FindOwned(const Audio::EventHandle& handle) const
{
    const auto owned = std::find_if(m_events.begin(), m_events.end(),
        [&handle](const OwnedEvent& event) { return event.handle == handle; });
    return owned == m_events.end() ? nullptr : &*owned;
}

bool GameAudioSession::IsEventSounding(const OwnedEvent& event) const
{
    const Audio::PlaybackState state = m_audio->GetPlaybackState(event.handle);
    return IsSounding(state) || (state == Audio::PlaybackState::Stopped && !HasStarted(event, state));
}

GameAudioSession::SourceState* GameAudioSession::FindSource(std::string_view objectKey)
{
    return const_cast<SourceState*>(static_cast<const GameAudioSession*>(this)->FindSource(objectKey));
}

const GameAudioSession::SourceState* GameAudioSession::FindSource(std::string_view objectKey) const
{
    if(!m_active || objectKey.empty())
    {
        return nullptr;
    }

    const auto found = std::find_if(m_sources.begin(), m_sources.end(),
        [&](const SourceState& source) { return source.authored.objectID == objectKey; });
    return found == m_sources.end() ? nullptr : &*found;
}

bool GameAudioSession::HasAudioSource(std::string_view objectKey) const
{
    return FindSource(objectKey) != nullptr;
}

bool GameAudioSession::PlayAudioSource(std::string_view objectKey)
{
    SourceState* source = FindSource(objectKey);

    if(!source)
    {
        Warn("AudioSource.Play: the object has no Audio Source in the running scene.");
        return false;
    }

    if(!source->enabled)
    {
        Warn("AudioSource.Play on '" + source->authored.label + "': the source is disabled; Play is ignored.");
        return false;
    }

    if(source->authored.eventGuid.empty())
    {
        Warn("Audio Source on '" + source->authored.label + "' has no event; it does not play.");
        return false;
    }

    if(!Running())
    {
        Warn("audio is not available; AudioSource.Play on '" + source->authored.label + "' is ignored.");
        return false;
    }

    const OwnedEvent* current = source->event ? FindOwned(*source->event) : nullptr;

    // One instance per source: Play while it is playing changes nothing.
    if(current && IsEventSounding(*current))
    {
        return true;
    }

    // A Play that starts the source during startup replaces its autoplay; a refused Play decides nothing.
    const bool started = StartSource(*source);
    source->autoplayHandled = source->autoplayHandled || started;
    return started;
}

bool GameAudioSession::StopAudioSource(std::string_view objectKey, bool allowFadeOut)
{
    SourceState* source = FindSource(objectKey);

    if(!source)
    {
        Warn("AudioSource.Stop: the object has no Audio Source in the running scene.");
        return false;
    }

    // Stopping before autoplay cancels it: the script decided the source stays silent.
    source->autoplayHandled = true;
    const OwnedEvent* current = source->event && Running() ? FindOwned(*source->event) : nullptr;

    // The event stays owned until FMOD reports it stopped (after any fade-out); Update then releases it.
    return !current || m_audio->StopEvent(current->handle, allowFadeOut);
}

bool GameAudioSession::IsAudioSourcePlaying(std::string_view objectKey) const
{
    const SourceState* source = FindSource(objectKey);

    if(!source || !source->event || !Running())
    {
        return false;
    }

    const OwnedEvent* current = FindOwned(*source->event);
    return current && IsEventSounding(*current);
}

bool GameAudioSession::SetAudioSourceEnabled(std::string_view objectKey, bool enabled)
{
    SourceState* source = FindSource(objectKey);

    if(!source)
    {
        Warn("AudioSource.SetEnabled: the object has no Audio Source in the running scene.");
        return false;
    }

    source->enabled = enabled;
    const OwnedEvent* current = !enabled && source->event && Running() ? FindOwned(*source->event) : nullptr;

    if(current)
    {
        m_audio->StopEvent(current->handle, true);
    }

    return true;
}

bool GameAudioSession::IsAudioSourceEnabled(std::string_view objectKey) const
{
    const SourceState* source = FindSource(objectKey);
    return source && source->enabled;
}

bool GameAudioSession::ChangeMixerValue(MixerTarget target, std::string_view name, float value)
{
    if(!Running())
    {
        return false;
    }

    const bool captured = std::any_of(m_restores.begin(), m_restores.end(), [&](const MixerRestore& restore)
    {
        return restore.target == target && restore.name == name;
    });

    // The original value is read before the first change, so End can put back exactly what the session found.
    std::optional<float> original;

    if(!captured)
    {
        switch(target)
        {
        case MixerTarget::Bus:
            original = m_audio->GetBusVolume(name);
            break;
        case MixerTarget::VCA:
            original = m_audio->GetVCAVolume(name);
            break;
        case MixerTarget::GlobalParameter:
            original = m_audio->GetGlobalParameter(name);
            break;
        }

        if(!original)
        {
            Warn("unknown " + std::string(MixerTargetName(target)) + " \"" + std::string(name) + "\".");
            return false;
        }
    }

    std::string error;
    bool changed = false;

    switch(target)
    {
    case MixerTarget::Bus:
        changed = m_audio->SetBusVolume(name, value, error);
        break;
    case MixerTarget::VCA:
        changed = m_audio->SetVCAVolume(name, value, error);
        break;
    case MixerTarget::GlobalParameter:
        changed = m_audio->SetGlobalParameter(name, value, error);
        break;
    }

    if(!changed)
    {
        Warn("\"" + std::string(name) + "\": " + error);
        return false;
    }

    if(original)
    {
        m_restores.push_back({target, std::string(name), *original});
    }

    return true;
}

bool GameAudioSession::SetGlobalParameter(std::string_view name, float value)
{
    return ChangeMixerValue(MixerTarget::GlobalParameter, name, value);
}

bool GameAudioSession::SetBusVolume(std::string_view bus, float volume)
{
    return ChangeMixerValue(MixerTarget::Bus, bus, volume);
}

bool GameAudioSession::SetVCAVolume(std::string_view vca, float volume)
{
    return ChangeMixerValue(MixerTarget::VCA, vca, volume);
}
