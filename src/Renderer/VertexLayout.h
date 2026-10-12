#pragma once

#include <d3d12.h>

#include <cstddef>
#include <string>
#include <vector>

enum class VertexAttributeType
{
    Float,
    Float2,
    Float3,
    Float4
};

struct VertexAttribute
{
    const char* semantic;
    unsigned int semanticIndex = 0;
    VertexAttributeType type;
    unsigned int offset;
};

class VertexLayout
{
    public:
    void Add(
      const char* semantic,
      unsigned int semanticIndex,
      VertexAttributeType type,
      unsigned int offset);
    const std::vector<VertexAttribute>& GetAttributes() const;

    unsigned int GetStride() const;

    static DXGI_FORMAT GetDXGIFormat(VertexAttributeType type);

    private:
    std::vector<VertexAttribute> m_attributes;
    unsigned int m_stride = 0;
};
