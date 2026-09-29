#pragma once

#include <cstddef>

struct TriangleVertex
{
    float position[3];
    float normal[3];
    float uv[2];
};

const TriangleVertex TriangleVertices[] =
{
    { {  0.0f,  0.5f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 0.5f, 0.0f } },
    { {  0.5f, -0.5f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 1.0f } },
    { { -0.5f, -0.5f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f } }
};

inline VertexLayout CreateTriangleVertexLayout()
{
    VertexLayout layout;

    layout.Add("POSITION", 0, VertexAttributeType::Float3, offsetof(TriangleVertex, position));
    layout.Add("NORMAL", 0, VertexAttributeType::Float3, offsetof(TriangleVertex, normal));
    layout.Add("TEXCOORD", 0, VertexAttributeType::Float2, offsetof(TriangleVertex, uv));

    return layout;
}