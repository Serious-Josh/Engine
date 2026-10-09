#pragma once

#include "Renderer/Vertex.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace TangentGenerator
{
    struct Vec3
    {
        float x, y, z;
    };

    inline Vec3 Sub(Vec3 a, Vec3 b)
    {
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    }

    inline Vec3 Add(Vec3 a, Vec3 b)
    {
        return {a.x + b.x, a.y + b.y, a.z + b.z};
    }

    inline Vec3 Mul(Vec3 v, float s)
    {
        return {v.x * s, v.y * s, v.z * s};
    }

    inline float Dot(Vec3 a, Vec3 b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    inline Vec3 Cross(Vec3 a, Vec3 b)
    {
        return {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        };
    }

    inline bool Valid(Vec3 v)
    {
        return std::isfinite(v.x) &&
               std::isfinite(v.y) &&
               std::isfinite(v.z);
    }

    inline Vec3 NormalizeOr(Vec3 v, Vec3 fallback)
    {
        const float lengthSquared = Dot(v, v);

        if (!Valid(v) || !std::isfinite(lengthSquared) || lengthSquared < 1e-20f)
        {
            return fallback;
        }

        return Mul(v, 1.0f / std::sqrt(lengthSquared));
    }

    inline Vec3 FallbackTangent(Vec3 normal)
    {
        const Vec3 n = NormalizeOr(normal, {0.0f, 0.0f, 1.0f});
        const Vec3 axis = std::abs(n.z) < 0.9f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};

        return NormalizeOr(Cross(axis, n), {1.0f, 0.0f, 0.0f});
    }

    inline Vec3 Position(const Vertex& v)
    {
        return {v.position[0], v.position[1], v.position[2]};
    }

    inline Vec3 Normal(const Vertex& v)
    {
        return {v.normal[0], v.normal[1], v.normal[2]};
    }

    inline void GenerateTangents(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices)
    {
        if (vertices.empty() || indices.empty() ||
            indices.size() % 3 != 0)
        {
            return;
        }

        constexpr float uvEpsilon = 1e-12f;

        const std::size_t originalVertexCount = vertices.size();

        // A duplicated vertex is created only when a triangle
        // requires the opposite tangent-space handedness.
        std::vector<std::int8_t> assignedSign(originalVertexCount, 0);
        std::vector<std::uint32_t> oppositeVertex(originalVertexCount, UINT32_MAX);
        std::vector<Vec3> tangentSum(originalVertexCount, {0.0f, 0.0f, 0.0f});
        std::vector<Vec3> bitangentSum(originalVertexCount, {0.0f, 0.0f, 0.0f});

        for (std::size_t i = 0; i < indices.size(); i += 3)
        {
            const std::uint32_t i0 = indices[i];
            const std::uint32_t i1 = indices[i + 1];
            const std::uint32_t i2 = indices[i + 2];

            if (i0 >= originalVertexCount ||
                i1 >= originalVertexCount ||
                i2 >= originalVertexCount)
            {
                continue;
            }

            const Vertex& v0 = vertices[i0];
            const Vertex& v1 = vertices[i1];
            const Vertex& v2 = vertices[i2];

            const Vec3 e1 = Sub(Position(v1), Position(v0));
            const Vec3 e2 = Sub(Position(v2), Position(v0));

            const float du1 = v1.uv[0] - v0.uv[0];
            const float dv1 = v1.uv[1] - v0.uv[1];
            const float du2 = v2.uv[0] - v0.uv[0];
            const float dv2 = v2.uv[1] - v0.uv[1];

            const float determinant = du1 * dv2 - du2 * dv1;

            if (!std::isfinite(determinant) || std::abs(determinant) < uvEpsilon)
            {
                continue;
            }

            const float inverse = 1.0f / determinant;
            const Vec3 tangent = Mul(Sub(Mul(e1, dv2), Mul(e2, dv1)), inverse);
            const Vec3 bitangent = Mul(Sub(Mul(e2, du1), Mul(e1, du2)), inverse);

            if (!Valid(tangent) || !Valid(bitangent))
            {
                continue;
            }

            const Vec3 faceNormal = NormalizeOr(Cross(e1, e2), {0.0f, 0.0f, 1.0f});

            const float orientation = Dot(Cross(faceNormal, tangent), bitangent);
            const std::int8_t sign = orientation < 0.0f ? -1 : 1;
            const std::uint32_t original[3] = {i0, i1, i2};

            for (int corner = 0; corner < 3; ++corner)
            {
                const std::uint32_t source = original[corner];
                std::uint32_t destination = source;

                if (assignedSign[source] == 0)
                {
                    assignedSign[source] = sign;
                }
                else if (assignedSign[source] != sign)
                {
                    if (oppositeVertex[source] == UINT32_MAX)
                    {
                        destination = static_cast<std::uint32_t>(vertices.size());

                        oppositeVertex[source] = destination;
                        vertices.push_back(vertices[source]);

                        tangentSum.push_back({0.0f, 0.0f, 0.0f});
                        bitangentSum.push_back({0.0f, 0.0f, 0.0f});
                    }
                    else
                    {
                        destination = oppositeVertex[source];
                    }
                }

                indices[i + corner] = destination;

                tangentSum[destination] = Add(tangentSum[destination], tangent);

                bitangentSum[destination] = Add(bitangentSum[destination], bitangent);
            }
        }

        for (std::size_t i = 0; i < vertices.size(); ++i)
        {
            Vertex& vertex = vertices[i];

            const Vec3 n = NormalizeOr(Normal(vertex), {0.0f, 0.0f, 1.0f});

            // Gram-Schmidt orthogonalization.
            Vec3 t = Sub(tangentSum[i], Mul(n, Dot(n, tangentSum[i])));

            t = NormalizeOr(t, FallbackTangent(n));

            const float orientation = Dot(Cross(n, t), bitangentSum[i]);
            const float handedness = orientation < 0.0f ? -1.0f : 1.0f;

            vertex.tangent[0] = t.x;
            vertex.tangent[1] = t.y;
            vertex.tangent[2] = t.z;
            vertex.tangent[3] = handedness;
        }
    }
}