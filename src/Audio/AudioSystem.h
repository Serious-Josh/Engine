#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Audio
{
// FMOD Studio's 128-bit identifier of an event, bank, bus or VCA (FMOD_GUID's layout). Event GUIDs are the stable
// references Engine stores: they survive renames and moves in FMOD Studio.
struct Guid
{
    std::uint32_t data1 = 0;
    std::uint16_t data2 = 0;
    std::uint16_t data3 = 0;
    std::array<std::uint8_t, 8> data4{};

    bool operator==(const Guid&) const = default;
};

// FMOD's text form, "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" (as exported by FMOD Studio's GUIDs.txt); lowercase out.
std::optional<Guid> ParseGuid(std::string_view text);
std::string ToString(const Guid& guid);

struct EventInfo
{
    Guid id;
    // "event:/..." for display and debugging; empty unless a strings bank naming the event is loaded.
    std::string path;
};

// An event instance created by AudioSystem. A value: it does not own the instance and becomes stale (every query
// answers "invalid") once the instance is released, its bank is unloaded or audio shuts down.
struct EventHandle
{
    std::uint32_t index = 0;
    // 0 is never issued, so a default handle is always invalid.
    std::uint32_t generation = 0;

    bool operator==(const EventHandle&) const = default;
};

// Audio.2A world-space 3D attributes, in Engine's and FMOD's shared convention: left-handed, +X right, +Y up,
// +Z forward, meters and meters per second. FMOD is initialized without FMOD_INIT_3D_RIGHTHANDED, so values pass
// through unchanged. forward and up must be unit length and perpendicular (FMOD's requirement).
struct Attributes3D
{
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> velocity{0.0f, 0.0f, 0.0f};
    std::array<float, 3> forward{0.0f, 0.0f, 1.0f};
    std::array<float, 3> up{0.0f, 1.0f, 0.0f};

    bool operator==(const Attributes3D&) const = default;
};

enum class PlaybackState
{
    // The handle is stale or was never valid.
    Invalid,
    Starting,
    Playing,
    Sustaining,
    Stopping,
    Stopped
};

enum class Status
{
    // Initialize has not run, or Shutdown has completed.
    NotStarted,
    // FMOD is running and mixing to an output device.
    Active,
    // FMOD is running with its no-sound output because no usable output device was available.
    NoOutput,
    // This build has no FMOD backend (ENGINE_ENABLE_FMOD=OFF or the SDK was not found), or the host chose not to
    // start audio.
    Disabled,
    // FMOD failed for a reason other than the output device; Engine continues without audio.
    Unavailable
};

std::string_view ToString(Status status);

// Controlled failures for verification only: hardware and SDK failures that cannot be produced on demand.
// Production hosts always pass None.
enum class InjectedFailure
{
    None,
    // The device output fails to initialize, as when no usable device exists; the no-sound retry runs.
    OutputInitialization,
    // The no-sound retry fails too, after the device output failed.
    NoSoundInitialization,
    // Studio system creation fails before anything was created.
    StudioCreation,
    // Studio initialization fails for a non-output reason after the Studio system was created.
    StudioInitialization
};

struct InitializeOptions
{
    // Skips the output device and starts with FMOD's no-sound output directly (headless diagnostics).
    bool noSoundOutput = false;
    InjectedFailure injectedFailure = InjectedFailure::None;
};

// Engine-owned FMOD Studio system (Audio.1A). Construct, use and destroy on the main thread. FMOD types never appear
// in this interface; the Studio system, and the Core system it owns, stay private to the audio module.
//
// At most one AudioSystem holds an FMOD Studio system per process: a second Initialize while another instance is
// running reports Unavailable without creating anything.
class AudioSystem final
{
public:
    AudioSystem();
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // True when this build was compiled against the FMOD SDK.
    static bool IsBackendCompiled() noexcept;
    // Bytes FMOD currently holds across the process (FMOD::Memory_GetStats), for leak checks; -1 when the backend
    // is not compiled or its runtime libraries were never loaded.
    static std::int64_t GetBackendAllocatedBytes() noexcept;

    // Starts FMOD and returns the resulting status. Only device output failures fall back to the no-sound output;
    // any other failure releases everything created so far and reports Unavailable. Never throws, and never
    // prevents the caller from continuing without audio. Calling it while already started changes nothing.
    Status Initialize(const InitializeOptions& options = {});

    // Leaves the session in its current state: a host that never wants audio records Disabled with a reason.
    void Disable(std::string_view reason);

    // Advances FMOD Studio by one Engine frame: flushes queued Studio commands to FMOD's own threads. Does not wait
    // for the mixer or for any GPU work. A no-op unless Active or NoOutput.
    void Update() noexcept;

    // Stops and releases every tracked event instance, unloads every bank, then releases the Studio system, which
    // releases its Core system. Idempotent; safe after any partial failure.
    void Shutdown() noexcept;

    Status GetStatus() const noexcept;
    // Why the status is what it is, for logs and diagnostics UI ("output device 'Speakers'", the FMOD error, ...).
    const std::string& GetStatusDetail() const noexcept;
    // Successful Studio updates since the last Initialize.
    std::uint64_t GetUpdateCount() const noexcept;

    // --- Banks (Audio.1B) ---------------------------------------------------------------------------------------
    // Banks are keyed by the caller's stable identity (the AudioBank asset reference, "asset:<uuid>"). Loading a key
    // that is already loaded adds a reference; each load needs one UnloadBank. Loads are synchronous: on success the
    // bank's metadata (events, buses) is available. Sample data loads on demand when events play. Every function
    // fails with an error while audio is not running (Disabled, Unavailable, NotStarted).

    // Development: FMOD reads the file at `path` (the file must stay in place while the bank is loaded; FMOD
    // streams from it). Fails without loading anything for a missing, unreadable or corrupt file, or when the same
    // bank is already loaded under a different key.
    bool LoadBankFile(std::string_view key, const std::filesystem::path& path, std::string& error);
    // Packaged games: FMOD copies `bytes` before returning (FMOD_STUDIO_LOAD_MEMORY), so the caller may free them at
    // once. Same reference and failure rules as LoadBankFile.
    bool LoadBankMemory(std::string_view key, std::span<const std::byte> bytes, std::string& error);
    // Releases one reference; the last one unloads the bank, which destroys every event instance of its events
    // (their handles become stale). False for a key that is not loaded.
    bool UnloadBank(std::string_view key);
    bool IsBankLoaded(std::string_view key) const;
    std::size_t GetLoadedBankCount() const noexcept;
    // Every event of one loaded bank, in FMOD's order; empty for an unknown key.
    std::vector<EventInfo> GetBankEvents(std::string_view key) const;
    // Audio.2B: every event of every loaded bank, each once, sorted by path (events without a path last, by GUID).
    std::vector<EventInfo> GetLoadedEvents() const;

    // --- Events (Audio.1B: internal; scripts reach them through Audio.1C's session API) -------------------------
    // Resolves "event:/..." to its GUID; needs the strings bank that names it.
    std::optional<Guid> FindEvent(std::string_view path) const;
    // True when a loaded bank defines the event.
    bool HasEvent(const Guid& event) const;
    // The event's path, or empty without a strings bank naming it (or for an unknown event).
    std::string GetEventPath(const Guid& event) const;

    // Creates one instance of a loaded event; AudioSystem tracks it until ReleaseEvent, its bank unloads or Shutdown.
    // The returned handle is the caller's only way to reach it.
    std::optional<EventHandle> CreateEventInstance(const Guid& event, std::string& error);
    bool StartEvent(EventHandle handle);
    bool StopEvent(EventHandle handle, bool allowFadeOut);
    // Gives the instance back to FMOD, which destroys it once it has stopped (a looping event must be stopped
    // first). The handle is stale from this call on.
    bool ReleaseEvent(EventHandle handle);
    PlaybackState GetPlaybackState(EventHandle handle) const;
    // Tracked instances whose handles are still valid.
    std::size_t GetEventInstanceCount() const;

    // --- Audio.1C control ----------------------------------------------------------------------------------------
    // Volumes are linear gains in [0, MaximumVolume]; parameter values must be finite (FMOD clamps them to the
    // parameter's authored range). Invalid values and unknown names fail with an error and change nothing.
    static constexpr float MaximumVolume = 10.0f;

    // "{guid}" (FMOD Studio's GUID text) or "event:/path" (needs the strings bank) to an event of a loaded bank.
    std::optional<Guid> ResolveEventReference(std::string_view reference) const;
    bool SetEventParameter(EventHandle handle, std::string_view name, float value, std::string& error);
    bool SetEventVolume(EventHandle handle, float volume, std::string& error);

    // Buses ("bus:/..." or "{guid}"), VCAs ("vca:/..." or "{guid}") and global parameters (by name). Getters answer
    // the value last set through FMOD (the authored default until then); nullopt for an unknown object.
    std::optional<float> GetBusVolume(std::string_view bus) const;
    bool SetBusVolume(std::string_view bus, float volume, std::string& error);
    std::optional<float> GetVCAVolume(std::string_view vca) const;
    bool SetVCAVolume(std::string_view vca, float volume, std::string& error);
    std::optional<float> GetGlobalParameter(std::string_view name) const;
    bool SetGlobalParameter(std::string_view name, float value, std::string& error);

    // --- Audio.2A 3D --------------------------------------------------------------------------------------------
    // Whether a loaded event is spatialized (authored in FMOD Studio); false for unknown events.
    bool IsEvent3D(const Guid& event) const;
    // Fails for non-finite values or a forward/up pair that is not unit length and perpendicular.
    bool SetEventAttributes(EventHandle handle, const Attributes3D& attributes, std::string& error);
    std::optional<Attributes3D> GetEventAttributes(EventHandle handle) const;
    // FMOD listener 0, the only listener Engine uses.
    bool SetListenerAttributes(const Attributes3D& attributes, std::string& error);
    std::optional<Attributes3D> GetListenerAttributes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Runs AudioSystem::Update once when a frame scope ends, on every return path of the frame.
class FrameUpdateScope final
{
public:
    explicit FrameUpdateScope(AudioSystem& audio) noexcept : m_audio(audio) {}
    ~FrameUpdateScope() { m_audio.Update(); }
    FrameUpdateScope(const FrameUpdateScope&) = delete;
    FrameUpdateScope& operator=(const FrameUpdateScope&) = delete;

private:
    AudioSystem& m_audio;
};
}
