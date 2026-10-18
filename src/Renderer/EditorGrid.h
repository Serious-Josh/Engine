#pragma once

#include "Math/Matrix4.h"
#include "Math/Vector3.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

// Editor-only adaptive viewport grid on the world XZ plane at Y = 0 (G.3).
//
// The base spacing is the G.1 translation snap interval. Displayed spacings are integer
// multiples of it from the nested hierarchy 1, 5, 10, 50, 100, ... (alternating x5, x2), so every
// coarser lattice is a subset of every finer one and of the snap lattice. EditorGrid.hlsl
// evaluates the same functions per pixel; keep the constants and formulas in sync.
namespace EditorGrid
{
    // Lines spaced closer than kFadeStartPixels are invisible; they reach full minor opacity at
    // kFadeEndPixels. A level brightens towards major opacity as its spacing grows from
    // kMajorStartPixels to kMajorEndPixels. Every alpha is a continuous function of on-screen
    // spacing, so density changes fade instead of popping.
    inline constexpr float kFadeStartPixels = 4.0f;
    inline constexpr float kFadeEndPixels = 12.0f;
    inline constexpr float kMajorStartPixels = 40.0f;
    inline constexpr float kMajorEndPixels = 200.0f;
    inline constexpr float kMinorAlpha = 0.24f;
    inline constexpr float kMajorAlpha = 0.42f;
    inline constexpr float kAxisAlpha = 0.65f;

    // Coarsest level index drawn (multiple 10^6). The lattice period below is a multiple of every
    // drawable level, so reducing the camera position by it never shifts a displayed line.
    inline constexpr int kMaxLevelIndex = 12;
    inline constexpr double kLatticePeriodMultiple = 1.0e6;

    // Levels evaluated per pixel, starting at the finest level at most FadeStartPixels apart.
    // That level can be as close as FadeStartPixels / 5; six further x5/x2 steps always reach
    // a level at or beyond kMajorEndPixels, so advancing the window never adds an unsaturated
    // coarser level (which would pop).
    inline constexpr int kLevelsPerPixel = 7;

    // Integer multiple of the base spacing for a hierarchy level: 1, 5, 10, 50, 100, ...
    inline double LevelMultiple(int levelIndex)
    {
        const double decade = std::pow(10.0, levelIndex / 2);
        return (levelIndex % 2 == 0) ? decade : 5.0 * decade;
    }

    // Largest level whose multiple does not exceed requiredMultiple (>= 1), clamped to the
    // drawable range.
    inline int FinestLevelAtMost(double requiredMultiple)
    {
        if(!std::isfinite(requiredMultiple) || requiredMultiple <= 1.0)
        {
            return 0;
        }

        const double decade = std::floor(std::log10(requiredMultiple));
        const double mantissa = requiredMultiple / std::pow(10.0, decade);
        const int levelIndex = static_cast<int>(decade) * 2 + (mantissa >= 5.0 ? 1 : 0);
        return std::clamp(levelIndex, 0, kMaxLevelIndex);
    }

    inline float SmoothStep(float edge0, float edge1, float value)
    {
        const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    // Opacity of one level's lines given their on-screen spacing in pixels.
    inline float LevelAlpha(float pixelSpacing)
    {
        const float visibility = SmoothStep(kFadeStartPixels, kFadeEndPixels, pixelSpacing);
        const float emphasis = SmoothStep(kMajorStartPixels, kMajorEndPixels, pixelSpacing);
        return visibility * (kMinorAlpha + (kMajorAlpha - kMinorAlpha) * emphasis);
    }

    // Camera coordinate reduced by a whole lattice period so the shader's lattice coordinates stay
    // small (float-precise) at large world positions. Done in double on the CPU.
    inline float LatticeOffset(double cameraCoordinate, double baseSpacing)
    {
        const double period = baseSpacing * kLatticePeriodMultiple;
        return static_cast<float>(cameraCoordinate - std::round(cameraCoordinate / period) * period);
    }

    // Root-constant image of the HLSL GridConstants cbuffer. Matrices use Engine's row-major
    // storage with column vectors. "Camera-relative" space is world space translated so the camera
    // is at the origin, which keeps reconstruction precise far from the world origin.
    struct Constants
    {
        Matrix4 clipFromCameraRelative;
        Matrix4 cameraRelativeFromClip;
        float cameraPosition[3] = {};
        float baseSpacing = 0.0f;
        float latticeOffset[2] = {};
        float padding[2] = {};
    };

    inline constexpr unsigned int kConstantCount = 40;
    static_assert(sizeof(Constants) == kConstantCount * sizeof(float));

    inline bool IsFiniteMatrix(const Matrix4& matrix)
    {
        for(const auto& row : matrix.m)
        {
            for(const float element : row)
            {
                if(!std::isfinite(element))
                {
                    return false;
                }
            }
        }

        return true;
    }

    // Builds the per-frame constants. Returns false (draw nothing) for an invalid spacing, a
    // non-finite camera or a singular projection.
    inline bool BuildConstants(const Matrix4& view, const Matrix4& projection, const Vector3& cameraPosition,
        float baseSpacing, Constants& constants)
    {
        const bool validSpacing = std::isfinite(baseSpacing) && baseSpacing > 0.0f;
        const bool finiteCamera = std::isfinite(cameraPosition.x) && std::isfinite(cameraPosition.y) &&
            std::isfinite(cameraPosition.z);

        if(!validSpacing || !finiteCamera)
        {
            return false;
        }

        const Matrix4 cameraRelativeView = view * Matrix4::Translation(cameraPosition);
        constants.clipFromCameraRelative = projection * cameraRelativeView;
        constants.cameraRelativeFromClip = constants.clipFromCameraRelative.Inverse();

        if(!IsFiniteMatrix(constants.clipFromCameraRelative) || !IsFiniteMatrix(constants.cameraRelativeFromClip))
        {
            return false;
        }

        constants.cameraPosition[0] = cameraPosition.x;
        constants.cameraPosition[1] = cameraPosition.y;
        constants.cameraPosition[2] = cameraPosition.z;
        constants.baseSpacing = baseSpacing;
        constants.latticeOffset[0] = LatticeOffset(cameraPosition.x, baseSpacing);
        constants.latticeOffset[1] = LatticeOffset(cameraPosition.z, baseSpacing);
        return true;
    }
}

// Per-frame Editor request; absent means no grid (Play mode, Overlays off, runtime).
struct EditorGridRequest
{
    float baseSpacing = 0.25f;
};
