#pragma once

#include "Engine/GameAudioSession.h"
#include "assets/AcousticMaterial.h"
#include "assets/ImportedMesh.h"
#include "Engine/PhysicsSimulationController.h"
#include "Scene/RenderView.h"
#include "Scripting/ScriptHost.h"
#include "assets/AssetID.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Input;

// Gameplay lifecycle shared by Editor Play and the packaged runtime (AS.4). Hosts own the
// window, presentation surface and failure policy; gameplay state changes only through here.
// The runtime scene is the authored scene's runtime instance: physics simulates a captured
// copy of the document and presents poses through a lease that Stop clears, so the authored
// document and Editor history never observe gameplay. AS.5 script instances live here too.
class Mesh;

class GameRuntime
{
public:
    struct Frame
    {
        double deltaSeconds = 0.0;
        // Gameplay input for this frame; null while gameplay does not own input (Editor
        // viewport unfocused or ImGui capturing the keyboard). Scripts read it during OnUpdate.
        const Input* input = nullptr;
        // The host granted the cursor lock this frame: raw mouse motion reaches scripts only then.
        bool cursorCaptured = false;
    };

    // Compiles one Script asset into a module of the session's host: project files in the
    // Editor, the cooked source snapshot in packaged games. Everything after it is shared.
    using ScriptModuleSource = std::function<Scripting::Result(Scripting::ScriptHost&, const AssetID&,
        Scripting::ModuleID&)>;

    // Borrows the Engine's only simulation controller, so Game Play and the Editor's Physics
    // Simulation tool can never drive the scene at the same time.
    explicit GameRuntime(PhysicsSimulationController& physics) : m_physics(physics) {}
    GameRuntime(const GameRuntime&) = delete;
    GameRuntime& operator=(const GameRuntime&) = delete;

    // Audio.1C: the process's AudioSystem, borrowed by every gameplay audio session. Set once by the host; null
    // (or audio not running) leaves scripts' audio calls as quiet no-ops.
    void SetAudioSystem(Audio::AudioSystem* audio) noexcept { m_audio.SetAudioSystem(audio); }

    // M4: what a Play needs to build its acoustic scene. Set by the host before Start; both may be empty (no acoustics).
    struct AcousticAssets
    {
        std::function<const ImportedMesh*(const Mesh&)> meshGeometry;
        std::function<std::optional<AcousticMaterialData>(const std::string& reference, std::string* error)> material;
    };

    void SetAcousticAssets(AcousticAssets assets) { m_acousticAssets = std::move(assets); }
    const GameAudioSession& GetAudioSession() const noexcept { return m_audio; }

    // Admits the Main Camera (when required), compiles and validates every enabled Script
    // component, starts physics, constructs one instance per scripted object and runs OnStart.
    // Any failure before OnStart leaves nothing running. Legacy archives without Camera
    // components pass requireMainCamera = false.
    bool Start(FileSceneRuntime& runtime, Renderer& renderer, bool requireMainCamera,
        std::uint32_t viewWidth, std::uint32_t viewHeight, std::string& error,
        const ScriptModuleSource& scripts = {});
    // One gameplay frame: OnUpdate for every started script, their Character Controller requests
    // submitted for exactly the fixed steps this frame runs, then physics and presentation.
    bool Update(FileSceneRuntime& runtime, Renderer& renderer, const Frame& frame, std::string& error);
    // The active gameplay camera, resolved every frame from accepted poses; nullopt keeps the
    // host's default view (legacy archives only).
    bool ResolveView(const FileSceneRuntime& runtime, std::uint32_t width, std::uint32_t height,
        std::optional<RenderView>& view, std::string& error) const;
    // Runs OnStop for every successfully started instance, destroys all script state, then
    // discards physics. Returns false while presentation cleanup still needs a retry; the host
    // repeats the controller's Stop at later boundaries.
    bool Stop(FileSceneRuntime& runtime, Renderer& renderer);

    // False as soon as the controller session it started ends, whoever stopped it.
    bool IsActive() const noexcept
    {
        return m_running && m_physicsSession != 0 && m_physics.GetSessionGeneration() == m_physicsSession;
    }

    // Started and not yet stopped: script instances may still be awaiting OnStop.
    bool IsRunning() const noexcept { return m_running; }
    // AS.6B: a script asked for the cursor lock (Input::SetCursorLocked); hosts grant it only
    // while gameplay has focus and release it themselves on Home, focus loss and Stop.
    bool WantsCursorLock() const noexcept { return m_running && m_cursorLockRequested; }
    // First runtime script error (constructor excluded) of the current session; empty if none.
    const std::string& GetScriptFault() const noexcept { return m_scriptFault; }
    std::size_t GetScriptInstanceCount() const noexcept { return m_scriptInstances.size(); }

private:
#if defined(ENGINE_HAS_VERIFICATION)
    friend class GamePlayGPUVerification;
#endif
    // Owner handles of one session resolve through this table: slot -> persistent object ID of
    // the runtime scene the session started on. No pointer is retained beyond a dispatch.
    // AS.6A per-object Character Controller requests of the current frame. Movement is held for
    // every fixed step the frame runs, then cleared; a jump waits for the next frame that runs one.
    struct CharacterRequest
    {
        Vector3 velocity;
        bool moving = false;
        bool jump = false;
    };

    class ObjectTable final : public Scripting::ObjectResolver
    {
    public:
        GameRuntime* owner = nullptr;
        FileSceneRuntime* runtime = nullptr;
        std::uint64_t runtimeIdentity = 0;
        std::vector<std::string> objectIDs;
        std::vector<CharacterRequest> requests;

        bool IsValid(std::uint32_t slot) const override;
        std::string GetName(std::uint32_t slot) const override;
        bool GetPosition(std::uint32_t slot, float (&position)[3]) const override;
        bool HasCharacterController(std::uint32_t slot) const override;
        bool MoveCharacter(std::uint32_t slot, const float (&velocity)[3], std::string& error) override;
        bool JumpCharacter(std::uint32_t slot, std::string& error) override;
        bool IsCharacterGrounded(std::uint32_t slot) const override;
        bool GetCharacterYaw(std::uint32_t slot, float& yaw) const override;
        bool SetCharacterYaw(std::uint32_t slot, float yaw, std::string& error) override;
        bool FindChild(std::uint32_t slot, std::string_view name, std::uint32_t& child) override;
        bool GetLocalRotation(std::uint32_t slot, float (&rotation)[3]) const override;
        bool SetLocalRotation(std::uint32_t slot, const float (&rotation)[3], std::string& error) override;
        bool GetForward(std::uint32_t slot, float (&forward)[3]) const override;
        std::string GetObjectKey(std::uint32_t slot) const override;

    private:
        const SceneObjectDocument* Find(std::uint32_t slot) const;
        std::optional<RenderObjectID> RuntimeIndex(std::uint32_t slot) const;
    };

    // Maps the script Key enum to this frame's Input; installed only during OnUpdate.
    class GameplayInput final : public Scripting::InputSource
    {
    public:
        const Input* input = nullptr;
        bool cursorCaptured = false;
        bool* cursorLockRequest = nullptr;

        bool IsKeyDown(Scripting::Key key) const override;
        bool WasKeyPressed(Scripting::Key key) const override;
        void GetMouseDelta(float (&delta)[2]) const override;
        void SetCursorLocked(bool locked) override;
    };

    struct PendingScript
    {
        std::uint32_t slot = 0;
        Scripting::ModuleID module = 0;
        std::string className;
        std::string label;
    };

    struct ScriptInstance
    {
        Scripting::InstanceID instance = 0;
        std::uint32_t slot = 0;
        std::string label;
        bool started = false;
        // Cleared by an OnUpdate fault: the instance gets no further updates (OnStop still runs).
        bool updating = false;
    };

    bool PrepareScripts(FileSceneRuntime& runtime, const ScriptModuleSource& scripts,
        std::vector<PendingScript>& pending, std::string& error);
    bool CreateScriptInstances(const std::vector<PendingScript>& pending, std::string& error);
    void RunScriptCallbacks(FileSceneRuntime& runtime, Scripting::Lifecycle callback);
    void RunScriptUpdates(FileSceneRuntime& runtime, const Frame& frame);
    void SubmitCharacterRequests(double elapsedSeconds);
    void ReportScriptFault(const std::string& message);
    // Audio.2C: selects and places the listener and gives the audio session the scene's Audio Sources and object
    // poses, before any script code runs.
    void BeginSceneAudio(FileSceneRuntime& runtime);
    void PublishAcousticScene(FileSceneRuntime& runtime);
    // Audio.2A autoplay after OnStart (the listener was selected by BeginSceneAudio).
    void StartSceneAudio(FileSceneRuntime& runtime);
    // Audio.2A: moves the FMOD listener and every object-bound event to the presented world poses of this frame.
    void UpdateSceneAudio(const FileSceneRuntime& runtime, double deltaSeconds);
    void ReleaseScripts() noexcept;

    PhysicsSimulationController& m_physics;
    std::uint64_t m_physicsSession = 0;
    std::uint64_t m_runtimeIdentity = 0;
    bool m_mainCamera = false;
    bool m_running = false;
    bool m_cursorLockRequested = false;
    std::unique_ptr<Scripting::ScriptHost> m_scripts;
    ObjectTable m_objects;
    GameplayInput m_input;
    // Audio.1C: begins before script constructors and OnStart, ends after OnStop (or on any failed Start).
    GameAudioSession m_audio;
    AcousticAssets m_acousticAssets;
    // Audio.2A: the active Audio Listener's object (first enabled in scene order), or empty for the camera fallback.
    std::string m_listenerObjectID;
    std::vector<ScriptInstance> m_scriptInstances;
    std::string m_scriptFault;
};
