#pragma once

#include "Renderer/VertexLayout.h"

#include <cstddef>

struct Vertex
{
    float position[3];
    float normal[3];
    float uv[2];
    float tangent[4] = {1.0f, 0.0f, 0.0f, 1.0f};
};

inline VertexLayout CreateVertexLayout()
{
    VertexLayout layout;

    layout.Add("POSITION", 0, VertexAttributeType::Float3, offsetof(Vertex, position));
    layout.Add("NORMAL", 0, VertexAttributeType::Float3, offsetof(Vertex, normal));
    layout.Add("TEXCOORD", 0, VertexAttributeType::Float2, offsetof(Vertex, uv));
    layout.Add("TANGENT", 0, VertexAttributeType::Float4, offsetof(Vertex, tangent));

    return layout;
}