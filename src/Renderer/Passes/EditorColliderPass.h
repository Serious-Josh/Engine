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
    // capsule half-cylinder height in radius units (zero for Box and Sphere), and visual role.
    static constexpr unsigned int TransformConstantCount = 16;
    static constexpr unsigned int ShapeConstantCount = 1;
    static constexpr unsigned int CapsuleConstantCount = 1;
    static constexpr unsigned int RoleConstantCount = 1;
    static constexpr unsigned int ColliderConstantCount =
        TransformConstantCount + ShapeConstantCount + CapsuleConstantCount + RoleConstantCount;

    bool Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary);
    void Shutdown();
    void Execute(ID3D12GraphicsCommandList* commandList, RenderTarget& viewportColor, DepthBuffer& viewportDepth,
        const Matrix4& viewProjection, const EditorColliderOverlay& overlay) const;

private:
    Pipeline m_pipeline; // Renderer releases this only after its existing GPU-confirmed teardown.
};
