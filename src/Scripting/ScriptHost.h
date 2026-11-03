#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "Scripting/ScriptSources.h"

namespace Scripting
{
enum class Status
{
    Ready,
    Compiled,
    Finished,
    NotInitialized,
    InvalidRequest,
    Failed,
    MissingFunction,
    Exception,
    BudgetExceeded,
    Suspended
};

struct Diagnostic
{
    std::string section;
    int line = 0;
    int column = 0;
    std::string severity;
    std::string text;
    std::string assetReference;
    std::string sourceRevision;
};

struct Result
{
    Status status = Status::Failed;
    std::vector<Diagnostic> diagnostics;
    std::shared_ptr<const SourceSnapshot> snapshot;
    // Qualified names of the instantiable EAScript classes a successful validation or build found.
    std::vector<std::string> classes;
};

struct ExecutionLimits
{
    std::size_t statements = 100000;
    std::chrono::milliseconds timeout{100};
};

// AS.5 answers for the owner handles scripts hold. The host never owns a resolver; the gameplay
// runtime installs one only while the objects it describes are alive. Slots are runtime-local.
class ObjectResolver
{
public:
    virtual ~ObjectResolver() = default;
    virtual bool IsValid(std::uint32_t slot) const = 0;
    virtual std::string GetName(std::uint32_t slot) const = 0;
    virtual bool GetPosition(std::uint32_t slot, float (&position)[3]) const = 0;

    // AS.6A Character Controller requests. The defaults describe an object without one.
    // Move and Jump only record a request for the runtime; they never move anything directly.
    virtual bool HasCharacterController(std::uint32_t) const
    {
        return false;
    }

    virtual bool MoveCharacter(std::uint32_t, const float (&)[3], std::string& error)
    {
        error = "The object has no Character Controller.";
        return false;
    }

    virtual bool JumpCharacter(std::uint32_t, std::string& error)
    {
        error = "The object has no Character Controller.";
        return false;
    }

    virtual bool IsCharacterGrounded(std::uint32_t) const
    {
        return false;
    }

    // AS.6B controller facing (radians about world +Y). Setting it is immediate and runtime-only.
    virtual bool GetCharacterYaw(std::uint32_t, float&) const
    {
        return false;
    }

    virtual bool SetCharacterYaw(std::uint32_t, float, std::string& error)
    {
        error = "The object has no Character Controller.";
        return false;
    }

    // AS.6B hierarchy and runtime transforms. FindChild returns a new slot for a direct child.
    virtual bool FindChild(std::uint32_t, std::string_view, std::uint32_t&)
    {
        return false;
    }

    virtual bool GetLocalRotation(std::uint32_t, float (&)[3]) const
    {
        return false;
    }

    virtual bool SetLocalRotation(std::uint32_t, const float (&)[3], std::string& error)
    {
        error = "The object cannot be rotated at runtime.";
        return false;
    }

    virtual bool GetForward(std::uint32_t, float (&)[3]) const
    {
        return false;
    }

    // Audio.2C: the object's persistent scene ID, the key gameplay audio uses; empty when the slot is not valid.
    virtual std::string GetObjectKey(std::uint32_t) const
    {
        return {};
    }
};

// AS.6A keys scripts may query, mirrored by the script enum Key; hosts map them to platform keys.
enum class Key : int
{
    W,
    A,
    S,
    D,
    Space
};

// Gameplay keyboard state for one dispatch. Installed only while callbacks run; without one every
// query answers false (no gameplay focus).
class InputSource
{
public:
    virtual ~InputSource() = default;
    virtual bool IsKeyDown(Key key) const = 0;
    virtual bool WasKeyPressed(Key key) const = 0;

    // AS.6B raw relative mouse motion of this frame in pixels; zero unless the cursor is captured.
    virtual void GetMouseDelta(float (&delta)[2]) const
    {
        delta[0] = 0.0f;
        delta[1] = 0.0f;
    }

    // Records the script's cursor-lock request; the host decides when the lock is granted.
    virtual void SetCursorLocked(bool)
    {
    }
};

// Audio.1C script-visible event handle: plain data copied by value. `session` is the gameplay audio session that
// issued it, so a handle never reaches an instance of another Play session. The default handle is invalid.
struct AudioEventHandle
{
    std::uint64_t session = 0;
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
};

// Audio.1C gameplay audio. Installed only while the gameplay session is running; without one every call fails
// quietly (scripts keep running, events are not played). Implementations validate every handle and value and never
// throw; expected misuse (stale handles) answers false, actionable problems are reported by the implementation.
class AudioService
{
public:
    virtual ~AudioService() = default;

    // `event` is "{guid}" (FMOD Studio's event GUID text) or "event:/path" (needs the strings bank). On success the
    // event is playing and `handle` refers to it.
    virtual bool PlayEvent(std::string_view event, AudioEventHandle& handle) = 0;
    virtual bool IsEventValid(const AudioEventHandle& handle) const = 0;
    // True from Play until the event has stopped (including a fade-out in progress).
    virtual bool IsEventPlaying(const AudioEventHandle& handle) const = 0;
    virtual bool StopEvent(const AudioEventHandle& handle, bool allowFadeOut) = 0;
    virtual bool SetEventParameter(const AudioEventHandle& handle, std::string_view name, float value) = 0;
    virtual bool SetEventVolume(const AudioEventHandle& handle, float volume) = 0;
    // Stops the event (allowing its authored fade-out) and gives it back; the handle becomes invalid at once.
    virtual bool ReleaseEvent(const AudioEventHandle& handle) = 0;

    // Audio.2C positional events. Positions are world-space meters (Engine and FMOD share +X right, +Y up, +Z
    // forward). Whether an event is 3D, loops or attenuates stays as authored in FMOD Studio.
    virtual bool PlayEventAt(std::string_view event, const float (&position)[3], AudioEventHandle& handle) = 0;
    // Places the event; an attached event is detached first (an explicit position wins).
    virtual bool SetEventPosition(const AudioEventHandle& handle, const float (&position)[3]) = 0;
    // The event follows the object's world pose every frame and stops when the object leaves the scene.
    virtual bool AttachEvent(const AudioEventHandle& handle, std::string_view objectKey) = 0;
    // Stops following; the event stays where the object last placed it.
    virtual bool DetachEvent(const AudioEventHandle& handle) = 0;

    // Audio.2C runtime control of an object's authored Audio Source. `objectKey` is GetObjectKey's ID (empty for a
    // stale object). Runtime state lasts for the session only; the authored component never changes.
    virtual bool HasAudioSource(std::string_view objectKey) const = 0;
    // Starts the source's event unless it is already playing (never a second instance) or the source is disabled.
    virtual bool PlayAudioSource(std::string_view objectKey) = 0;
    // Stops the source's event; also cancels a pending autoplay when called during OnStart.
    virtual bool StopAudioSource(std::string_view objectKey, bool allowFadeOut) = 0;
    virtual bool IsAudioSourcePlaying(std::string_view objectKey) const = 0;
    // Disabling stops a playing source (allowing its fade-out) and makes Play and autoplay ignore it.
    virtual bool SetAudioSourceEnabled(std::string_view objectKey, bool enabled) = 0;
    virtual bool IsAudioSourceEnabled(std::string_view objectKey) const = 0;

    // Session-scoped mixer changes: the session restores every value it changed when gameplay stops.
    virtual bool SetGlobalParameter(std::string_view name, float value) = 0;
    virtual bool SetBusVolume(std::string_view bus, float volume) = 0;
    virtual bool SetVCAVolume(std::string_view vca, float volume) = 0;
};

// Indices into one host's built modules and created instances.
using ModuleID = std::size_t;
using InstanceID = std::size_t;

enum class Lifecycle
{
    Start,
    Stop
};

// Construct, use and destroy on one owning thread. No VM/context pointers escape this API.
// Every built module includes the engine section defining the EAScript base class.
class ScriptHost final
{
public:
    ScriptHost();
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    Result Initialize();
    Result Shutdown();
    Result Compile(std::string_view section, std::string_view source);
    // Nonexecuting validation only: does not install an executable module.
    Result ValidateSources(std::string_view rootSection, const SourceReader& reader, SourceLimits limits = {});
    Result ValidateSnapshot(const SourceSnapshot& snapshot, SourceLimits limits = {});
    // AS.2 permits only a global void function with no arguments, executed synchronously.
    Result Execute(std::string_view declaration, ExecutionLimits limits = {});

    // AS.5: validates and keeps one EAScript module for a source closure in a single compile.
    // Runs no script code; global variables stay forbidden, so modules hold no state.
    Result BuildSources(std::string_view rootSection, const SourceReader& reader, ModuleID& module,
        SourceLimits limits = {});
    // Same for a memory snapshot (packaged games): include policy and revision are rechecked.
    Result BuildSnapshot(const SourceSnapshot& snapshot, ModuleID& module, SourceLimits limits = {});
    // Constructs one instance of an eligible class of a built module (its constructor runs under
    // the limits) and binds its owner handle to the resolver slot.
    Result CreateInstance(ModuleID module, std::string_view className, std::uint32_t ownerSlot,
        InstanceID& instance, ExecutionLimits limits = {});
    // Runs one optional lifecycle callback; a class without an override runs EAScript's empty one.
    Result Invoke(InstanceID instance, Lifecycle callback, ExecutionLimits limits = {});
    // AS.6A: runs OnUpdate(float deltaTime) under the same limits and fault reporting.
    Result Update(InstanceID instance, float deltaTime, ExecutionLimits limits = {});
    void SetObjectResolver(ObjectResolver* resolver) noexcept;
    void SetInputSource(InputSource* input) noexcept;
    // Audio.1C: the gameplay audio session; scripts reach it only while their code executes.
    void SetAudioService(AudioService* audio) noexcept;

private:
    friend class ScriptHostVerification;
    Result InitializeWithLogDeclaration(const char* declaration);
    Result BuildClosure(std::string_view rootSection, const SourceReader& reader, SourceLimits limits, ModuleID* keep);
    Result BuildSnapshotClosure(const SourceSnapshot& snapshot, SourceLimits limits, ModuleID* keep);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
