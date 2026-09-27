#pragma once

#include <cstddef>

struct TriangleVertex
{
    float position[3];
    float uv[2];
};

const TriangleVertex TriangleVertices[] =
{
    { {  0.0f,  0.5f, 0.0f }, { 0.5f, 0.0f } },
    { {  0.5f, -0.5f, 0.0f }, { 1.0f, 1.0f } },
    { { -0.5f, -0.5f, 0.0f }, { 0.0f, 1.0f } }
};

inline VertexLayout CreateTriangleVertexLayout()
{
    VertexLayout layout;

    layout.Add("POSITION", 0, VertexAttributeType::Float3, offsetof(TriangleVertex, position));
    layout.Add("TEXCOORD", 0, VertexAttributeType::Float2, sizeof(float) * 3);

    return layout;
}