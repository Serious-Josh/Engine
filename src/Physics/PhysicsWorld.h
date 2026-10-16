#pragma once

#include "Math/Vector3.h"
#include "Physics/PhysicsComponent.h"

#include <memory>

namespace Physics
{
// Active right-hand rotations, vector part xyz followed by scalar w.
struct Quaternion
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct Pose
{
    Vector3 position;
    Quaternion orientation;
};

struct WorldConfig
{
    Vector3 gravity{0.0f, -9.81f, 0.0f};
    int substeps = 4;
};

struct BoxDefinition
{
    Pose pose;
    Vector3 halfExtents{0.5f, 0.5f, 0.5f};
    BodyType bodyType = BodyType::Static;
    float density = 1.0f;
    Vector3 localPositionOffset{0.0f, 0.0f, 0.0f};
    float friction = 0.5f;
    float restitution = 0.0f;
};

// Copied from the registered Box3D shape, rather than cached creation inputs.
struct BoxProperties
{
    Vector3 halfExtents;
    Vector3 localPositionOffset;
    float density;
    float friction;
    float restitution;
};

struct WorldState;

// Owns a body while its world exists. World destruction invalidates the wrapper.
class Body
{
public:
    Body();
    ~Body();

    Body(Body&&) noexcept;
    Body& operator=(Body&&) noexcept;
    Body(const Body&) = delete;
    Body& operator=(const Body&) = delete;

    bool IsValid() const;
    Pose GetPose() const;
    bool IsAwake() const;
    bool HasContact() const;
    BodyType GetType() const;
    BoxProperties GetBoxProperties() const;
    float GetMass() const;
    Vector3 GetWorldCenterOfMass() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    explicit Body(std::unique_ptr<Impl> implementation);
    void Reset();
    friend class World;
    friend class ScenePhysicsSession;
};

// Single-threaded simulation owner; independent of authored scene transforms.
class World
{
public:
    static constexpr float FixedTimeStep = 1.0f / 60.0f;

    explicit World(const WorldConfig& config = {});
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

    Body CreateBox(const BoxDefinition& definition);
    void SetGravity(Vector3 gravity);

    // Exactly one fixed tick; caller owns scheduling. No accumulator or time loss.
    void Step();

private:
    std::shared_ptr<WorldState> m_state;
    int m_substeps;
};
}
