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

private:
    friend class ScriptHostVerification;
    Result InitializeWithLogDeclaration(const char* declaration);
    Result BuildClosure(std::string_view rootSection, const SourceReader& reader, SourceLimits limits, ModuleID* keep);
    Result BuildSnapshotClosure(const SourceSnapshot& snapshot, SourceLimits limits, ModuleID* keep);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
