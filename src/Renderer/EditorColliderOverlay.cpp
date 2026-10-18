#include "EditorColliderOverlay.h"

#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <numbers>
#include <stdexcept>

namespace
{
bool ResolveColliderRigidTransform(const RenderObject& object, bool presentationActive, Matrix4& rigidTransform)
{
    const auto physicsPresentationPose = object.GetPhysicsPresentationPose();

    if(presentationActive)
    {
        // Missing presentation must suppress the collider, never fall back to an authored pose.
        if(!physicsPresentationPose)
        {
            return false;
        }

        const Vector3 unitScale{1.0f, 1.0f, 1.0f};
        rigidTransform = Physics::ComposeRuntimeModelMatrix(*physicsPresentationPose, unitScale);
        return true;
    }

    if(physicsPresentationPose)
    {
        return false;
    }

    const Matrix4 authoredTranslation = Matrix4::Translation(object.transform.position);
    const Matrix4 authoredRotation = Matrix4::Rotation(object.transform.rotation);
    rigidTransform = authoredTranslation * authoredRotation;

    return true;
}

bool HasFiniteWorldCoordinates(const Matrix4& colliderWorldTransform, ColliderOverlayShape shape,
    float halfCylinderHeightInRadii)
{
    for(const auto& matrixRow : colliderWorldTransform.m)
    {
        for(float matrixElement : matrixRow)
        {
            if(!std::isfinite(matrixElement))
            {
                return false;
            }
        }
    }

    // Finite matrix elements alone do not guarantee finite transformed endpoints.
    const unsigned int vertexCount = ColliderVertexCount(shape);

    for(unsigned int vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
    {
        const Vector3 localPosition = ColliderShapeVertex(shape, vertexIndex, halfCylinderHeightInRadii);

        for(unsigned int coordinate = 0; coordinate < 3; ++coordinate)
        {
            const float worldCoordinate = colliderWorldTransform[coordinate][0] * localPosition.x +
                colliderWorldTransform[coordinate][1] * localPosition.y +
                colliderWorldTransform[coordinate][2] * localPosition.z + colliderWorldTransform[coordinate][3];

            if(!std::isfinite(worldCoordinate))
            {
                return false;
            }
        }
    }

    return true;
}

// Composes bodyRigidTransform * Translation(localOffset) * shapeRotation * Scale(shapeScale).
// The offset is applied once, in body-local meters, so it rotates with the body but not
// with shapeRotation (identity except for Capsule). Authored visual scale is never part of
// this transform; shapeRotation and shapeScale come only from collider data.
bool BuildColliderMatrix(Vector3 localPositionOffset, const RenderObject& object,
    bool presentationActive, const Matrix4& shapeRotation, Vector3 shapeScale, ColliderOverlayShape shape,
    float halfCylinderHeightInRadii, Matrix4& colliderWorldTransform)
{
    try
    {
        Matrix4 bodyRigidTransform;

        if(!ResolveColliderRigidTransform(object, presentationActive, bodyRigidTransform))
        {
            return false;
        }

        const Matrix4 colliderLocalOffset = Matrix4::Translation(localPositionOffset);
        const Matrix4 colliderShapeScale = Matrix4::Scale(shapeScale);
        const Matrix4 candidateTransform = bodyRigidTransform * colliderLocalOffset * shapeRotation * colliderShapeScale;

        if(!HasFiniteWorldCoordinates(candidateTransform, shape, halfCylinderHeightInRadii))
        {
            return false;
        }

        colliderWorldTransform = candidateTransform;
        return true;
    }
    catch(const std::exception&)
    {
        return false;
    }
}

template <typename Component>
bool BuildCapsulePlacement(const Component& component, const RenderObject& object,
    bool presentationActive, CapsuleColliderOverlay& capsuleCollider)
{
    // The matrix scales the radius-1 wireframe uniformly by the authored radius, so the
    // caps stay hemispherical whatever the visual scale. The cylinder half-height is
    // expressed in radius units, matching Box3D's centers at offset + (0, -/+ h/2, 0).
    const float radius = component.capsuleRadius;
    const float halfCylinderHeightInRadii = component.capsuleCylinderHeight * 0.5f / radius;

    if(!std::isfinite(halfCylinderHeightInRadii))
    {
        return false;
    }

    // The capsule local rotation uses the Transform convention (Rz * Ry * Rx, radians),
    // matching the quaternion ScenePhysicsSession gives Box3D.
    const Matrix4 capsuleLocalRotation = Matrix4::Rotation(component.capsuleLocalRotation);
    const Vector3 uniformRadiusScale{radius, radius, radius};
    Matrix4 colliderWorldTransform;

    const bool placementValid = BuildColliderMatrix(component.colliderLocalPositionOffset, object, presentationActive,
        capsuleLocalRotation, uniformRadiusScale, ColliderOverlayShape::Capsule, halfCylinderHeightInRadii, colliderWorldTransform);

    if(!placementValid)
    {
        return false;
    }

    capsuleCollider.colliderWorldTransform = colliderWorldTransform;
    capsuleCollider.halfCylinderHeightInRadii = halfCylinderHeightInRadii;

    return true;
}

// Unit-radius point on a circle in the local XZ plane at the given ring point index.
// Rings, longitudinal lines and arc endpoints all use this so shared points coincide.
Vector3 CapsuleRingPoint(unsigned int pointIndex, float height)
{
    constexpr unsigned int segmentCount = EditorColliderOverlay::CapsuleRingSegments;
    const float angle = static_cast<float>(pointIndex % segmentCount) * (2.0f * std::numbers::pi_v<float> / segmentCount);

    return {std::cos(angle), height, std::sin(angle)};
}
}

std::size_t EditorColliderOverlay::GetColliderCount() const noexcept
{
    const std::array counts{boxColliderWorldTransforms.size(), sphereColliderWorldTransforms.size(), capsuleColliders.size(),
        triggerBoxWorldTransforms.size(), triggerSphereWorldTransforms.size(), triggerCapsules.size(),
        characterMarkers.size()};
    std::size_t count = 0;

    for(const auto size : counts)
    {
        if(size > MaxColliders - count)
        {
            return MaxColliders + 1;
        }

        count += size;
    }

    return count;
}

bool EditorColliderOverlay::Matches(const Scene& currentScene, std::uint64_t currentRendererLifetime) const noexcept
{
    const bool sameScene = scene == &currentScene;
    const bool sameStructure = structureRevision == currentScene.GetStructureRevision();
    const bool sameRenderer = rendererLifetime == currentRendererLifetime;
    const bool withinCapacity = GetColliderCount() <= MaxColliders;

    return sameScene && sameStructure && sameRenderer && withinCapacity;
}

Vector3 ColliderBoxVertex(unsigned int vertexIndex)
{
    if(vertexIndex >= EditorColliderOverlay::VerticesPerBox)
    {
        throw std::out_of_range("Collider vertex ID");
    }

    // Each axis has four parallel edges, with two endpoints per edge. The other
    // two axes select one of four fixed sides. Keep this order identical to HLSL.
    constexpr unsigned int endpointsPerEdge = 2;
    constexpr unsigned int edgesPerAxis = 4;
    constexpr unsigned int axisCount = 3;
    const unsigned int edgeIndex = vertexIndex / endpointsPerEdge;
    const unsigned int edgeAxis = edgeIndex / edgesPerAxis;
    const unsigned int sideIndex = edgeIndex % edgesPerAxis;
    const unsigned int firstSideAxis = (edgeAxis + 1) % axisCount;
    const unsigned int secondSideAxis = (edgeAxis + 2) % axisCount;

    float coordinates[axisCount]{};
    coordinates[edgeAxis] = static_cast<float>(vertexIndex % endpointsPerEdge) - 0.5f;
    coordinates[firstSideAxis] = static_cast<float>(sideIndex % 2) - 0.5f;
    coordinates[secondSideAxis] = static_cast<float>(sideIndex / 2) - 0.5f;

    return {coordinates[0], coordinates[1], coordinates[2]};
}

Vector3 ColliderSphereVertex(unsigned int vertexIndex)
{
    if(vertexIndex >= EditorColliderOverlay::VerticesPerSphere)
    {
        throw std::out_of_range("Collider sphere vertex ID");
    }

    // Each circle is a run of line segments with two endpoints per segment. The second
    // endpoint of the last segment wraps to point zero, so every circle closes exactly.
    // Keep this order and arithmetic identical to SphereCircleEndpoint in HLSL.
    constexpr unsigned int endpointsPerSegment = 2;
    constexpr unsigned int segmentCount = EditorColliderOverlay::SphereSegmentsPerCircle;
    constexpr unsigned int verticesPerCircle = segmentCount * endpointsPerSegment;
    const unsigned int circleIndex = vertexIndex / verticesPerCircle;
    const unsigned int circleVertex = vertexIndex % verticesPerCircle;
    const unsigned int segmentIndex = circleVertex / endpointsPerSegment;
    const unsigned int pointIndex = (segmentIndex + circleVertex % endpointsPerSegment) % segmentCount;

    const float angle = static_cast<float>(pointIndex) * (2.0f * std::numbers::pi_v<float> / segmentCount);
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);

    // Circle 0 lies in the local XY plane, circle 1 in XZ and circle 2 in YZ.
    if(circleIndex == 0)
    {
        return {cosine, sine, 0.0f};
    }
    else if(circleIndex == 1)
    {
        return {cosine, 0.0f, sine};
    }
    else
    {
        return {0.0f, cosine, sine};
    }
}

Vector3 ColliderCapsuleVertex(unsigned int vertexIndex, float halfCylinderHeightInRadii)
{
    if(vertexIndex >= EditorColliderOverlay::VerticesPerCapsule)
    {
        throw std::out_of_range("Collider capsule vertex ID");
    }

    // Radius-1 capsule along +Y with hemisphere centers at y = -/+ halfHeight. Lines are
    // laid out as: rings (lower then upper), longitudinal lines, then hemisphere arcs.
    // Keep this order and arithmetic identical to CapsuleEndpoint in HLSL.
    constexpr unsigned int ringSegments = EditorColliderOverlay::CapsuleRingSegments;
    constexpr unsigned int arcSegments = EditorColliderOverlay::CapsuleArcSegments;
    constexpr unsigned int ringLineCount = EditorColliderOverlay::CapsuleRingLineCount;
    constexpr unsigned int longitudinalLineCount = EditorColliderOverlay::CapsuleLongitudinalLineCount;
    constexpr unsigned int quarterTurnPoints = ringSegments / 4;
    const unsigned int lineIndex = vertexIndex / 2;
    const unsigned int endpoint = vertexIndex % 2;
    const float halfHeight = halfCylinderHeightInRadii;

    // Rings: closed 64-segment circles in the XZ plane at each hemisphere center.
    if(lineIndex < ringLineCount)
    {
        const unsigned int ringIndex = lineIndex / ringSegments;
        const unsigned int segmentIndex = lineIndex % ringSegments;
        const float ringHeight = ringIndex == 0 ? -halfHeight : halfHeight;

        return CapsuleRingPoint(segmentIndex + endpoint, ringHeight);
    }

    // Longitudinal lines: join the rings at +X, +Z, -X and -Z (every quarter turn).
    const unsigned int longitudinalIndex = lineIndex - ringLineCount;

    if(longitudinalIndex < longitudinalLineCount)
    {
        const float lineHeight = endpoint == 0 ? -halfHeight : halfHeight;

        return CapsuleRingPoint(longitudinalIndex * quarterTurnPoints, lineHeight);
    }

    // Hemisphere arcs: per hemisphere, a half-circle in the XY plane then one in the ZY
    // plane, each from the ring at +X (or +Z) over the pole to -X (or -Z).
    const unsigned int arcLineIndex = longitudinalIndex - longitudinalLineCount;
    const unsigned int hemisphereIndex = arcLineIndex / (2 * arcSegments);
    const unsigned int arcInHemisphere = (arcLineIndex / arcSegments) % 2;
    const unsigned int pointIndex = arcLineIndex % arcSegments + endpoint;

    const float theta = static_cast<float>(pointIndex) * (std::numbers::pi_v<float> / arcSegments);
    const float horizontal = std::cos(theta);
    const float poleward = std::sin(theta);
    const float height = hemisphereIndex == 0 ? -halfHeight - poleward : halfHeight + poleward;

    if(arcInHemisphere == 0)
    {
        return {horizontal, height, 0.0f};
    }
    else
    {
        return {0.0f, height, horizontal};
    }
}

unsigned int ColliderVertexCount(ColliderOverlayShape shape)
{
    switch(shape)
    {
    case ColliderOverlayShape::Box:
        return EditorColliderOverlay::VerticesPerBox;

    case ColliderOverlayShape::Sphere:
        return EditorColliderOverlay::VerticesPerSphere;

    case ColliderOverlayShape::Capsule:
        return EditorColliderOverlay::VerticesPerCapsule;

    default:
        throw std::invalid_argument("Unsupported collider overlay shape");
    }
}

Vector3 ColliderShapeVertex(ColliderOverlayShape shape, unsigned int vertexIndex, float halfCylinderHeightInRadii)
{
    switch(shape)
    {
    case ColliderOverlayShape::Box:
        return ColliderBoxVertex(vertexIndex);

    case ColliderOverlayShape::Sphere:
        return ColliderSphereVertex(vertexIndex);

    case ColliderOverlayShape::Capsule:
        return ColliderCapsuleVertex(vertexIndex, halfCylinderHeightInRadii);

    default:
        throw std::invalid_argument("Unsupported collider overlay shape");
    }
}

bool BuildColliderBoxMatrix(const Physics::PhysicsComponent& component, const RenderObject& object,
    bool presentationActive, Matrix4& colliderWorldTransform)
{
    // Never display a Sphere or Capsule as a Box.
    if(component.colliderType != Physics::ColliderType::Box || !Physics::ValidatePhysicsComponent(component))
    {
        return false;
    }

    // Unit coordinates in [-0.5, +0.5] times full dimensions give half-extents exactly once.
    const Vector3 colliderDimensions{
        component.boxDimensions.width,
        component.boxDimensions.height,
        component.boxDimensions.depth
    };

    return BuildColliderMatrix(component.colliderLocalPositionOffset, object, presentationActive,
        Matrix4::Identity(), colliderDimensions, ColliderOverlayShape::Box, 0.0f, colliderWorldTransform);
}

bool BuildColliderSphereMatrix(const Physics::PhysicsComponent& component, const RenderObject& object,
    bool presentationActive, Matrix4& colliderWorldTransform)
{
    // Never display a Box or Capsule as a Sphere.
    if(component.colliderType != Physics::ColliderType::Sphere || !Physics::ValidatePhysicsComponent(component))
    {
        return false;
    }

    // Unit circles times the authored radius in meters. The scale is uniform, so the
    // wireframe stays spherical whatever the object's visual scale.
    const float radius = component.sphereRadius;
    const Vector3 uniformRadiusScale{radius, radius, radius};

    return BuildColliderMatrix(component.colliderLocalPositionOffset, object, presentationActive,
        Matrix4::Identity(), uniformRadiusScale, ColliderOverlayShape::Sphere, 0.0f, colliderWorldTransform);
}

bool BuildColliderCapsule(const Physics::PhysicsComponent& component, const RenderObject& object,
    bool presentationActive, CapsuleColliderOverlay& capsuleCollider)
{
    // Never display a Box or Sphere as a Capsule.
    if(component.colliderType != Physics::ColliderType::Capsule || !Physics::ValidatePhysicsComponent(component))
    {
        return false;
    }

    return BuildCapsulePlacement(component, object, presentationActive, capsuleCollider);
}

bool BuildColliderBoxMatrix(const Physics::TriggerColliderComponent& component, const RenderObject& object,
    bool presentationActive, Matrix4& colliderWorldTransform)
{
    if(component.colliderType != Physics::ColliderType::Box || !Physics::ValidateTriggerColliderComponent(component))
    {
        return false;
    }

    const Vector3 dimensions{component.boxDimensions.width, component.boxDimensions.height, component.boxDimensions.depth};
    return BuildColliderMatrix(component.colliderLocalPositionOffset, object, presentationActive, Matrix4::Identity(),
        dimensions, ColliderOverlayShape::Box, 0.0f, colliderWorldTransform);
}

bool BuildColliderSphereMatrix(const Physics::TriggerColliderComponent& component, const RenderObject& object,
    bool presentationActive, Matrix4& colliderWorldTransform)
{
    if(component.colliderType != Physics::ColliderType::Sphere || !Physics::ValidateTriggerColliderComponent(component))
    {
        return false;
    }

    const float radius = component.sphereRadius;
    return BuildColliderMatrix(component.colliderLocalPositionOffset, object, presentationActive, Matrix4::Identity(),
        {radius, radius, radius}, ColliderOverlayShape::Sphere, 0.0f, colliderWorldTransform);
}

bool BuildColliderCapsule(const Physics::TriggerColliderComponent& component, const RenderObject& object,
    bool presentationActive, CapsuleColliderOverlay& capsuleCollider)
{
    if(component.colliderType != Physics::ColliderType::Capsule || !Physics::ValidateTriggerColliderComponent(component))
    {
        return false;
    }

    return BuildCapsulePlacement(component, object, presentationActive, capsuleCollider);
}

bool BuildCharacterControllerCapsule(const Physics::CharacterControllerComponent& component, const RenderObject& object,
    bool presentationActive, CapsuleColliderOverlay& capsuleCollider)
{
    if(!Physics::ValidateCharacterControllerComponent(component))
    {
        return false;
    }

    // Same upright capsule, offset and frame as the runtime observer (no local rotation).
    Physics::TriggerColliderComponent capsule;
    capsule.colliderType = Physics::ColliderType::Capsule;
    capsule.capsuleRadius = component.capsuleRadius;
    capsule.capsuleCylinderHeight = component.capsuleCylinderHeight;
    capsule.colliderLocalPositionOffset = component.localOffset;
    return BuildCapsulePlacement(capsule, object, presentationActive, capsuleCollider);
}

namespace
{
// Debug marker sizes in meters (display only).
constexpr float GroundPointRadius = 0.05f;
constexpr float GroundNormalLength = 0.4f;
constexpr float ContactPointRadius = 0.03f;
constexpr float ContactNormalLength = 0.25f;
constexpr float NormalBarWidth = 0.01f;

ColliderOverlayMarker MakePointMarker(const Vector3& point, float radius, ColliderOverlayRole role)
{
    const Matrix4 transform = Matrix4::Translation(point) * Matrix4::Scale(Vector3{radius, radius, radius});
    return {transform, ColliderOverlayShape::Sphere, role};
}

// A thin unit box whose long axis runs from point along the unit normal.
ColliderOverlayMarker MakeNormalMarker(const Vector3& point, const Vector3& normal, float length, ColliderOverlayRole role)
{
    const Vector3 helper = std::abs(normal.y) < 0.9f ? Vector3{0.0f, 1.0f, 0.0f} : Vector3{1.0f, 0.0f, 0.0f};
    Vector3 tangent = Vector3::Cross(helper, normal);
    tangent = tangent * (1.0f / std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z));
    const Vector3 bitangent = Vector3::Cross(normal, tangent);
    const Vector3 center = point + normal * (length * 0.5f);

    // Column-vector basis: columns are the scaled tangent, normal and bitangent.
    Matrix4 transform = Matrix4::Identity();
    const Vector3 columns[] = {tangent * NormalBarWidth, normal * length, bitangent * NormalBarWidth};

    for(int column = 0; column < 3; ++column)
    {
        transform.m[0][column] = columns[column].x;
        transform.m[1][column] = columns[column].y;
        transform.m[2][column] = columns[column].z;
    }

    transform.m[0][3] = center.x;
    transform.m[1][3] = center.y;
    transform.m[2][3] = center.z;
    return {transform, ColliderOverlayShape::Box, role};
}
}

void AppendCharacterDiagnosticMarkers(const Physics::CharacterControllerDiagnostics& diagnostics,
    std::vector<ColliderOverlayMarker>& markers)
{
    if(diagnostics.ground.grounded)
    {
        markers.push_back(MakePointMarker(diagnostics.ground.point, GroundPointRadius, ColliderOverlayRole::GroundSupport));
        markers.push_back(MakeNormalMarker(diagnostics.ground.point, diagnostics.ground.normal, GroundNormalLength,
            ColliderOverlayRole::GroundSupport));
    }

    const std::size_t contactCount = (std::min)(diagnostics.contacts.size(), Physics::MaximumCharacterDiagnosticContacts);

    for(std::size_t index = 0; index < contactCount; ++index)
    {
        const auto& contact = diagnostics.contacts[index];
        markers.push_back(MakePointMarker(contact.point, ContactPointRadius, ColliderOverlayRole::CharacterContact));
        markers.push_back(MakeNormalMarker(contact.point, contact.normal, ContactNormalLength,
            ColliderOverlayRole::CharacterContact));
    }
}
