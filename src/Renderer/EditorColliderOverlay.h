#pragma once

#include "Math/Matrix4.h"
#include "Physics/CharacterControllerComponent.h"
#include "Physics/CharacterControllerDiagnostics.h"
#include "Physics/PhysicsComponent.h"
#include "Physics/TriggerColliderComponent.h"
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
    Capsule = 2,
    Cone = 3,  // LIGHT.1 Spot Light: apex at the origin, unit ring at z = 1.
    Arrows = 4 // LIGHT.1 Directional Light: parallel unit arrows along +Z.
};

// Also selects the wireframe color in EditorCollider.hlsl; keep the values identical.
enum class ColliderOverlayRole : std::uint32_t
{
    Solid = 0,               // Green: PhysicsComponent colliders.
    Trigger = 1,             // Cyan: Trigger Colliders.
    CharacterController = 2, // Amber: Character Controller capsule (authored, or accepted runtime pose in Play).
    CharacterAlert = 3,      // Red: capsule whose last step was rejected or left a moving-body overlap Unresolved.
    GroundSupport = 4,       // Yellow: ground support point and normal.
    CharacterContact = 5,    // Magenta: solid contact points and normals.
    CameraFrustum = 6,       // Light grey: CAM.5 Camera frustum schematic.
    CameraFrustumSelected = 7, // Selection orange: frustum of a selected Camera.
    CameraFrustumDisabled = 8, // Dim grey: frustum of a disabled Camera, selected or not.
    Light = 9,                 // Pale yellow: LIGHT.1 Light wireframe.
    LightSelected = 10,        // Selection orange: wireframe of a selected Light.
    LightDisabled = 11         // Dim olive grey: wireframe of a disabled Light, selected or not.
};

// A Capsule needs one value beyond its matrix. The matrix scales uniformly by the radius,
// so the wireframe is generated in radius units: hemisphere centers sit at
// (0, -/+ halfCylinderHeightInRadii, 0), where halfCylinderHeightInRadii = (h / 2) / r.
struct CapsuleColliderOverlay
{
    Matrix4 colliderWorldTransform;
    float halfCylinderHeightInRadii = 0.0f;
};

// One Character Controller diagnostic wireframe: its capsule, or a marker drawn with the
// existing unit sphere (points) or a thin unit box (normals).
struct ColliderOverlayMarker
{
    Matrix4 colliderWorldTransform;
    ColliderOverlayShape shape = ColliderOverlayShape::Box;
    ColliderOverlayRole role = ColliderOverlayRole::CharacterController;
    float halfCylinderHeightInRadii = 0.0f;
};

// Engine owns this capture on the frame's stack. Renderer consumes it synchronously
// during Render and retains neither the matrices nor the borrowed scene identity.
struct EditorColliderOverlay
{
    // All six solid/trigger shape batches share one bound.
    static constexpr std::size_t MaxColliders = 16384; // Two optional colliders per 8192 scene objects.

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

    // A unit cone along +Z: a closed ring of radius 1 at z = 1, plus four radial lines from the
    // apex at the origin to the ring at +X, +Y, -X and -Y.
    static constexpr unsigned int ConeRingSegments = 64;
    static constexpr unsigned int ConeRadialLineCount = 4;
    static constexpr unsigned int VerticesPerCone = (ConeRingSegments + ConeRadialLineCount) * 2;

    // Five parallel arrows along +Z from z = 0 to z = 1 (center and four offsets), each a shaft
    // plus four head lines.
    static constexpr unsigned int ArrowCount = 5;
    static constexpr unsigned int LinesPerArrow = 5;
    static constexpr unsigned int VerticesPerArrows = ArrowCount * LinesPerArrow * 2;

    const Scene* scene = nullptr; // Identity comparison only; not an owning pointer.
    std::uint64_t structureRevision = 0;
    std::uint64_t rendererLifetime = 0;
    std::vector<Matrix4> boxColliderWorldTransforms;
    std::vector<Matrix4> sphereColliderWorldTransforms;
    std::vector<CapsuleColliderOverlay> capsuleColliders;
    std::vector<Matrix4> triggerBoxWorldTransforms;
    std::vector<Matrix4> triggerSphereWorldTransforms;
    std::vector<CapsuleColliderOverlay> triggerCapsules;
    // Controller capsules and diagnostic markers, bounded per controller.
    std::vector<ColliderOverlayMarker> characterMarkers;
    // CAM.5 Camera frusta: unit boxes under projective frustum models (Box shape only).
    std::vector<ColliderOverlayMarker> cameraFrusta;
    // LIGHT.1 Light wireframes (Sphere, Cone or Arrows), selected Lights last.
    std::vector<ColliderOverlayMarker> lightWireframes;
    // Capsule, ground point and normal, and a point and normal per copied contact.
    static constexpr std::size_t MaxMarkersPerCharacter = 3 + 2 * Physics::MaximumCharacterDiagnosticContacts;

    // Exact for valid batches; saturates at MaxColliders + 1 for safe capacity rejection.
    std::size_t GetColliderCount() const noexcept;
    bool Matches(const Scene& currentScene, std::uint64_t currentRendererLifetime) const noexcept;
};

// Unit-shape endpoints in the exact draw order the shader generates. Throw
// out_of_range for a vertex index outside the shape's draw count. The capsule
// half-height is in radius units and is ignored by Box and Sphere.
Vector3 ColliderBoxVertex(unsigned int vertexIndex);
Vector3 ColliderSphereVertex(unsigned int vertexIndex);
Vector3 ColliderCapsuleVertex(unsigned int vertexIndex, float halfCylinderHeightInRadii);
Vector3 ColliderConeVertex(unsigned int vertexIndex);
Vector3 ColliderArrowsVertex(unsigned int vertexIndex);
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

// Trigger geometry uses its own validation and the same placement/wireframe conventions.
// presentationActive is true only for a trigger sharing a presented PhysicsComponent body.
bool BuildColliderBoxMatrix(const Physics::TriggerColliderComponent& component, const RenderObject& object,
    bool presentationActive, Matrix4& colliderWorldTransform);
bool BuildColliderSphereMatrix(const Physics::TriggerColliderComponent& component, const RenderObject& object,
    bool presentationActive, Matrix4& colliderWorldTransform);
bool BuildColliderCapsule(const Physics::TriggerColliderComponent& component, const RenderObject& object,
    bool presentationActive, CapsuleColliderOverlay& capsuleCollider);

// Character Controller capsule: authored radius, cylinder height and local offset in the
// object's yaw-only frame (accepted presentation pose in Play), without visual scale.
bool BuildCharacterControllerCapsule(const Physics::CharacterControllerComponent& component, const RenderObject& object,
    bool presentationActive, CapsuleColliderOverlay& capsuleCollider);

// Ground support and contact markers (at most MaxMarkersPerCharacter - 1) from a diagnostics
// snapshot whose positions are already world space. Debug drawing only.
void AppendCharacterDiagnosticMarkers(const Physics::CharacterControllerDiagnostics& diagnostics,
    std::vector<ColliderOverlayMarker>& markers);
