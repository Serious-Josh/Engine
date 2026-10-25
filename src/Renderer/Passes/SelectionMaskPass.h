#pragma once

#include "Renderer/Pipeline.h"

class ShaderLibrary;
class VertexLayout;

class SelectionMaskPass
{
    public:
    bool IsCompatible(const VertexLayout& layout) const noexcept;

    bool Initialize(
      ID3D12Device* device,
      ShaderLibrary& shaderLibrary,
      const VertexLayout& vertexLayout);
    void Bind(ID3D12GraphicsCommandList* commandList, bool mirroredWinding = false) const;
    // UPSCALING.3C.4: the same shaders and bindings with depth written (LESS) for the Editor overlay depth prepass; its
    // mask output is cleared by the caller afterwards.
    void BindDepthPrepass(ID3D12GraphicsCommandList* commandList, bool mirroredWinding = false) const;
    void Shutdown();
    bool IsInitialized() const noexcept { return m_pipeline.IsInitialized(); }

    private:
    friend class Renderer;
    // Only for an empty active pass, after final validation before recording.
    void AdoptPrepared(SelectionMaskPass& candidate) noexcept;

    struct LayoutAttribute
    {
        std::string semantic;
        unsigned int index;
        VertexAttributeType type;
        unsigned int offset;
    };

    std::vector<LayoutAttribute> m_layout;
    unsigned int m_stride = 0;
    Pipeline m_pipeline;
    Pipeline m_depthPrepassPipeline;
};
