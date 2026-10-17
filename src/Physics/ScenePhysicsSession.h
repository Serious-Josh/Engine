#pragma once

#include "Physics/ContactEvents.h"
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
    // After Box3D finishes the step, its touch events are converted to object-level
    // ContactEvents for this step; nothing calls into scene objects during the step.
    void Step();

    // Number of completed fixed steps in this session (the last ContactEvent::fixedStep).
    std::uint64_t GetCompletedStepCount() const noexcept;

    // Returns, in ContactEventPrecedes order, every contact event produced since the
    // previous call (or since creation), and forgets them. Values only: nothing refers
    // back into the session, so results stay valid after the session is destroyed.
    std::vector<ContactEvent> TakeContactEvents();

    // Gameplay-facing filter over raw events taken from this session: keeps the events in
    // which at least one participant has generateContactEvents, in the same order, and
    // marks which participants opted in. Raw tracking is never filtered, so Begin/End stay
    // balanced whatever the opt-in. Opt-in is read from the authored snapshot at
    // registration; it cannot change during a session (Play locks Inspector editing).
    std::vector<ContactNotification> SelectContactNotifications(const std::vector<ContactEvent>& contactEvents) const;

private:
    explicit ScenePhysicsSession(const WorldConfig& config);
    void RegisterObjects(const SceneDocument& document);
    void CaptureStepContactEvents();

    // Declaration order is intentional: bodies are released before their world.
    World m_world;
    std::map<std::string, std::shared_ptr<Body>, std::less<>> m_bodies;

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
};
}
