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
    static constexpr unsigned int TransformConstantCount = 16;

    bool Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary);
    void Shutdown();
    void Execute(ID3D12GraphicsCommandList* commandList, RenderTarget& viewportColor, DepthBuffer& viewportDepth,
        const Matrix4& viewProjection, std::span<const Matrix4> colliderWorldTransforms) const;

private:
    Pipeline m_pipeline; // Renderer releases this only after its existing GPU-confirmed teardown.
};
