#pragma once

#include "Physics/CharacterControllerComponent.h"
#include "Physics/CharacterControllerDiagnostics.h"
#include "Physics/CharacterControllerMover.h"
#include "Physics/ContactEvents.h"
#include "Physics/TriggerEvents.h"
#include "Physics/PhysicsWorld.h"
#include "Physics/RuntimePoseSnapshot.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

struct SceneDocument;

namespace Physics
{
// Temporary, single-threaded registration of a fixed authored snapshot.
// The session retains no SceneDocument reference and never changes authored transforms.
class ScenePhysicsSession
{
public:
    static std::unique_ptr<ScenePhysicsSession> Create(const SceneDocument& document,
        std::string* errorMessage = nullptr, const WorldConfig& config = {});
    ~ScenePhysicsSession();

    ScenePhysicsSession(const ScenePhysicsSession&) = delete;
    ScenePhysicsSession& operator=(const ScenePhysicsSession&) = delete;
    ScenePhysicsSession(ScenePhysicsSession&&) = delete;
    ScenePhysicsSession& operator=(ScenePhysicsSession&&) = delete;

    // A missing ID returns an empty weak reference. Lock before reading; reads on
    // a retained body throw once session destruction has invalidated its resources.
    std::weak_ptr<const Body> FindBody(std::string_view objectID) const;
    std::size_t GetBodyCount() const noexcept;

    // Complete value-owned snapshot or nullopt. Read-only; never concurrent with
    // stepping or destruction. Existing body lookups are unaffected.
    std::optional<RuntimePoseSnapshot> CapturePoses(std::string* errorMessage = nullptr) const;

    // One explicit P.1 fixed tick. No frame scheduling or authored transform writeback.
    // Order: Character Controllers consume this step's commands and move against the
    // current (start-of-tick) solid geometry; then Box3D steps once; then its touch events
    // are converted to object-level ContactEvents and TriggerEvents for this step.
    // Nothing calls into scene objects during the step.
    void Step();

    // Character Controllers are Engine-owned geometric movers, not native bodies: they
    // create no shape, contact or trigger participation. Each is keyed by stable object ID
    // and owns its runtime pose; authored transforms are never written.
    std::size_t GetCharacterControllerCount() const noexcept;
    std::optional<Pose> FindCharacterControllerPose(std::string_view objectID) const;
    // Runtime capsule center, vertical velocity and ground support (copy), or nullopt.
    std::optional<CharacterMotionState> FindCharacterMotionState(std::string_view objectID) const;

    // Read-only copies of every controller's runtime state, in stable-ID order. Bounded:
    // at most MaximumCharacterDiagnosticContacts contacts per controller.
    std::vector<CharacterControllerDiagnostics> CaptureCharacterDiagnostics() const;

    // True while the logical trigger/visitor pair has at least one live native contribution.
    bool IsTriggerOverlapping(std::string_view triggerObjectID, std::string_view otherObjectID) const;

    // Same-session lifecycle (CM.8). Retiring a controller destroys its observer, its own
    // Trigger Collider and its Kinematic host, discards its commands and support, and retires
    // every aggregate contribution naming those shapes. Replacing the observer recreates only
    // the visitor sensor with a new World shape key. A pair whose last contribution is retired
    // emits one logical Exit at the next fixed step unless a replacement contribution for the
    // same pair begins in that step. Full session teardown stays silent (no synthetic Exit).
    bool RetireCharacterController(std::string_view objectID, std::string* errorMessage = nullptr);
    bool ReplaceCharacterObserver(std::string_view objectID, std::string* errorMessage = nullptr);
    // Result of the controller's most recent fixed step, or null for an unknown ID.
    // Borrowed from the session; valid until the next Step or session destruction.
    const CharacterMoveResult* FindLastCharacterMoveResult(std::string_view objectID) const;

    // Desired world-space horizontal velocity in m/s (y must be exactly 0) for exactly one
    // future fixed step, numbered like GetCompletedStepCount (the next step is completed + 1).
    // A later submission for the same controller and step replaces the earlier one. The
    // command is consumed by that step and never reapplied; a step without a command does
    // not move. Speeds above maximumHorizontalSpeed are scaled down, keeping direction.
    // To move for N ticks, submit one command per step number. Commands are discarded with
    // the session. Rejects unknown IDs, past or too-distant steps and nonfinite input.
    bool SubmitCharacterMovement(std::string_view objectID, std::uint64_t fixedStep,
        Vector3 desiredHorizontalVelocity, std::string* errorMessage = nullptr);
    // Requests a jump at exactly one future fixed step, with the same step window and
    // identity checks as movement. Repeating a request for the same step is idempotent.
    // Consumed by that step: accepted only if grounded at the step start (vertical velocity
    // becomes jumpSpeed), otherwise discarded. No buffering, coyote time or later replay.
    bool SubmitCharacterJump(std::string_view objectID, std::uint64_t fixedStep, std::string* errorMessage = nullptr);
    static constexpr std::uint64_t MaximumCharacterCommandLeadSteps = 600;
    // Spawn admission: shapes within radius + collisionClearance + guard must all be walkable
    // floor contact (no wall/ceiling/unclassifiable overlap), and the solid capsule shrunk by
    // the contact tolerance must overlap nothing. Placements are never repaired.
    static constexpr float CharacterSpawnGuard = 0.002f;
    static constexpr float CharacterSpawnContactTolerance = 0.005f;

    // Number of completed fixed steps in this session (the last ContactEvent::fixedStep).
    std::uint64_t GetCompletedStepCount() const noexcept;

    // Returns, in ContactEventPrecedes order, every contact event produced since the
    // previous call (or since creation), and forgets them. Values only: nothing refers
    // back into the session, so results stay valid after the session is destroyed.
    std::vector<ContactEvent> TakeContactEvents();
    // Directed values from all completed steps since the previous take, then cleared.
    std::vector<TriggerEvent> TakeTriggerEvents();

    // Gameplay-facing filter over raw events taken from this session: keeps the events in
    // which at least one participant has generateContactEvents, in the same order, and
    // marks which participants opted in. Raw tracking is never filtered, so Begin/End stay
    // balanced whatever the opt-in. Opt-in is read from the authored snapshot at
    // registration; it cannot change during a session (Play locks Inspector editing).
    std::vector<ContactNotification> SelectContactNotifications(const std::vector<ContactEvent>& contactEvents) const;

private:
    explicit ScenePhysicsSession(const WorldConfig& config);
    void RegisterObjects(const SceneDocument& document);
    void RegisterCharacterControllers(const SceneDocument& document);
    std::vector<CharacterObstacle> CollectCharacterObstacles(std::string_view excludedObjectID) const;
    void SynchronizeCharacterHosts();
    void ReconcileCharacterControllers();
    void RetireTriggerShape(std::uint64_t shapeKey);
    struct CharacterControllerRuntime;
    void AdmitCharacterSpawn(const std::string& objectID, const CharacterControllerRuntime& runtime) const;
    void MoveCharacterControllers();
    void CaptureStepContactEvents();
    void ProcessStepTriggerEvents(const std::vector<SensorTouchEvent>& observations);

    struct TriggerShapeOwner
    {
        std::string objectID;
        bool sensor;
    };

    using TriggerObjectPair = std::pair<std::string, std::string>;
    using TriggerShapePair = std::pair<std::uint64_t, std::uint64_t>;
    std::unordered_map<std::uint64_t, TriggerShapeOwner> m_triggerShapeOwners;
    std::map<TriggerObjectPair, std::set<TriggerShapePair>> m_triggerOverlaps;
    std::vector<TriggerEvent> m_pendingTriggerEvents;
    // Pairs emptied by same-session retirement; resolved (Exit or continuity) at the next step.
    std::set<TriggerObjectPair> m_retiredTriggerPairs;

    // Declaration order is intentional: bodies are released before their world.
    World m_world;
    std::map<std::string, std::shared_ptr<Body>, std::less<>> m_bodies;
    // Separate membership keeps sensor-only hosts out of solid pose/contact contracts.
    // Sensors are destroyed before either kind of host body.
    std::map<std::string, Body, std::less<>> m_sensorBodies;
    std::map<std::string, Sensor, std::less<>> m_sensors;

    // Contact identity: World body keys map to the stable scene-object IDs registered
    // above. Keys are never reused within the world, so no recycled native handle can be
    // attributed to another object.
    std::unordered_map<std::uint64_t, std::string> m_objectIDsByContactKey;

    // Touching shape contacts per canonical object pair. Object-level Begin is emitted on
    // 0 -> 1 and End on 1 -> 0, so several shape contacts never duplicate transitions.
    std::map<std::pair<std::string, std::string>, int> m_touchingContactCounts;

    std::uint64_t m_completedSteps = 0;
    std::vector<ContactEvent> m_pendingContactEvents;

    // Objects whose PhysicsComponent opted into contact notifications.
    std::set<std::string, std::less<>> m_contactNotificationObjects;

    // Value-owned runtime state per controller; no reference into the authored document.
    struct CharacterControllerRuntime
    {
        CharacterControllerComponent configuration;
        Pose spawnPose;
        // Accepted current pose: object origin, orientation fixed at the authored yaw.
        Pose pose;
        // World-space offset from object origin to capsule center (rotated local offset).
        Vector3 centerOffset;
        // Capsule, gravity, jump, slope and snap constants derived once at admission.
        CharacterMotionSettings settings;
        // Runtime-only capsule center, vertical velocity and ground support.
        CharacterMotionState motion;
        // Desired horizontal velocity keyed by the fixed step that consumes it.
        std::map<std::uint64_t, Vector3> pendingVelocities;
        // Fixed steps with a pending jump request; each is consumed by its step.
        std::set<std::uint64_t> pendingJumps;
        CharacterMoveResult lastResult;
        // Moving support recorded before the native step: owning object and its pose then.
        struct MovingSupport
        {
            std::string objectID;
            Pose poseBeforeStep;
        };
        std::optional<MovingSupport> support;
        // Diagnostics only: last consumed command steps, the center the last contacts are
        // relative to, and the carrier of the most recent step.
        std::optional<std::uint64_t> lastMovementCommandStep;
        std::optional<std::uint64_t> lastJumpRequestStep;
        Vector3 contactOrigin;
        std::string carrierObjectID;
        // Native presence (declaration order: the observer is destroyed before its host).
        // Kinematic, shape-free host at the accepted pose; owns the visitor-only observer and,
        // through m_sensors, the controller's authored Trigger Collider.
        Body host;
        Sensor observer;
    };

    // Ordered by stable ID: movement processing order is deterministic.
    std::map<std::string, CharacterControllerRuntime, std::less<>> m_characterControllers;
    // Vertical world gravity for controllers (WorldConfig::gravity.y); +Y is up.
    float m_characterGravity = 0.0f;
    // Solid shapes of Dynamic/Kinematic bodies, by World shape key: possible moving supports.
    std::unordered_map<std::uint64_t, std::string> m_movingBodyShapes;
};
}
