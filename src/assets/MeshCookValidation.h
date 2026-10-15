#pragma once

#include "assets/ImportedMesh.h"

#include <bit>

// Development-only comparison shared by offline cooking and CPU verification.
namespace MeshCookValidation
{
    inline bool Same(float a, float b)
    {
        return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
    }

    inline bool Same(const Vector3& a, const Vector3& b)
    {
        return Same(a.x, b.x) && Same(a.y, b.y) && Same(a.z, b.z);
    }

    inline bool ExactlyEqual(const ImportedMesh& a, const ImportedMesh& b)
    {
        if(a.vertices.size() != b.vertices.size() || a.indices != b.indices || a.metadata != b.metadata ||
            !Same(a.boundsMin, b.boundsMin) || !Same(a.boundsMax, b.boundsMax))
        {
            return false;
        }

        for(size_t i = 0; i < a.vertices.size(); ++i)
        {
            const auto& x = a.vertices[i];
            const auto& y = b.vertices[i];

            for(size_t j = 0; j < 3; ++j)
            {
                if(!Same(x.position[j], y.position[j]) || !Same(x.normal[j], y.normal[j]))
                {
                    return false;
                }
            }

            for(size_t j = 0; j < 2; ++j)
            {
                if(!Same(x.uv[j], y.uv[j]))
                {
                    return false;
                }
            }

            for(size_t j = 0; j < 4; ++j)
            {
                if(!Same(x.tangent[j], y.tangent[j]))
                {
                    return false;
                }
            }
        }

        return true;
    }
}
