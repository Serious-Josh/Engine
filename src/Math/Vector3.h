#pragma once

#include <cmath>

struct Vector3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vector3() = default;

    Vector3(float x, float y, float z) : x(x), y(y), z(z)
    {
    }

    Vector3 operator+(const Vector3& other) const
    {
        return {x + other.x, y + other.y, z + other.z};
    }

    Vector3 operator-(const Vector3& other) const
    {
        return {x - other.x, y - other.y, z - other.z};
    }

    Vector3 operator*(float scalar) const
    {
        return {x * scalar, y * scalar, z * scalar};
    }

    float Length() const
    {
        return std::sqrt(x * x + y * y + z * z);
    }

    Vector3 Normalized() const
    {
        const float length = Length();

        if (length == 0.0f)
        {
            return {};
        }

        return {x / length, y / length, z / length};
    }

    static float Dot(
        const Vector3& a,
        const Vector3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static Vector3 Cross(
        const Vector3& a,
        const Vector3& b)
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }
};
