#pragma once

#include "Math/Vector3.h"
#include "Math/Matrix4.h"

class Camera
{
    public:
        void SetPosition(const Vector3& position)
        {
            m_position = position;
        }

        void SetTarget(const Vector3& target)
        {
            m_target = target;
        }

        void SetAspectRatio(float aspectRatio)
        {
            m_aspectRatio = aspectRatio;
        }

        void SetPerspective(float verticalFov, float aspectRatio, float nearPlane, float farPlane)
        {
            m_verticalFov = verticalFov;
            m_aspectRatio = aspectRatio;
            m_nearPlane = nearPlane;
            m_farPlane = farPlane;
        }

        Matrix4 GetViewMatrix() const
        {
            return Matrix4::LookAt(
                m_position,
                m_target,
                {0.0f, 1.0f, 0.0f}
            );
        }

        Matrix4 GetProjectionMatrix() const
        {
            return Matrix4::Perspective(
                m_verticalFov,
                m_aspectRatio,
                m_nearPlane,
                m_farPlane
            );
        }

        const Vector3& GetPosition() const
        {
            return m_position;
        }

        const Vector3& GetTarget() const
        {
            return m_target;
        }

    private:
        Vector3 m_position{0.0f, 0.0f, -5.0f};
        Vector3 m_target{0.0f, 0.0f, 0.0f};

        float m_verticalFov = 1.04719755f; // 60 degrees
        float m_aspectRatio = 16.0f / 9.0f;
        float m_nearPlane = 0.1f;
        float m_farPlane = 1000.0f;
};