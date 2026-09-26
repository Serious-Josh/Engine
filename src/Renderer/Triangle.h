#pragma once

struct TriangleVertex
{
    float x;
    float y;
    float z;
};

inline constexpr TriangleVertex TriangleVertices[] =
{
    { 0.0f,  0.5f, 0.0f },
    { 0.5f, -0.5f, 0.0f },
    {-0.5f, -0.5f, 0.0f}
};