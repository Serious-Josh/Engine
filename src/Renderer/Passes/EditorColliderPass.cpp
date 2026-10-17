#include "EditorColliderPass.h"

#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/DepthBuffer.h"
#include "Renderer/RenderTarget.h"

#include <cmath>
#include <cstdint>

namespace
{
// CPU image of the HLSL ColliderConstants cbuffer, copied as root constants per draw.
struct ColliderDrawConstants
{
    Matrix4 clipFromCollider;
    std::uint32_t colliderShape = 0;
    float capsuleHalfCylinderHeight = 0.0f;
};

static_assert(sizeof(ColliderDrawConstants) == EditorColliderPass::ColliderConstantCount * sizeof(float));

// One collider to validate or draw: its world matrix plus the capsule parameter (zero
// for Box and Sphere).
struct ColliderDraw
{
    const Matrix4& colliderWorldTransform;
    float capsuleHalfCylinderHeight;
};

bool HasFiniteClipCoordinates(const Matrix4& viewProjection, const ColliderDraw& collider, ColliderOverlayShape shape)
{
    const Matrix4 clipFromCollider = viewProjection * collider.colliderWorldTransform;

    for(const auto& matrixRow : clipFromCollider.m)
    {
        for(float matrixElement : matrixRow)
        {
            if(!std::isfinite(matrixElement))
            {
                return false;
            }
        }
    }

    const unsigned int vertexCount = ColliderVertexCount(shape);

    for(unsigned int vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
    {
        const Vector3 localPosition = ColliderShapeVertex(shape, vertexIndex, collider.capsuleHalfCylinderHeight);

        for(unsigned int coordinate = 0; coordinate < 4; ++coordinate)
        {
            const float clipCoordinate = clipFromCollider[coordinate][0] * localPosition.x +
                clipFromCollider[coordinate][1] * localPosition.y +
                clipFromCollider[coordinate][2] * localPosition.z + clipFromCollider[coordinate][3];

            if(!std::isfinite(clipCoordinate))
            {
                return false;
            }
        }
    }

    return true;
}

void DrawCollider(ID3D12GraphicsCommandList* commandList, const Matrix4& viewProjection, const ColliderDraw& collider,
    ColliderOverlayShape shape)
{
    // The shape constant and vertex count are chosen together here, so one shape can
    // never generate another shape's endpoints.
    ColliderDrawConstants drawConstants;
    drawConstants.clipFromCollider = viewProjection * collider.colliderWorldTransform;
    drawConstants.colliderShape = static_cast<std::uint32_t>(shape);
    drawConstants.capsuleHalfCylinderHeight = collider.capsuleHalfCylinderHeight;

    // Root constants are copied into the command stream per draw. No frame can
    // overwrite another frame's upload storage, and changing counts needs no retirement.
    commandList->SetGraphicsRoot32BitConstants(0, EditorColliderPass::ColliderConstantCount, &drawConstants, 0);
    commandList->DrawInstanced(ColliderVertexCount(shape), 1, 0, 0);
}

// Applies one operation to every collider of all three shapes, in the fixed draw order
// Box, Sphere, Capsule. Returns false as soon as the operation does.
template <typename Operation>
bool ForEachCollider(std::span<const Matrix4> boxColliderWorldTransforms,
    std::span<const Matrix4> sphereColliderWorldTransforms, std::span<const CapsuleColliderOverlay> capsuleColliders,
    Operation operation)
{
    for(const Matrix4& colliderWorldTransform : boxColliderWorldTransforms)
    {
        if(!operation(ColliderDraw{colliderWorldTransform, 0.0f}, ColliderOverlayShape::Box))
        {
            return false;
        }
    }

    for(const Matrix4& colliderWorldTransform : sphereColliderWorldTransforms)
    {
        if(!operation(ColliderDraw{colliderWorldTransform, 0.0f}, ColliderOverlayShape::Sphere))
        {
            return false;
        }
    }

    for(const CapsuleColliderOverlay& capsuleCollider : capsuleColliders)
    {
        const ColliderDraw collider{capsuleCollider.colliderWorldTransform, capsuleCollider.halfCylinderHeightInRadii};

        if(!operation(collider, ColliderOverlayShape::Capsule))
        {
            return false;
        }
    }

    return true;
}
}

bool EditorColliderPass::Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary)
{
    const Shader* vertexShader = shaderLibrary.Load({"EditorCollider.hlsl", "VSMain", ShaderStage::Vertex});
    const Shader* pixelShader = shaderLibrary.Load({"EditorCollider.hlsl", "PSMain", ShaderStage::Pixel});

    if(!vertexShader || !pixelShader)
    {
        return false;
    }

    PipelineDescription pipelineDescription;
    pipelineDescription.vertexShader = vertexShader;
    pipelineDescription.pixelShader = pixelShader;
    pipelineDescription.renderTargets.count = 1;
    pipelineDescription.renderTargets.formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipelineDescription.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;

    // Read the viewport depth for occlusion without contributing collider geometry to scene depth.
    pipelineDescription.depthEnabled = true;
    pipelineDescription.depthFormat = DXGI_FORMAT_D32_FLOAT;
    pipelineDescription.depthComparison = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pipelineDescription.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pipelineDescription.rootParameters.push_back(
        {RootParameterType::Constants, 0, ColliderConstantCount, D3D12_SHADER_VISIBILITY_VERTEX});

    return m_pipeline.Initialize(device, pipelineDescription);
}

void EditorColliderPass::Shutdown()
{
    m_pipeline.Shutdown();
}

void EditorColliderPass::Execute(ID3D12GraphicsCommandList* commandList, RenderTarget& viewportColor,
    DepthBuffer& viewportDepth, const Matrix4& viewProjection, std::span<const Matrix4> boxColliderWorldTransforms,
    std::span<const Matrix4> sphereColliderWorldTransforms,
    std::span<const CapsuleColliderOverlay> capsuleColliders) const
{
    // All shapes share one capacity bound; an empty batch records no GPU work.
    const std::size_t colliderCount =
        boxColliderWorldTransforms.size() + sphereColliderWorldTransforms.size() + capsuleColliders.size();

    if(colliderCount == 0 || colliderCount > EditorColliderOverlay::MaxColliders)
    {
        return;
    }

    if(!m_pipeline.IsInitialized())
    {
        return;
    }

    const bool matchingTargetDimensions = viewportColor.GetWidth() == viewportDepth.GetWidth() &&
        viewportColor.GetHeight() == viewportDepth.GetHeight();

    if(!matchingTargetDimensions)
    {
        return;
    }

    // Validate the whole batch, every shape, before changing state or recording any
    // partial visualization.
    const bool finiteBatch = ForEachCollider(boxColliderWorldTransforms, sphereColliderWorldTransforms, capsuleColliders,
        [&](const ColliderDraw& collider, ColliderOverlayShape shape)
        {
            return HasFiniteClipCoordinates(viewProjection, collider, shape);
        });

    if(!finiteBatch)
    {
        return;
    }

    const D3D12_RESOURCE_STATES previousDepthState = viewportDepth.GetState();
    viewportDepth.Transition(commandList, D3D12_RESOURCE_STATE_DEPTH_READ);

    const D3D12_CPU_DESCRIPTOR_HANDLE colorTarget = viewportColor.GetRTV();
    const D3D12_CPU_DESCRIPTOR_HANDLE readOnlyDepth = viewportDepth.GetReadOnlyDSV();
    commandList->OMSetRenderTargets(1, &colorTarget, FALSE, &readOnlyDepth);
    m_pipeline.Bind(commandList);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);

    ForEachCollider(boxColliderWorldTransforms, sphereColliderWorldTransforms, capsuleColliders,
        [&](const ColliderDraw& collider, ColliderOverlayShape shape)
        {
            DrawCollider(commandList, viewProjection, collider, shape);
            return true;
        });

    // Unbind the read-only DSV before restoring the tracked state for later consumers.
    commandList->OMSetRenderTargets(1, &colorTarget, FALSE, nullptr);
    viewportDepth.Transition(commandList, previousDepthState);
}
