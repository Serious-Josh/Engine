#pragma once

#include "Math/Vector3.h"
#include "Physics/PhysicsComponent.h"

#include <cstdint>
#include <memory>
#include <vector>

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

// Radius and local center offset are unscaled meters in the body frame.
struct SphereDefinition
{
    Pose pose;
    float radius = 0.5f;
    BodyType bodyType = BodyType::Static;
    float density = 1.0f;
    Vector3 localPositionOffset{0.0f, 0.0f, 0.0f};
    float friction = 0.5f;
    float restitution = 0.0f;
};

// Capsule along localOrientation * +Y. cylinderHeight excludes the hemispherical caps;
// the localPositionOffset moves both hemisphere centers and is not itself rotated by
// localOrientation. All lengths are unscaled meters.
struct CapsuleDefinition
{
    Pose pose;
    float radius = 0.5f;
    float cylinderHeight = 1.0f;
    Quaternion localOrientation;
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

// Copied from the registered Box3D sphere shape, rather than cached creation inputs.
struct SphereProperties
{
    float radius;
    Vector3 localPositionOffset;
    float density;
    float friction;
    float restitution;
};

// Copied from the registered Box3D capsule shape: hemisphere centers in the body frame.
struct CapsuleProperties
{
    Vector3 lowerCenter;
    Vector3 upperCenter;
    float radius;
    float density;
    float friction;
    float restitution;
};

// One native touch transition between two registered bodies, copied out of Box3D after a
// step. Body keys are World-assigned, world-unique and never reused; they are not Box3D
// handles. Used by ScenePhysicsSession to build object-level contact events.
struct ContactTouchEvent
{
    bool began = false;
    std::uint64_t bodyKeyA = 0;
    std::uint64_t bodyKeyB = 0;
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
    // Each shape query throws logic_error when the body holds a different collider type.
    BoxProperties GetBoxProperties() const;
    SphereProperties GetSphereProperties() const;
    CapsuleProperties GetCapsuleProperties() const;
    float GetMass() const;
    Vector3 GetWorldCenterOfMass() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    explicit Body(std::unique_ptr<Impl> implementation);
    void Reset();

    // The World-assigned contact key (see ContactTouchEvent). Throws for an invalid body.
    std::uint64_t GetContactKey() const;

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
    Body CreateSphere(const SphereDefinition& definition);
    Body CreateCapsule(const CapsuleDefinition& definition);
    void SetGravity(Vector3 gravity);

    // Exactly one fixed tick; caller owns scheduling. No accumulator or time loss.
    void Step();

    // Copies the touch begin/end events produced by the most recent Step. Box3D's own
    // event arrays are only valid until the next step, so nothing native is retained.
    // Events naming a shape this World never registered (or has since destroyed) are
    // dropped rather than guessed.
    std::vector<ContactTouchEvent> CollectContactTouchEvents() const;

private:
    // Creates the body only; the caller attaches its single shape. RAII releases the
    // body if the caller throws before publishing the wrapper.
    std::unique_ptr<Body::Impl> CreateBodyImplementation(const Pose& pose, BodyType bodyType);

    // Assigns the contact key, records the shape for event mapping and transfers ownership
    // to the returned wrapper. Called only after every creation check has succeeded.
    Body PublishBody(std::unique_ptr<Body::Impl> bodyImplementation);

    std::shared_ptr<WorldState> m_state;
    int m_substeps;
};
}
