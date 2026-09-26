#include "VertexLayout.h"

void VertexLayout::Add(const char* semantic, unsigned int semanticIndex, VertexAttributeType type, unsigned int offset)
{
    m_attributes.push_back({semantic, semanticIndex, type, offset});

    switch(type){
        case VertexAttributeType::Float:
            m_stride = offset + sizeof(float);
            break;
        case VertexAttributeType::Float2:
            m_stride = offset + sizeof(float) * 2;
            break;
        case VertexAttributeType::Float3:
            m_stride = offset + sizeof(float) * 3;
            break;
        case VertexAttributeType::Float4:
            m_stride = offset + sizeof(float) * 4;
            break;
    }
}

const std::vector<VertexAttribute>& VertexLayout::GetAttributes() const
{
    return m_attributes;
}

unsigned int VertexLayout::GetStride() const
{
    return m_stride;
}

DXGI_FORMAT VertexLayout::GetDXGIFormat(VertexAttributeType type)
{
    switch(type){
        case VertexAttributeType::Float:
            return DXGI_FORMAT_R32_FLOAT;
        case VertexAttributeType::Float2:
            return DXGI_FORMAT_R32G32_FLOAT;
        case VertexAttributeType::Float3:
            return DXGI_FORMAT_R32G32B32_FLOAT;
        case VertexAttributeType::Float4:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
    }

    return DXGI_FORMAT_UNKNOWN;
}