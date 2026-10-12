#pragma once

#include "Math/Vector3.h"
#include "Math/Matrix4.h"

struct Transform
{
    Vector3 position{0.0f, 0.0f, 0.0f};
    Vector3 rotation{0.0f, 0.0f, 0.0f};
    Vector3 scale{1.0f, 1.0f, 1.0f};

    Matrix4 GetModelMatrix() const
    {
        const Matrix4 translationMatrix = Matrix4::Translation(position);
        const Matrix4 rotationmatrix = Matrix4::Rotation(rotation);
        const Matrix4 scaleMatrix = Matrix4::Scale(scale);

        return translationMatrix * rotationmatrix * scaleMatrix;
    }
};
