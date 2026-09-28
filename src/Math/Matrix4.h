#pragma once

#include "Vector3.h"

#include <cstddef>
#include <cmath>

struct Matrix4
{
    float m[4][4]{};

    static Matrix4 Identity()
    {
        Matrix4 result{};

        result.m[0][0] = 1.0f;
        result.m[1][1] = 1.0f;
        result.m[2][2] = 1.0f;
        result.m[3][3] = 1.0f;

        return result;
    }

    static Matrix4 Translation(float x, float y, float z)
    {
        Matrix4 result = Identity();

        result.m[0][3] = x;
        result.m[1][3] = y;
        result.m[2][3] = z;

        return result;
    }

    static Matrix4 Scale(float x, float y, float z)
    {
        Matrix4 result = Identity();

        result.m[0][0] = x;
        result.m[1][1] = y;
        result.m[2][2] = z;

        return result;
    }

    static Matrix4 Translation(const Vector3& translation)
    {
        return Translation(
            translation.x,
            translation.y,
            translation.z
        );
    }

    static Matrix4 Scale(const Vector3& scale)
    {
        return Scale(
            scale.x,
            scale.y,
            scale.z
        );
    }

    static Matrix4 RotationX(float radians)
    {
        Matrix4 result = Identity();

        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);

        result.m[1][1] = cosine;
        result.m[1][2] = -sine;
        result.m[2][1] = sine;
        result.m[2][2] = cosine;

        return result;
    }

    static Matrix4 RotationY(float radians)
    {
        Matrix4 result = Identity();

        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);

        result.m[0][0] = cosine;
        result.m[0][2] = sine;
        result.m[2][0] = -sine;
        result.m[2][2] = cosine;

        return result;
    }

    static Matrix4 RotationZ(float radians)
    {
        Matrix4 result = Identity();

        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);

        result.m[0][0] = cosine;
        result.m[0][1] = -sine;
        result.m[1][0] = sine;
        result.m[1][1] = cosine;

        return result;
    }

    static Matrix4 Rotation(const Vector3& rotation)
    {
        return RotationZ(rotation.z) * RotationY(rotation.y) * RotationX(rotation.x);
    }

    float* operator[](std::size_t row)
    {
        return m[row];
    }

    const float* operator[](std::size_t row) const
    {
        return m[row];
    }

    Matrix4 operator*(const Matrix4& other) const
    {
        Matrix4 result{};

        for(std::size_t row = 0; row < 4; ++row){
            for(std::size_t column = 0; column < 4; ++column){
                for(std::size_t i = 0; i < 4; ++i){
                    result.m[row][column] += m[row][i] * other.m[i][column];
                }
            }
        }

        return result;
    }

    static Matrix4 LookAt(const Vector3& eye, const Vector3& target, const Vector3& up)
    {
        const Vector3 forward = (target - eye).Normalized();
        const Vector3 right = Vector3::Cross(up, forward).Normalized();
        const Vector3 cameraUp = Vector3::Cross(forward, right);

        Matrix4 result = Identity();

        result.m[0][0] = right.x;
        result.m[0][1] = right.y;
        result.m[0][2] = right.z;
        result.m[0][3] = -Vector3::Dot(right, eye);

        result.m[1][0] = cameraUp.x;
        result.m[1][1] = cameraUp.y;
        result.m[1][2] = cameraUp.z;
        result.m[1][3] = -Vector3::Dot(cameraUp, eye);

        result.m[2][0] = forward.x;
        result.m[2][1] = forward.y;
        result.m[2][2] = forward.z;
        result.m[2][3] = -Vector3::Dot(forward, eye);

        return result;
    }

    static Matrix4 Perspective(float verticalFov, float aspectRatio, float nearPlane, float farPlane)
    {
        Matrix4 result{};

        const float yScale = 1.0f / std::tan(verticalFov * 0.5f);
        const float xScale = yScale / aspectRatio;

        result.m[0][0] = xScale;
        result.m[1][1] = yScale;

        result.m[2][2] = farPlane / (farPlane - nearPlane);

        result.m[2][3] = (-nearPlane * farPlane) / (farPlane - nearPlane);

        result.m[3][2] = 1.0f;

        return result;
    }
};