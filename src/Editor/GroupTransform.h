#pragma once

#include "Scene/Transform.h"
#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <vector>

namespace GroupTransform
{
    enum class Mode { Translate, Rotate, Scale };

    inline Vector3 Direction(const Matrix4& matrix, const Vector3& value)
    {
        return {matrix.m[0][0] * value.x + matrix.m[0][1] * value.y + matrix.m[0][2] * value.z,
            matrix.m[1][0] * value.x + matrix.m[1][1] * value.y + matrix.m[1][2] * value.z,
            matrix.m[2][0] * value.x + matrix.m[2][1] * value.y + matrix.m[2][2] * value.z};
    }

    inline Vector3 Euler(const Matrix4& rotation)
    {
        const float y = std::asin((std::clamp)(-rotation.m[2][0], -1.0f, 1.0f));
        if(std::abs(std::cos(y)) > 0.00001f)
        {
            return {std::atan2(rotation.m[2][1], rotation.m[2][2]), y,
                std::atan2(rotation.m[1][0], rotation.m[0][0])};
        }
        return {std::atan2(-rotation.m[1][2], rotation.m[1][1]), y, 0.0f};
    }

    // All results derive from drag-start TRS values. Reject shear before publishing any result.
    inline bool Calculate(Mode mode, const Transform& anchorBefore, const Transform& anchorAfter,
        std::span<const Transform> before, std::vector<Transform>& after, std::string& error)
    {
        error.clear();
        after.assign(before.begin(), before.end());
        Matrix4 delta = Matrix4::Identity();
        float uniformRatio = 0.0f;
        if(mode == Mode::Rotate)
        {
            delta = Matrix4::Rotation(anchorAfter.rotation) * Matrix4::Rotation(anchorBefore.rotation).Transpose();
        }
        if(mode == Mode::Scale)
        {
            if(anchorBefore.scale.x <= 0.0f || anchorBefore.scale.y <= 0.0f || anchorBefore.scale.z <= 0.0f)
            {
                error = "Group scaling requires positive anchor scale.";
                return false;
            }
            const Vector3 ratio{anchorAfter.scale.x / anchorBefore.scale.x,
                anchorAfter.scale.y / anchorBefore.scale.y, anchorAfter.scale.z / anchorBefore.scale.z};
            if(!std::isfinite(ratio.x) || !std::isfinite(ratio.y) || !std::isfinite(ratio.z) ||
                ratio.x <= 0.0f || ratio.y <= 0.0f || ratio.z <= 0.0f)
            {
                error = "Group scaling requires finite positive scale ratios.";
                return false;
            }
            const auto basis = Matrix4::Rotation(anchorBefore.rotation);
            delta = basis * Matrix4::Scale(ratio) * basis.Transpose();
            if(std::abs(ratio.x - ratio.y) < 0.000001f && std::abs(ratio.x - ratio.z) < 0.000001f)
            {
                uniformRatio = ratio.x;
                delta = Matrix4::Scale(uniformRatio, uniformRatio, uniformRatio);
            }
        }

        if(mode != Mode::Translate)
        {
            bool unchanged = true;
            for(size_t row = 0; row < 3; ++row)
                for(size_t column = 0; column < 3; ++column)
                    unchanged = unchanged && std::abs(delta.m[row][column] - (row == column ? 1.0f : 0.0f)) < 0.000001f;
            if(unchanged) return true;
        }

        for(size_t index = 0; index < before.size(); ++index)
        {
            const auto& start = before[index];
            auto& result = after[index];
            if(mode == Mode::Translate)
            {
                result.position = start.position + (anchorAfter.position - anchorBefore.position);
                continue;
            }
            result.position = anchorBefore.position + Direction(delta, start.position - anchorBefore.position);
            const auto orientation = delta * Matrix4::Rotation(start.rotation);
            if(mode == Mode::Rotate)
            {
                result.rotation = Euler(orientation);
                continue;
            }

            if(uniformRatio > 0.0f)
            {
                result.scale = start.scale * uniformRatio;
                if(result.scale.x < 0.01f || result.scale.y < 0.01f || result.scale.z < 0.01f)
                {
                    error = "Group scaling cannot reduce any object scale below 0.01.";
                    return false;
                }
                continue;
            }

            Vector3 axes[3];
            float lengths[3];
            for(size_t axis = 0; axis < 3; ++axis)
            {
                axes[axis] = {orientation.m[0][axis], orientation.m[1][axis], orientation.m[2][axis]};
                lengths[axis] = axes[axis].Length();
                if(!std::isfinite(lengths[axis]) || lengths[axis] <= 0.000001f)
                {
                    error = "Group scale is degenerate.";
                    return false;
                }
                axes[axis] = axes[axis] * (1.0f / lengths[axis]);
            }
            if(std::abs(Vector3::Dot(axes[0], axes[1])) > 0.00001f ||
                std::abs(Vector3::Dot(axes[0], axes[2])) > 0.00001f ||
                std::abs(Vector3::Dot(axes[1], axes[2])) > 0.00001f)
            {
                error = "This nonuniform group scale would introduce shear. Use uniform scaling or edit objects individually.";
                return false;
            }
            result.scale = {start.scale.x * lengths[0], start.scale.y * lengths[1], start.scale.z * lengths[2]};
            if(result.scale.x < 0.01f || result.scale.y < 0.01f || result.scale.z < 0.01f)
            {
                error = "Group scaling cannot reduce any object scale below 0.01.";
                return false;
            }
            Matrix4 rotation = Matrix4::Identity();
            for(size_t axis = 0; axis < 3; ++axis)
            {
                rotation.m[0][axis] = axes[axis].x;
                rotation.m[1][axis] = axes[axis].y;
                rotation.m[2][axis] = axes[axis].z;
            }
            result.rotation = Euler(rotation);
        }
        return true;
    }
}
