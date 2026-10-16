#include "EditorColliderOverlay.h"

#include "Scene/Scene.h"

#include <cmath>
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

bool HasFiniteWorldCoordinates(const Matrix4& colliderWorldTransform)
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
    for(unsigned int vertexIndex = 0; vertexIndex < EditorColliderOverlay::VerticesPerBox; ++vertexIndex)
    {
        const Vector3 localPosition = ColliderBoxVertex(vertexIndex);
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
}

bool EditorColliderOverlay::Matches(const Scene& currentScene, std::uint64_t currentRendererLifetime) const noexcept
{
    const bool sameScene = scene == &currentScene;
    const bool sameStructure = structureRevision == currentScene.GetStructureRevision();
    const bool sameRenderer = rendererLifetime == currentRendererLifetime;
    const bool withinCapacity = colliderWorldTransforms.size() <= MaxBoxes;
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

bool BuildColliderBoxMatrix(const Physics::PhysicsComponent& component, const RenderObject& object,
    bool presentationActive, Matrix4& colliderWorldTransform)
{
    if(!Physics::ValidatePhysicsComponent(component))
    {
        return false;
    }

    try
    {
        Matrix4 bodyRigidTransform;
        if(!ResolveColliderRigidTransform(object, presentationActive, bodyRigidTransform))
        {
            return false;
        }

        const Vector3 colliderDimensions{
            component.boxDimensions.width, component.boxDimensions.height, component.boxDimensions.depth};
        const Matrix4 colliderLocalOffset = Matrix4::Translation(component.colliderLocalPositionOffset);
        const Matrix4 colliderDimensionScale = Matrix4::Scale(colliderDimensions);

        // Box3D's dimensions and local offset are explicit meters, independent of visual scale.
        const Matrix4 candidateTransform = bodyRigidTransform * colliderLocalOffset * colliderDimensionScale;
        if(!HasFiniteWorldCoordinates(candidateTransform))
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
