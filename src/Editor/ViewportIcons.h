#pragma once

#include "EditorIcons.h"
#include "Math/Matrix4.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <span>

namespace ViewportIcons
{
    inline float ScreenSize(const Vector3& camera, const Vector3& anchor, float fontSize)
    {
        if(!std::isfinite(camera.x) || !std::isfinite(camera.y) || !std::isfinite(camera.z) ||
            !std::isfinite(anchor.x) || !std::isfinite(anchor.y) || !std::isfinite(anchor.z) ||
            !std::isfinite(fontSize) || fontSize <= 0)
        {
            return 0;
        }

        // Engine distances are metres. Match the former 2-font size at 8m, with
        // 1.5/5-font bounds. The clamped inverse is continuous.
        const double distance = std::hypot(static_cast<double>(camera.x) - anchor.x,
            static_cast<double>(camera.y) - anchor.y, static_cast<double>(camera.z) - anchor.z);
        const double scale = (std::clamp)(16.0 / (std::max)(distance, 0.001), 1.5, 5.0);
        return fontSize * static_cast<float>(scale);
    }

    struct Icon
    {
        size_t objectIndex = 0;
        EditorIcon artwork = EditorIcon::Light;
        ImVec2 minimum;
        ImVec2 maximum;
        double depthBucket = 0;
        bool pickable = false;
    };

    inline std::optional<Vector3> WorldAnchor(const Matrix4& model)
    {
        for(const auto& row : model.m)
        {
            for(float value : row)
            {
                if(!std::isfinite(value))
                {
                    return std::nullopt;
                }
            }
        }

        return Vector3{model.m[0][3], model.m[1][3], model.m[2][3]};
    }

    inline bool Project(const Matrix4& viewProjection, const Vector3& anchor,
        ImVec2 viewportOrigin, ImVec2 viewportSize, ImVec2 iconSize, Icon& icon)
    {
        if(!std::isfinite(anchor.x) || !std::isfinite(anchor.y) || !std::isfinite(anchor.z) ||
            !std::isfinite(viewportSize.x) || !std::isfinite(viewportSize.y) ||
            !std::isfinite(viewportOrigin.x) || !std::isfinite(viewportOrigin.y) ||
            !std::isfinite(iconSize.x) || !std::isfinite(iconSize.y) ||
            viewportSize.x <= 0 || viewportSize.y <= 0 || iconSize.x <= 0 || iconSize.y <= 0)
        {
            return false;
        }

        float clip[4]{};

        for(size_t row = 0; row < 4; ++row)
        {
            clip[row] = viewProjection.m[row][0] * anchor.x + viewProjection.m[row][1] * anchor.y +
                viewProjection.m[row][2] * anchor.z + viewProjection.m[row][3];

            if(!std::isfinite(clip[row]))
            {
                return false;
            }
        }

        // Engine uses column vectors, forward-positive perspective, and D3D depth [0,1].
        if(clip[3] <= 0.000001f || clip[2] < 0 || clip[2] > clip[3] ||
            std::abs(clip[0]) > clip[3] || std::abs(clip[1]) > clip[3])
        {
            return false;
        }

        const ImVec2 center(viewportOrigin.x + (clip[0] / clip[3] + 1.0f) * 0.5f * viewportSize.x,
            viewportOrigin.y + (1.0f - clip[1] / clip[3]) * 0.5f * viewportSize.y);
        icon.minimum = ImVec2(center.x - iconSize.x * 0.5f, center.y - iconSize.y * 0.5f);
        icon.maximum = ImVec2(center.x + iconSize.x * 0.5f, center.y + iconSize.y * 0.5f);

        if(!std::isfinite(icon.minimum.x) || !std::isfinite(icon.minimum.y) ||
            !std::isfinite(icon.maximum.x) || !std::isfinite(icon.maximum.y))
        {
            return false;
        }

        // Quantize perspective distance to 1mm: near-coplanar icons do not jitter between
        // targets due to floating-point noise. Runtime slot is a stable tie-breaker per scene.
        icon.depthBucket = std::round(static_cast<double>(clip[3]) * 1000.0);
        icon.pickable = icon.minimum.x >= viewportOrigin.x && icon.minimum.y >= viewportOrigin.y &&
            icon.maximum.x <= viewportOrigin.x + viewportSize.x &&
            icon.maximum.y <= viewportOrigin.y + viewportSize.y;
        return true;
    }

    // Far-to-near painter order; lower runtime slot wins equal-depth overlaps.
    inline bool Behind(const Icon& first, const Icon& second)
    {
        if(first.depthBucket != second.depthBucket)
        {
            return first.depthBucket > second.depthBucket;
        }

        return first.objectIndex > second.objectIndex;
    }

    inline std::optional<size_t> Pick(std::span<const Icon> sortedIcons, ImVec2 mouse)
    {
        if(!std::isfinite(mouse.x) || !std::isfinite(mouse.y))
        {
            return std::nullopt;
        }

        for(auto it = sortedIcons.rbegin(); it != sortedIcons.rend(); ++it)
        {
            if(it->pickable && mouse.x >= it->minimum.x && mouse.y >= it->minimum.y &&
                mouse.x < it->maximum.x && mouse.y < it->maximum.y)
            {
                return it->objectIndex;
            }
        }

        return std::nullopt;
    }
}
