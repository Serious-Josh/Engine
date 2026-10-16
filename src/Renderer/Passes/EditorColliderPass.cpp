#include "EditorColliderPass.h"

#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/DepthBuffer.h"
#include "Renderer/RenderTarget.h"

#include <cmath>

namespace
{
bool HasFiniteClipCoordinates(const Matrix4& viewProjection, std::span<const Matrix4> colliderWorldTransforms)
{
    for(const Matrix4& colliderWorldTransform : colliderWorldTransforms)
    {
        const Matrix4 clipFromBox = viewProjection * colliderWorldTransform;
        for(const auto& matrixRow : clipFromBox.m)
        {
            for(float matrixElement : matrixRow)
            {
                if(!std::isfinite(matrixElement))
                {
                    return false;
                }
            }
        }

        for(unsigned int vertexIndex = 0; vertexIndex < EditorColliderOverlay::VerticesPerBox; ++vertexIndex)
        {
            const Vector3 localPosition = ColliderBoxVertex(vertexIndex);
            for(unsigned int coordinate = 0; coordinate < 4; ++coordinate)
            {
                const float clipCoordinate = clipFromBox[coordinate][0] * localPosition.x +
                    clipFromBox[coordinate][1] * localPosition.y +
                    clipFromBox[coordinate][2] * localPosition.z + clipFromBox[coordinate][3];
                if(!std::isfinite(clipCoordinate))
                {
                    return false;
                }
            }
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
        {RootParameterType::Constants, 0, TransformConstantCount, D3D12_SHADER_VISIBILITY_VERTEX});
    return m_pipeline.Initialize(device, pipelineDescription);
}

void EditorColliderPass::Shutdown()
{
    m_pipeline.Shutdown();
}

void EditorColliderPass::Execute(ID3D12GraphicsCommandList* commandList, RenderTarget& viewportColor,
    DepthBuffer& viewportDepth, const Matrix4& viewProjection, std::span<const Matrix4> colliderWorldTransforms) const
{
    if(colliderWorldTransforms.empty() || colliderWorldTransforms.size() > EditorColliderOverlay::MaxBoxes)
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

    // Validate the whole batch before changing state or recording any partial visualization.
    if(!HasFiniteClipCoordinates(viewProjection, colliderWorldTransforms))
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

    // Root constants are copied into the command stream per draw. No frame can
    // overwrite another frame's upload storage, and changing counts needs no retirement.
    static_assert(sizeof(Matrix4) == TransformConstantCount * sizeof(float));
    for(const Matrix4& colliderWorldTransform : colliderWorldTransforms)
    {
        const Matrix4 clipFromBox = viewProjection * colliderWorldTransform;
        commandList->SetGraphicsRoot32BitConstants(0, TransformConstantCount, &clipFromBox, 0);
        commandList->DrawInstanced(EditorColliderOverlay::VerticesPerBox, 1, 0, 0);
    }

    // Unbind the read-only DSV before restoring the tracked state for later consumers.
    commandList->OMSetRenderTargets(1, &colorTarget, FALSE, nullptr);
    viewportDepth.Transition(commandList, previousDepthState);
}
