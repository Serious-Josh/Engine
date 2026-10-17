#pragma once

#include "Math/Matrix4.h"
#include "Physics/PhysicsComponent.h"
#include "Renderer/RenderObject.h"

#include <cstdint>
#include <vector>

class Scene;

// Selects the unit wireframe the collider shader generates. The value is copied into
// a root constant, so it must stay identical to the HLSL constants in EditorCollider.hlsl.
enum class ColliderOverlayShape : std::uint32_t
{
    Box = 0,
    Sphere = 1,
    Capsule = 2
};

// A Capsule needs one value beyond its matrix. The matrix scales uniformly by the radius,
// so the wireframe is generated in radius units: hemisphere centers sit at
// (0, -/+ halfCylinderHeightInRadii, 0), where halfCylinderHeightInRadii = (h / 2) / r.
struct CapsuleColliderOverlay
{
    Matrix4 colliderWorldTransform;
    float halfCylinderHeightInRadii = 0.0f;
};

// Engine owns this capture on the frame's stack. Renderer consumes it synchronously
// during Render and retains neither the matrices nor the borrowed scene identity.
struct EditorColliderOverlay
{
    // Boxes, Spheres and Capsules share one bound: their combined count may not exceed it.
    static constexpr std::size_t MaxColliders = 8192;

    // A unit box: twelve edges, two line-list endpoints each.
    static constexpr unsigned int VerticesPerBox = 24;

    // A unit sphere: three great circles in the local XY, XZ and YZ planes.
    static constexpr unsigned int SphereCircleCount = 3;
    static constexpr unsigned int SphereSegmentsPerCircle = 64;
    static constexpr unsigned int VerticesPerSphere = SphereCircleCount * SphereSegmentsPerCircle * 2;

    // A capsule along local +Y: two rings at the hemisphere centers, four straight
    // longitudinal lines, and two perpendicular meridian half-arcs per hemisphere.
    static constexpr unsigned int CapsuleRingCount = 2;
    static constexpr unsigned int CapsuleRingSegments = 64;
    static constexpr unsigned int CapsuleLongitudinalLineCount = 4;
    static constexpr unsigned int CapsuleArcCount = 4;
    static constexpr unsigned int CapsuleArcSegments = CapsuleRingSegments / 2;
    static constexpr unsigned int CapsuleRingLineCount = CapsuleRingCount * CapsuleRingSegments;
    static constexpr unsigned int CapsuleArcLineCount = CapsuleArcCount * CapsuleArcSegments;
    static constexpr unsigned int CapsuleLineCount =
        CapsuleRingLineCount + CapsuleLongitudinalLineCount + CapsuleArcLineCount;
    static constexpr unsigned int VerticesPerCapsule = CapsuleLineCount * 2;

    const Scene* scene = nullptr; // Identity comparison only; not an owning pointer.
    std::uint64_t structureRevision = 0;
    std::uint64_t rendererLifetime = 0;
    std::vector<Matrix4> boxColliderWorldTransforms;
    std::vector<Matrix4> sphereColliderWorldTransforms;
    std::vector<CapsuleColliderOverlay> capsuleColliders;

    std::size_t GetColliderCount() const noexcept;
    bool Matches(const Scene& currentScene, std::uint64_t currentRendererLifetime) const noexcept;
};

// Unit-shape endpoints in the exact draw order the shader generates. Throw
// out_of_range for a vertex index outside the shape's draw count. The capsule
// half-height is in radius units and is ignored by Box and Sphere.
Vector3 ColliderBoxVertex(unsigned int vertexIndex);
Vector3 ColliderSphereVertex(unsigned int vertexIndex);
Vector3 ColliderCapsuleVertex(unsigned int vertexIndex, float halfCylinderHeightInRadii);
unsigned int ColliderVertexCount(ColliderOverlayShape shape);
Vector3 ColliderShapeVertex(ColliderOverlayShape shape, unsigned int vertexIndex, float halfCylinderHeightInRadii = 0.0f);

// Shape-specific collider placement. Each rejects the other collider types, and none
// applies authored visual scale.
bool BuildColliderBoxMatrix(const Physics::PhysicsComponent& component, const RenderObject& object,
    bool presentationActive, Matrix4& colliderWorldTransform);
bool BuildColliderSphereMatrix(const Physics::PhysicsComponent& component, const RenderObject& object,
    bool presentationActive, Matrix4& colliderWorldTransform);
bool BuildColliderCapsule(const Physics::PhysicsComponent& component, const RenderObject& object,
    bool presentationActive, CapsuleColliderOverlay& capsuleCollider);
