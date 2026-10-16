#pragma once

#include "Physics/PhysicsWorld.h"
#include "Physics/RuntimePoseSnapshot.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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
    void Step();

private:
    explicit ScenePhysicsSession(const WorldConfig& config);
    void RegisterObjects(const SceneDocument& document);

    // Declaration order is intentional: bodies are released before their world.
    World m_world;
    std::map<std::string, std::shared_ptr<Body>, std::less<>> m_bodies;
};
}
