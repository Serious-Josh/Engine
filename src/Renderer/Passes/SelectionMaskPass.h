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
    void Bind(ID3D12GraphicsCommandList* commandList) const;
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
};
