#pragma once

#include "Renderer/Pipeline.h"

class ShaderLibrary;
class VertexLayout;

class SelectionMaskPass
{
    public:
    bool Initialize(
      ID3D12Device* device,
      ShaderLibrary& shaderLibrary,
      const VertexLayout& vertexLayout);
    void Bind(ID3D12GraphicsCommandList* commandList) const;
    void Shutdown();

    private:
    Pipeline m_pipeline;
};
