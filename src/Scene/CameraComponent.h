#pragma once

#include "Scene/CameraExposure.h"

#include <cmath>
#include <cstdint>
#include <string>

enum class CameraProjection : std::uint32_t
{
    Perspective = 0
};

// Authored lens/selection intent only. Pose comes from the object's evaluated world
// transform; aspect, runtime selection and renderer histories are never stored here.
struct CameraComponent
{
    static constexpr float MinimumVerticalFov = 0.01745329252f;
    static constexpr float MaximumVerticalFov = 3.12413936107f;
    static constexpr float MinimumNearPlane = 0.001f;
    static constexpr float MaximumFarPlane = 1000000.0f;

    bool enabled = true;
    CameraProjection projection = CameraProjection::Perspective;
    float verticalFov = 1.04719755f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    bool main = false;
    // Photographic exposure of this camera (CameraExposure.h); presentation only.
    CameraExposure exposure;

    bool operator==(const CameraComponent&) const = default;
};

// Shared by authoring, codecs and future checked gameplay APIs. Validate the
// complete proposed value; malformed input must never be silently clamped.
inline bool ValidateCameraComponent(const CameraComponent& camera, std::string* error = nullptr)
{
    if(error)
    {
        error->clear();
    }

    const char* reason = nullptr;

    if(camera.projection != CameraProjection::Perspective)
    {
        reason = "Camera projection must be Perspective.";
    }
    else if(!std::isfinite(camera.verticalFov) || !std::isfinite(camera.nearPlane) ||
        !std::isfinite(camera.farPlane))
    {
        reason = "Camera FOV and clipping planes must be finite.";
    }
    else if(camera.verticalFov < CameraComponent::MinimumVerticalFov ||
        camera.verticalFov > CameraComponent::MaximumVerticalFov)
    {
        reason = "Camera vertical FOV must be between 1 and 179 degrees (stored in radians).";
    }
    else if(camera.nearPlane < CameraComponent::MinimumNearPlane)
    {
        reason = "Camera near plane must be at least 0.001 meters.";
    }
    else if(camera.farPlane <= camera.nearPlane || camera.farPlane > CameraComponent::MaximumFarPlane)
    {
        reason = "Camera far plane must exceed near and be at most 1000000 meters.";
    }
    else if(const char* exposureReason = nullptr; !ValidateCameraExposure(camera.exposure, &exposureReason))
    {
        reason = exposureReason;
    }

    if(reason && error)
    {
        *error = reason;
    }

    return reason == nullptr;
}
