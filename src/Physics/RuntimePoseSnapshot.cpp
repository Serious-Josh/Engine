#include "Physics/RuntimePoseSnapshot.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Physics
{
namespace
{
bool IsFinite(const Vector3& vector)
{
    return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
}

bool Fail(std::string* errorMessage, const char* message)
{
    if (errorMessage)
    {
        *errorMessage = message;
    }
    return false;
}

double QuaternionLengthSquared(const Quaternion& orientation)
{
    // Double intermediates avoid overflow when validating finite float inputs.
    return static_cast<double>(orientation.x) * orientation.x +
           static_cast<double>(orientation.y) * orientation.y +
           static_cast<double>(orientation.z) * orientation.z +
           static_cast<double>(orientation.w) * orientation.w;
}
}

std::span<const RuntimePoseEntry> RuntimePoseSnapshot::GetEntries() const noexcept
{
    return m_entries;
}

const Pose* RuntimePoseSnapshot::FindPose(std::string_view objectID) const noexcept
{
    const auto found = std::lower_bound(m_entries.begin(), m_entries.end(), objectID,
        [](const RuntimePoseEntry& entry, std::string_view requestedID)
        {
            return entry.objectID < requestedID;
        });
    if (found == m_entries.end() || found->objectID != objectID)
    {
        return nullptr;
    }
    return &found->pose;
}

bool ValidateRuntimePose(const Pose& pose, std::string* errorMessage)
{
    if (errorMessage)
    {
        errorMessage->clear();
    }
    if (!IsFinite(pose.position))
    {
        return Fail(errorMessage, "Runtime physics position must be finite");
    }

    const Quaternion& orientation = pose.orientation;
    if (!std::isfinite(orientation.x) || !std::isfinite(orientation.y) ||
        !std::isfinite(orientation.z) || !std::isfinite(orientation.w))
    {
        return Fail(errorMessage, "Runtime physics quaternion components must be finite");
    }

    const double lengthSquared = QuaternionLengthSquared(orientation);
    if (lengthSquared == 0.0)
    {
        return Fail(errorMessage, "Runtime physics quaternion cannot be normalized");
    }
    constexpr double unitQuaternionTolerance = 0.0001;
    if (std::abs(lengthSquared - 1.0) > unitQuaternionTolerance)
    {
        return Fail(errorMessage, "Runtime physics quaternion must be a unit rotation");
    }
    return true;
}

Matrix4 ComposeRuntimeModelMatrix(const Pose& pose, const Vector3& authoredVisualScale)
{
    std::string error;
    if (!ValidateRuntimePose(pose, &error))
    {
        throw std::invalid_argument(error);
    }
    if (!IsFinite(authoredVisualScale))
    {
        throw std::invalid_argument("Authored visual scale must be finite");
    }

    // Normalize only local math intermediates. Snapshot quaternion values stay
    // exactly as returned by Body::GetPose(), including their sign.
    const double inverseLength = 1.0 / std::sqrt(QuaternionLengthSquared(pose.orientation));
    const double x = pose.orientation.x * inverseLength;
    const double y = pose.orientation.y * inverseLength;
    const double z = pose.orientation.z * inverseLength;
    const double w = pose.orientation.w * inverseLength;
    Matrix4 rotation = Matrix4::Identity();
    rotation[0][0] = static_cast<float>(1.0 - 2.0 * (y * y + z * z));
    rotation[0][1] = static_cast<float>(2.0 * (x * y - z * w));
    rotation[0][2] = static_cast<float>(2.0 * (x * z + y * w));
    rotation[1][0] = static_cast<float>(2.0 * (x * y + z * w));
    rotation[1][1] = static_cast<float>(1.0 - 2.0 * (x * x + z * z));
    rotation[1][2] = static_cast<float>(2.0 * (y * z - x * w));
    rotation[2][0] = static_cast<float>(2.0 * (x * z - y * w));
    rotation[2][1] = static_cast<float>(2.0 * (y * z + x * w));
    rotation[2][2] = static_cast<float>(1.0 - 2.0 * (x * x + y * y));

    const Matrix4 model = Matrix4::Translation(pose.position) * rotation * Matrix4::Scale(authoredVisualScale);
    for (std::size_t row = 0; row < 4; ++row)
    {
        for (std::size_t column = 0; column < 4; ++column)
        {
            if (!std::isfinite(model[row][column]))
            {
                throw std::overflow_error("Runtime model matrix is not representable as finite floats");
            }
        }
    }
    return model;
}
}
