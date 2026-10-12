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

static_assert(sizeof(Vertex) == 48);

static_assert(offsetof(Vertex, position) == 0);
static_assert(offsetof(Vertex, normal) == 12);
static_assert(offsetof(Vertex, uv) == 24);
static_assert(offsetof(Vertex, tangent) == 32);

inline VertexLayout CreateVertexLayout()
{
    VertexLayout layout;

    layout.Add("POSITION", 0, VertexAttributeType::Float3, offsetof(Vertex, position));
    layout.Add("NORMAL", 0, VertexAttributeType::Float3, offsetof(Vertex, normal));
    layout.Add("TEXCOORD", 0, VertexAttributeType::Float2, offsetof(Vertex, uv));
    layout.Add("TANGENT", 0, VertexAttributeType::Float4, offsetof(Vertex, tangent));

    return layout;
}
