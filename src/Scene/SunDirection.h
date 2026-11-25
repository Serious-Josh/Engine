#pragma once

#include "Math/Matrix4.h"
#include "Math/Vector3.h"

#include <algorithm>
#include <cmath>

// Physical sky S2: a Directional Light's orientation as the sun's elevation and azimuth. A light travels along its rotated +Z
// (SceneLighting.cpp, ResolveLightWorldPlacement), and Transform rotations are Euler radians applied as Rz Ry Rx
// (Matrix4::Rotation). Elevation is the angle of the direction toward the sun above the horizon; azimuth is that direction's
// heading, clockwise from +Z toward +X, in [0, 360).
struct SunAngles
{
    float elevationDegrees = 45.0f;
    float azimuthDegrees = 135.0f;
};

namespace SunDirection
{
    inline constexpr double Pi = 3.14159265358979323846;

    // The light's travel direction for a rotation (roll has no effect).
    inline Vector3 TravelFromRotation(const Vector3& rotation)
    {
        const Matrix4 matrix = Matrix4::Rotation(rotation);
        return Vector3{matrix.m[0][2], matrix.m[1][2], matrix.m[2][2]};
    }

    inline SunAngles AnglesFromTravel(const Vector3& travel)
    {
        const double length = std::sqrt(double(travel.x) * travel.x + double(travel.y) * travel.y + double(travel.z) * travel.z);

        if(!(length > 0.0))
        {
            return SunAngles{90.0f, 0.0f};
        }

        const double towardX = -travel.x / length;
        const double towardY = -travel.y / length;
        const double towardZ = -travel.z / length;
        double azimuth = std::atan2(towardX, towardZ) * 180.0 / Pi;

        if(azimuth < 0.0)
        {
            azimuth += 360.0;
        }

        return SunAngles{float(std::asin(std::clamp(towardY, -1.0, 1.0)) * 180.0 / Pi), float(azimuth)};
    }

    // A rotation whose travel direction points from the sun at these angles toward the ground (roll 0).
    inline Vector3 RotationFromAngles(const SunAngles& angles)
    {
        const double elevation = angles.elevationDegrees * Pi / 180.0;
        const double azimuth = angles.azimuthDegrees * Pi / 180.0;
        return Vector3{float(elevation), float(azimuth - Pi), 0.0f};
    }
}
