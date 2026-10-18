#pragma once

#include "Math/Vector3.h"
#include "Physics/PhysicsComponent.h"
#include "Physics/TriggerColliderComponent.h"

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

// Internal, copied native observations with never-reused World shape keys, not native IDs.
// Sensor-to-sensor observations are normalized to both directions before aggregation.
struct SensorTouchEvent
{
    bool began = false;
    std::uint64_t sensorShapeKey = 0;
    std::uint64_t visitorShapeKey = 0;
};

// Capsule for Engine-owned geometric movers. Hemisphere centers are relative to the
// query origin; all lengths are unscaled meters. Movers are never native bodies.
struct MoverCapsule
{
    Vector3 lowerCenter;
    Vector3 upperCenter;
    float radius = 0.0f;
};

// One solid constraint copied from a mover collide query, relative to the query origin.
// A positive offset is penetration depth along the outward normal. shapeKey is the
// World's never-reused key (0 if the shape was never registered by this World).
struct MoverPlane
{
    std::uint64_t shapeKey = 0;
    Vector3 normal;
    float offset = 0.0f;
    Vector3 point;
};

struct MoverPlaneQuery
{
    std::vector<MoverPlane> planes;
    // Planes beyond the requested capacity; callers must treat nonzero as a failed query.
    std::size_t overflowCount = 0;
};

// Owns only the attached sensor shape; never prolongs its body or world's lifetime.
class Sensor
{
public:
    Sensor();
    ~Sensor();
    Sensor(Sensor&&) noexcept;
    Sensor& operator=(Sensor&&) noexcept;
    Sensor(const Sensor&) = delete;
    Sensor& operator=(const Sensor&) = delete;
    bool IsValid() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    explicit Sensor(std::unique_ptr<Impl> implementation);
    std::uint64_t GetShapeKey() const;
    friend class World;
    friend class ScenePhysicsSession;
};

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
    // Each shape query throws logic_error for a different collider type or shape-free host.
    BoxProperties GetBoxProperties() const;
    SphereProperties GetSphereProperties() const;
    CapsuleProperties GetCapsuleProperties() const;
    float GetMass() const;
    Vector3 GetWorldCenterOfMass() const;
    // Teleports a Kinematic body (zero velocity is preserved). Intended for shape-free
    // controller hosts between steps; throws for other body types or invalid poses.
    void SetKinematicPose(const Pose& pose);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    explicit Body(std::unique_ptr<Impl> implementation);
    void Reset();

    // The World-assigned contact key (see ContactTouchEvent). Throws for an invalid body.
    std::uint64_t GetContactKey() const;
    std::uint64_t GetShapeKey() const;

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
    // Shape-free host. It is not a registered solid/contact participant. Static hosts carry
    // authored triggers of non-controller objects; Kinematic hosts follow a controller.
    Body CreateSensorBody(const Pose& pose, BodyType bodyType = BodyType::Static);
    Sensor AttachSensor(Body& body, const TriggerColliderComponent& component);
    void SetGravity(Vector3 gravity);

    // Exactly one fixed tick; caller owns scheduling. No accumulator or time loss.
    void Step();

    // Copies the touch begin/end events produced by the most recent Step. Box3D's own
    // event arrays are only valid until the next step, so nothing native is retained.
    // Events naming a shape this World never registered (or has since destroyed) are
    // dropped rather than guessed.
    std::vector<ContactTouchEvent> CollectContactTouchEvents() const;
    std::vector<SensorTouchEvent> CollectSensorTouchEvents() const;
    bool IsShapeKeyValid(std::uint64_t shapeKey) const;

    // Solid-only mover queries. Every native sensor shape is excluded; all Static,
    // Kinematic and Dynamic solid shapes are obstacles at their current transforms.
    // Exact overlap count; firstShapeKey (optional) receives the lowest overlapping key.
    std::size_t CountSolidOverlaps(Vector3 origin, const MoverCapsule& capsule,
        std::uint64_t* firstShapeKey = nullptr) const;
    // Sorted, deduplicated World shape keys of every overlapping solid shape.
    std::vector<std::uint64_t> CollectSolidOverlapKeys(Vector3 origin, const MoverCapsule& capsule) const;
    // Planes sorted by shape key, then normal and offset, independent of native traversal.
    MoverPlaneQuery CollideSolidMover(Vector3 origin, const MoverCapsule& capsule, std::size_t capacity) const;
    // Fraction in [0, 1] of translation that is free. Initial overlap is not reported.
    float CastSolidMover(Vector3 origin, const MoverCapsule& capsule, Vector3 translation) const;

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
