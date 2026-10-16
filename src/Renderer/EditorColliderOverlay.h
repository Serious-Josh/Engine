#pragma once

#include "Math/Matrix4.h"
#include "Physics/PhysicsComponent.h"
#include "Renderer/RenderObject.h"

#include <cstdint>
#include <vector>

class Scene;

// Engine owns this capture on the frame's stack. Renderer consumes it synchronously
// during Render and retains neither the matrices nor the borrowed scene identity.
struct EditorColliderOverlay
{
    static constexpr std::size_t MaxBoxes = 8192;
    static constexpr unsigned int VerticesPerBox = 24;

    const Scene* scene = nullptr; // Identity comparison only; not an owning pointer.
    std::uint64_t structureRevision = 0;
    std::uint64_t rendererLifetime = 0;
    std::vector<Matrix4> colliderWorldTransforms;

    bool Matches(const Scene& currentScene, std::uint64_t currentRendererLifetime) const noexcept;
};

Vector3 ColliderBoxVertex(unsigned int vertexIndex);
bool BuildColliderBoxMatrix(const Physics::PhysicsComponent& component, const RenderObject& object,
    bool presentationActive, Matrix4& colliderWorldTransform);
