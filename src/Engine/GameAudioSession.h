#pragma once

#include "Audio/AudioSystem.h"
#include "Math/Matrix4.h"
#include "Scripting/ScriptHost.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Audio.1C: the gameplay audio of one Play session (Editor Play and packaged games, through GameRuntime).
//
// FMOD and the project's banks are process-lifetime (Audio.1A/1B); a session only owns what gameplay created or
// changed while it ran: the event instances scripts played and the original values of every bus, VCA and global
// parameter they changed. End stops and releases those instances and restores those values, so nothing a session
// did is audible or observable afterwards, and handles from it never reach a later session.
//
// Main thread only. The AudioSystem is borrowed: it must outlive every session that is still active, and a session
// never touches it outside Begin/Update/End and script dispatch (its destructor does not).
class GameAudioSession final : public Scripting::AudioService
{
public:
    // Script-created events alive at once; further Play calls fail with a warning.
    static constexpr std::size_t MaximumEvents = 256;
    // Audio updates an event may take to report it started (FMOD Studio applies commands on its own thread). An
    // event still not seen playing after that is treated as finished.
    static constexpr std::uint64_t StartGraceUpdates = 30;

    // Audio.2A: above this speed a frame-to-frame move is a teleport (or a scene/camera cut), and its velocity is
    // reported as zero instead of producing a Doppler spike. Meters per second.
    static constexpr float MaximumPlausibleSpeed = 200.0f;

    // A world-space pose: position, and unit, perpendicular forward (+Z) and up (+Y) axes. Scale is removed.
    struct ObjectPose
    {
        std::array<float, 3> position{0.0f, 0.0f, 0.0f};
        std::array<float, 3> forward{0.0f, 0.0f, 1.0f};
        std::array<float, 3> up{0.0f, 1.0f, 0.0f};
    };

    // The current world pose of a scene object; nullopt when it no longer exists in the running scene.
    using PoseResolver = std::function<std::optional<ObjectPose>(std::string_view objectID)>;

    // A scene Audio Source of the running scene, as authored. Audio.2C keeps one runtime record per source for the
    // session; scripts change only that record, never the authored component.
    struct SceneSource
    {
        std::string objectID;
        std::string label;
        std::string eventGuid;
        bool enabled = true;
        bool autoplay = true;
    };

    // Engine's world matrices (row-indexed storage, column-vector math): translation is column 3, local +Z and +Y
    // are columns 2 and 1. Up is re-orthogonalized against forward; a degenerate basis yields nullopt.
    static std::optional<ObjectPose> PoseFromWorld(const Matrix4& world);

    GameAudioSession() = default;
    GameAudioSession(const GameAudioSession&) = delete;
    GameAudioSession& operator=(const GameAudioSession&) = delete;

    void SetAudioSystem(Audio::AudioSystem* audio) noexcept;

    // Opens a new session with a new identity; a previous one still active is ended first.
    void Begin();
    // Once per gameplay frame: releases events that have finished, and forgets events FMOD destroyed (bank unload).
    void Update();
    // Stops and releases every event of the session immediately and restores every changed mixer value, newest change
    // first. Idempotent; safe when audio is unavailable or was never started.
    void End() noexcept;

    // Audio.2C: how this session finds object poses (attachment, source playback). Set after Begin; End clears it.
    void SetPoseResolver(PoseResolver resolve);
    // Audio.2C: records every Audio Source of the running scene before any script code runs, so scripts can control
    // them from their constructors and OnStart on.
    void RegisterSceneSources(std::vector<SceneSource> sources);
    // Audio.2A autoplay, after every OnStart: starts each registered source that is enabled, set to autoplay, not
    // already playing, and not stopped by a script during startup. Each event is placed at its object before it
    // starts; missing events and pose-less objects are skipped with a warning.
    void StartAutoplaySources();
    // Audio.2A form: SetPoseResolver, RegisterSceneSources and StartAutoplaySources in one call.
    void StartSceneSources(const std::vector<SceneSource>& sources, const PoseResolver& resolve);
    // Audio.2A, once per gameplay frame after transforms are final: moves the FMOD listener to the given pose (the
    // listener object, or the camera fallback; nullopt leaves it where it is) and every object-bound event to its
    // object. Velocities are frame-to-frame displacement over `deltaSeconds`, zero on the first frame, a listener
    // change, a non-positive delta or an implausible jump. An event whose object is gone is stopped and released.
    void UpdateSpatial(double deltaSeconds, std::string_view listenerKey, const std::optional<ObjectPose>& listener,
        const PoseResolver& resolve);
    // M4: one mesh object's place in the acoustic scene.
    struct AcousticObject
    {
        std::string objectID;
        std::shared_ptr<const Audio::AcousticMeshData> mesh;
        Audio::AcousticTransform world{};
    };

    // M4: replaces this session's acoustic scene. Without snapshot synchronization there is no geometry simulation: geometry
    // is not sent, and when objects asked for it a warning says so. Project settings are never changed. End clears it.
    void PublishAcousticScene(std::vector<AcousticObject> objects);
    // M4, once per gameplay frame: a moved object is sent to the worker only when its transform really changed. Null removes it
    // (the object left the running scene).
    using AcousticTransformResolver = std::function<std::optional<Audio::AcousticTransform>(std::string_view objectID)>;
    void UpdateAcousticTransforms(const AcousticTransformResolver& resolve);
    std::size_t GetAcousticObjectCount() const noexcept { return m_acousticObjects.size(); }

    // Diagnostics and tests: the object an event is bound to (empty for script events or an invalid handle).
    std::string GetEventObject(const Scripting::AudioEventHandle& handle) const;
    // Events started for Audio Source components (autoplay or AudioSource.Play); script events are not included.
    std::vector<Scripting::AudioEventHandle> GetSceneEvents() const;
    // Diagnostics and tests: every event the session owns, script and component.
    std::vector<Scripting::AudioEventHandle> GetEvents() const;

    bool IsActive() const noexcept { return m_active; }
    std::uint64_t GetSessionID() const noexcept { return m_session; }
    std::size_t GetEventCount() const noexcept { return m_events.size(); }
    std::size_t GetChangedMixerValueCount() const noexcept { return m_restores.size(); }

    // Scripting::AudioService.
    bool PlayEvent(std::string_view event, Scripting::AudioEventHandle& handle) override;
    bool IsEventValid(const Scripting::AudioEventHandle& handle) const override;
    bool IsEventPlaying(const Scripting::AudioEventHandle& handle) const override;
    bool StopEvent(const Scripting::AudioEventHandle& handle, bool allowFadeOut) override;
    bool SetEventParameter(const Scripting::AudioEventHandle& handle, std::string_view name, float value) override;
    bool SetEventVolume(const Scripting::AudioEventHandle& handle, float volume) override;
    bool ReleaseEvent(const Scripting::AudioEventHandle& handle) override;
    bool PlayEventAt(std::string_view event, const float (&position)[3], Scripting::AudioEventHandle& handle) override;
    bool SetEventPosition(const Scripting::AudioEventHandle& handle, const float (&position)[3]) override;
    bool AttachEvent(const Scripting::AudioEventHandle& handle, std::string_view objectKey) override;
    bool DetachEvent(const Scripting::AudioEventHandle& handle) override;
    bool HasAudioSource(std::string_view objectKey) const override;
    bool PlayAudioSource(std::string_view objectKey) override;
    bool StopAudioSource(std::string_view objectKey, bool allowFadeOut) override;
    bool IsAudioSourcePlaying(std::string_view objectKey) const override;
    bool SetAudioSourceEnabled(std::string_view objectKey, bool enabled) override;
    bool IsAudioSourceEnabled(std::string_view objectKey) const override;
    bool SetGlobalParameter(std::string_view name, float value) override;
    bool SetBusVolume(std::string_view bus, float volume) override;
    bool SetVCAVolume(std::string_view vca, float volume) override;

private:
    struct OwnedEvent
    {
        Audio::EventHandle handle;
        std::uint64_t playedAtUpdate = 0;
        // FMOD has reported Starting/Playing/Sustaining/Stopping at least once.
        bool seenPlaying = false;
        // The object the event follows: an Audio Source's object, or a script attachment (Audio.2C). Empty for an
        // unattached script event. lastPosition is the last placed position, for velocities.
        std::string objectID;
        std::optional<std::array<float, 3>> lastPosition;
        // Started for an Audio Source component (not a script event).
        bool sceneSource = false;
        // SA.2: the event's FMOD Studio GUID, and whether its spatializers have been checked (CheckSpatialization).
        Audio::Guid event;
        bool spatializationChecked = false;
    };

    // Audio.2C runtime state of one scene Audio Source for this session.
    struct SourceState
    {
        SceneSource authored;
        bool enabled = true;
        // Autoplay has run, or a script started or stopped the source first: autoplay never starts it again.
        bool autoplayHandled = false;
        // The source's current event while the session owns it.
        std::optional<Audio::EventHandle> event;
    };

    enum class MixerTarget
    {
        Bus,
        VCA,
        GlobalParameter
    };

    // The value a session found before its first change of one target; restored by End.
    struct MixerRestore
    {
        MixerTarget target = MixerTarget::Bus;
        std::string name;
        float original = 0.0f;
    };

    static const char* MixerTargetName(MixerTarget target) noexcept;
    bool Running() const noexcept;
    // The session's own event for a script handle, or null (other session, stale, released, never valid).
    OwnedEvent* Find(const Scripting::AudioEventHandle& handle);
    const OwnedEvent* Find(const Scripting::AudioEventHandle& handle) const;
    void Forget(const OwnedEvent& event);
    bool HasStarted(const OwnedEvent& event, Audio::PlaybackState state) const;
    bool IsEventSounding(const OwnedEvent& event) const;
    const OwnedEvent* FindOwned(const Audio::EventHandle& handle) const;
    SourceState* FindSource(std::string_view objectKey);
    const SourceState* FindSource(std::string_view objectKey) const;
    bool StartSource(SourceState& source);
    // Creates, places and starts one event; shared by script and component playback.
    std::optional<Audio::EventHandle> StartPlaced(const Audio::Guid& guid, const std::optional<ObjectPose>& pose,
        const std::string& context);
    void StopAndRelease(std::size_t index);
    void CheckSpatialization(OwnedEvent& event);
    bool ChangeMixerValue(MixerTarget target, std::string_view name, float value);
    // Logs each distinct message once per session, so a script calling every frame cannot flood the log.
    void Warn(const std::string& message);

    Audio::AudioSystem* m_audio = nullptr;
    bool m_active = false;
    std::uint64_t m_session = 0;
    std::vector<OwnedEvent> m_events;
    std::vector<MixerRestore> m_restores;
    std::vector<SourceState> m_sources;
    PoseResolver m_resolve;
    std::set<std::string> m_warnings;
    // Audio.2A listener continuity: which listener moved last, and from where.
    std::string m_listenerKey;
    std::optional<std::array<float, 3>> m_listenerLastPosition;

    struct PublishedAcousticObject
    {
        std::string objectID;
        std::uint64_t id = 0;
        Audio::AcousticTransform world{};
    };

    std::vector<PublishedAcousticObject> m_acousticObjects;
};
