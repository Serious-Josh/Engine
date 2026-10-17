#pragma once

#include "Renderer/Pipeline.h"
#include "Renderer/EditorColliderOverlay.h"

#include <span>

class ShaderLibrary;
class DepthBuffer;
class RenderTarget;

class EditorColliderPass
{
public:
    // Root constants at b0: the clip-from-collider matrix, the shape selector, and the
    // capsule half-cylinder height in radius units (zero for Box and Sphere).
    static constexpr unsigned int TransformConstantCount = 16;
    static constexpr unsigned int ShapeConstantCount = 1;
    static constexpr unsigned int CapsuleConstantCount = 1;
    static constexpr unsigned int ColliderConstantCount = TransformConstantCount + ShapeConstantCount + CapsuleConstantCount;

    bool Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary);
    void Shutdown();
    void Execute(ID3D12GraphicsCommandList* commandList, RenderTarget& viewportColor, DepthBuffer& viewportDepth,
        const Matrix4& viewProjection, std::span<const Matrix4> boxColliderWorldTransforms,
        std::span<const Matrix4> sphereColliderWorldTransforms,
        std::span<const CapsuleColliderOverlay> capsuleColliders) const;

private:
    Pipeline m_pipeline; // Renderer releases this only after its existing GPU-confirmed teardown.
};
