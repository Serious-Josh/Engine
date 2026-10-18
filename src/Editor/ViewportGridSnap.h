#pragma once

#include "Math/Matrix4.h"
#include "Math/Vector3.h"
#include <cmath>
#include <numbers>

// Absolute snapping for the viewport gizmo. Translation snaps to a grid anchored at the world
// origin; rotation snaps the twist angle around the dragged world axis. Both are absolute:
// snapped values are integer multiples of the interval, never offsets from the drag start.
namespace ViewportGridSnap
{
    inline constexpr float kDefaultInterval = 0.25f;
    inline constexpr float kDefaultRotationIntervalDegrees = 15.0f;

    // Fraction of one interval treated as "on the line". It absorbs float error in positions,
    // angles and gizmo output without being large enough to be felt while dragging.
    inline constexpr double kBoundaryTolerance = 1.0e-4;

    inline bool IsValidInterval(float interval)
    {
        return std::isfinite(interval) && interval > 0.0f;
    }

    // Returns the last grid line (a multiple of interval) that continuousValue has crossed while
    // moving away from dragStart, or dragStart while no line beyond it has been reached. An
    // off-grid start therefore never jumps backward to the nearest line, and an on-grid start
    // stays aligned. Callers recompute from the drag start every frame, so reversals are stable
    // and no error accumulates.
    inline double SnapValue(double dragStart, double continuousValue, double interval)
    {
        const double step = interval;
        const double tolerance = step * kBoundaryTolerance;
        const double displacement = continuousValue - dragStart;

        if(std::abs(displacement) <= tolerance)
        {
            return dragStart;
        }

        const double continuousSteps = continuousValue / step;

        if(displacement > 0.0)
        {
            const double crossedLine = std::floor(continuousSteps + kBoundaryTolerance) * step;

            if(crossedLine <= dragStart + tolerance)
            {
                return dragStart;
            }

            return crossedLine;
        }

        const double crossedLine = std::ceil(continuousSteps - kBoundaryTolerance) * step;

        if(crossedLine >= dragStart - tolerance)
        {
            return dragStart;
        }

        return crossedLine;
    }

    // Snaps one world-axis coordinate during a translation drag.
    inline float SnapCoordinate(float dragStart, float continuousCoordinate, float interval)
    {
        if(!IsValidInterval(interval) || !std::isfinite(dragStart) || !std::isfinite(continuousCoordinate))
        {
            return continuousCoordinate;
        }

        const double snapped = SnapValue(dragStart, continuousCoordinate, interval);

        if(snapped == static_cast<double>(dragStart))
        {
            return dragStart;
        }

        return static_cast<float>(snapped);
    }

    // Snaps each world axis independently, so single-axis, plane and screen-space handles all
    // use the same rule. Axes the gizmo did not move keep their drag-start value.
    inline Vector3 SnapTranslation(const Vector3& dragStart, const Vector3& continuousPosition, float interval)
    {
        return {
            SnapCoordinate(dragStart.x, continuousPosition.x, interval),
            SnapCoordinate(dragStart.y, continuousPosition.y, interval),
            SnapCoordinate(dragStart.z, continuousPosition.z, interval)};
    }

    // ---- Rotation ----------------------------------------------------------------------------
    //
    // Matrices here use Engine's column-vector convention (Matrix4::Rotation, Transform), not
    // ImGuizmo's transposed layout. Rotation matrices are the upper 3x3 of a Matrix4.

    struct Quaternion
    {
        double w = 1.0;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    // Rotation part of a TRS model matrix: each basis column divided by its scale. Callers only
    // pass shear-free models (authored TRS or ImGuizmo world rotations of them).
    inline Matrix4 RotationFromModel(const Matrix4& model)
    {
        Matrix4 rotation = Matrix4::Identity();

        for(int column = 0; column < 3; ++column)
        {
            const double length = std::sqrt(
                static_cast<double>(model.m[0][column]) * model.m[0][column] +
                static_cast<double>(model.m[1][column]) * model.m[1][column] +
                static_cast<double>(model.m[2][column]) * model.m[2][column]);

            if(!std::isfinite(length) || length <= 1.0e-12)
            {
                return Matrix4::Identity();
            }

            for(int row = 0; row < 3; ++row)
            {
                rotation.m[row][column] = static_cast<float>(model.m[row][column] / length);
            }
        }

        return rotation;
    }

    // Shepperd's method; the result has w >= 0 so its angle lies in [0, 180] degrees.
    inline Quaternion QuaternionFromRotation(const Matrix4& rotation)
    {
        const double m00 = rotation.m[0][0];
        const double m11 = rotation.m[1][1];
        const double m22 = rotation.m[2][2];
        const double trace = m00 + m11 + m22;
        Quaternion result;

        if(trace > 0.0)
        {
            const double scale = std::sqrt(trace + 1.0) * 2.0;
            result.w = 0.25 * scale;
            result.x = (rotation.m[2][1] - rotation.m[1][2]) / scale;
            result.y = (rotation.m[0][2] - rotation.m[2][0]) / scale;
            result.z = (rotation.m[1][0] - rotation.m[0][1]) / scale;
        }
        else if(m00 > m11 && m00 > m22)
        {
            const double scale = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
            result.w = (rotation.m[2][1] - rotation.m[1][2]) / scale;
            result.x = 0.25 * scale;
            result.y = (rotation.m[0][1] + rotation.m[1][0]) / scale;
            result.z = (rotation.m[0][2] + rotation.m[2][0]) / scale;
        }
        else if(m11 > m22)
        {
            const double scale = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
            result.w = (rotation.m[0][2] - rotation.m[2][0]) / scale;
            result.x = (rotation.m[0][1] + rotation.m[1][0]) / scale;
            result.y = 0.25 * scale;
            result.z = (rotation.m[1][2] + rotation.m[2][1]) / scale;
        }
        else
        {
            const double scale = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
            result.w = (rotation.m[1][0] - rotation.m[0][1]) / scale;
            result.x = (rotation.m[0][2] + rotation.m[2][0]) / scale;
            result.y = (rotation.m[1][2] + rotation.m[2][1]) / scale;
            result.z = 0.25 * scale;
        }

        if(result.w < 0.0)
        {
            result = {-result.w, -result.x, -result.y, -result.z};
        }

        return result;
    }

    // Rodrigues rotation of radians around a unit axis.
    inline Matrix4 AxisRotation(const Vector3& axis, double radians)
    {
        const double cosine = std::cos(radians);
        const double sine = std::sin(radians);
        const double oneMinusCosine = 1.0 - cosine;
        const double x = axis.x;
        const double y = axis.y;
        const double z = axis.z;

        Matrix4 rotation = Matrix4::Identity();
        rotation.m[0][0] = static_cast<float>(cosine + x * x * oneMinusCosine);
        rotation.m[0][1] = static_cast<float>(x * y * oneMinusCosine - z * sine);
        rotation.m[0][2] = static_cast<float>(x * z * oneMinusCosine + y * sine);
        rotation.m[1][0] = static_cast<float>(y * x * oneMinusCosine + z * sine);
        rotation.m[1][1] = static_cast<float>(cosine + y * y * oneMinusCosine);
        rotation.m[1][2] = static_cast<float>(y * z * oneMinusCosine - x * sine);
        rotation.m[2][0] = static_cast<float>(z * x * oneMinusCosine - y * sine);
        rotation.m[2][1] = static_cast<float>(z * y * oneMinusCosine + x * sine);
        rotation.m[2][2] = static_cast<float>(cosine + z * z * oneMinusCosine);
        return rotation;
    }

    // Signed twist angle of a rotation around a unit world axis, in (-180, 180] degrees.
    //
    // Any rotation factors as twist(axis) * swing, where swing's axis is perpendicular to the
    // world axis. A world-axis gizmo ring pre-multiplies by a rotation around that axis, which
    // changes only the twist. The twist is therefore the "absolute angle" G.2 snaps: for an
    // object rotated only around that axis it equals the familiar Euler angle. When the swing is
    // a half-turn the twist is undefined; atan2(0, 0) then deterministically reports 0.
    inline double TwistDegrees(const Matrix4& rotation, const Vector3& axis)
    {
        const Quaternion quaternion = QuaternionFromRotation(rotation);
        const double projection = quaternion.x * axis.x + quaternion.y * axis.y + quaternion.z * axis.z;
        double degrees = 2.0 * std::atan2(projection, quaternion.w) * 180.0 / std::numbers::pi;

        if(degrees <= -180.0)
        {
            degrees += 360.0;
        }
        else if(degrees > 180.0)
        {
            degrees -= 360.0;
        }

        return degrees;
    }

    // Tracks one rotation drag. ImGuizmo rotates incrementally from the matrix it is given, so
    // the Editor feeds it a private continuous matrix and reports each continuous frame here.
    // The tracker unwraps per-frame rotation into an unbounded signed angle around a fixed axis
    // (any number of turns), then snaps the start's twist plus that angle. The snapped result is
    // always rebuilt from the drag-start rotation and is never fed back to the gizmo.
    class RotationDrag
    {
    public:
        void Begin(const Matrix4& startRotation, const Matrix4& continuousRotation)
        {
            m_startRotation = startRotation;
            m_previousContinuous = continuousRotation;
            m_axis = {};
            m_axisKnown = false;
            m_continuousRadians = 0.0;
        }

        void Update(const Matrix4& continuousRotation)
        {
            // Frame delta in world space: continuous = delta * previous.
            const Matrix4 delta = continuousRotation * m_previousContinuous.Transpose();
            const Quaternion quaternion = QuaternionFromRotation(delta);
            m_previousContinuous = continuousRotation;

            const double sineHalf = std::sqrt(
                quaternion.x * quaternion.x + quaternion.y * quaternion.y + quaternion.z * quaternion.z);

            if(!m_axisKnown)
            {
                if(sineHalf < 1.0e-7)
                {
                    return;
                }

                m_axis = CanonicalAxis(quaternion, sineHalf);
                m_axisKnown = true;
            }

            m_continuousRadians += SignedAngleAroundAxis(delta, m_axis);
        }

        bool HasAxis() const
        {
            return m_axisKnown;
        }

        const Vector3& Axis() const
        {
            return m_axis;
        }

        double ContinuousDegrees() const
        {
            return m_continuousRadians * 180.0 / std::numbers::pi;
        }

        // Rotation to apply around Axis() relative to the drag start, in degrees. Zero means the
        // authored start rotation must be kept exactly.
        double SnappedDeltaDegrees(float intervalDegrees) const
        {
            if(!m_axisKnown || !IsValidInterval(intervalDegrees))
            {
                return 0.0;
            }

            const double startTwist = TwistDegrees(m_startRotation, m_axis);
            const double continuousTwist = startTwist + ContinuousDegrees();
            return SnapValue(startTwist, continuousTwist, intervalDegrees) - startTwist;
        }

        Matrix4 SnappedRotation(float intervalDegrees) const
        {
            const double deltaDegrees = SnappedDeltaDegrees(intervalDegrees);

            if(deltaDegrees == 0.0)
            {
                return m_startRotation;
            }

            return AxisRotation(m_axis, deltaDegrees * std::numbers::pi / 180.0) * m_startRotation;
        }

    private:
        // Signed angle of a (near) rotation around a unit axis. For R = rotation(axis, angle):
        // axis . vee(R - R^T) / 2 = sin(angle) and (trace(R) - axis^T R axis) / 2 = cos(angle).
        // Both scale identically under the slow uniform scale creep of ImGuizmo's float matrix
        // products, so atan2 stays exact where a quaternion's sqrt(trace + 1) would bias every
        // frame and accumulate visible drift. One frame of mouse motion stays well under a half
        // turn, so per-frame angles unwrap correctly into the running total.
        static double SignedAngleAroundAxis(const Matrix4& delta, const Vector3& axis)
        {
            const double veeX = static_cast<double>(delta.m[2][1]) - delta.m[1][2];
            const double veeY = static_cast<double>(delta.m[0][2]) - delta.m[2][0];
            const double veeZ = static_cast<double>(delta.m[1][0]) - delta.m[0][1];
            const double sinePart = 0.5 * (veeX * axis.x + veeY * axis.y + veeZ * axis.z);

            double axialPart = 0.0;

            for(int row = 0; row < 3; ++row)
            {
                for(int column = 0; column < 3; ++column)
                {
                    const double axisRow = row == 0 ? axis.x : (row == 1 ? axis.y : axis.z);
                    const double axisColumn = column == 0 ? axis.x : (column == 1 ? axis.y : axis.z);
                    axialPart += axisRow * delta.m[row][column] * axisColumn;
                }
            }

            const double trace = static_cast<double>(delta.m[0][0]) + delta.m[1][1] + delta.m[2][2];
            const double cosinePart = 0.5 * (trace - axialPart);
            return std::atan2(sinePart, cosinePart);
        }

        // World-axis rings rotate about an exact unit axis; snap float noise back onto it. The
        // sign is fixed so the axis is independent of the first drag direction.
        static Vector3 CanonicalAxis(const Quaternion& quaternion, double sineHalf)
        {
            double x = quaternion.x / sineHalf;
            double y = quaternion.y / sineHalf;
            double z = quaternion.z / sineHalf;
            const double ax = std::abs(x);
            const double ay = std::abs(y);
            const double az = std::abs(z);
            constexpr double kWorldAxisTolerance = 1.0e-4;

            if(ax >= ay && ax >= az)
            {
                if(1.0 - ax < kWorldAxisTolerance)
                {
                    return {1.0f, 0.0f, 0.0f};
                }

                return Signed(x, y, z, x);
            }

            if(ay >= az)
            {
                if(1.0 - ay < kWorldAxisTolerance)
                {
                    return {0.0f, 1.0f, 0.0f};
                }

                return Signed(x, y, z, y);
            }

            if(1.0 - az < kWorldAxisTolerance)
            {
                return {0.0f, 0.0f, 1.0f};
            }

            return Signed(x, y, z, z);
        }

        static Vector3 Signed(double x, double y, double z, double dominant)
        {
            const double sign = dominant < 0.0 ? -1.0 : 1.0;
            return {static_cast<float>(x * sign), static_cast<float>(y * sign), static_cast<float>(z * sign)};
        }

        Matrix4 m_startRotation = Matrix4::Identity();
        Matrix4 m_previousContinuous = Matrix4::Identity();
        Vector3 m_axis;
        bool m_axisKnown = false;
        double m_continuousRadians = 0.0;
    };
}
