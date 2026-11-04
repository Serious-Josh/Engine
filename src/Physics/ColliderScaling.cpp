#include "Physics/ColliderScaling.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <sstream>

namespace Physics
{
namespace
{
Vector3 Absolute(const Vector3& vector)
{
    return {std::fabs(vector.x), std::fabs(vector.y), std::fabs(vector.z)};
}

// Active Rz * Ry * Rx rotation (the Transform and capsule-rotation convention): X is applied first.
Vector3 RotateEuler(const Vector3& vector, const Vector3& rotation)
{
    const float sineX = std::sin(rotation.x);
    const float cosineX = std::cos(rotation.x);
    const Vector3 aboutX{vector.x, cosineX * vector.y - sineX * vector.z, sineX * vector.y + cosineX * vector.z};

    const float sineY = std::sin(rotation.y);
    const float cosineY = std::cos(rotation.y);
    const Vector3 aboutY{cosineY * aboutX.x + sineY * aboutX.z, aboutX.y, -sineY * aboutX.x + cosineY * aboutX.z};

    const float sineZ = std::sin(rotation.z);
    const float cosineZ = std::cos(rotation.z);
    return {cosineZ * aboutY.x - sineZ * aboutY.y, sineZ * aboutY.x + cosineZ * aboutY.y, aboutY.z};
}

// Length of a unit direction after the per-axis absolute scale.
float ScaledLength(const Vector3& absoluteScale, const Vector3& direction)
{
    const Vector3 scaled{absoluteScale.x * direction.x, absoluteScale.y * direction.y, absoluteScale.z * direction.z};
    return std::sqrt(scaled.x * scaled.x + scaled.y * scaled.y + scaled.z * scaled.z);
}

// The single expression every effective value uses; LocalizeStoredCollider inverts exactly this.
float Scaled(float authored, float factor)
{
    return authored * factor;
}

template <typename Component>
Component ScaleComponent(const Component& authored, const Vector3& objectScale)
{
    const ColliderScaleFactors factors = ComputeColliderScaleFactors(objectScale, authored.capsuleLocalRotation);
    Component effective = authored;

    effective.boxDimensions.width = Scaled(authored.boxDimensions.width, factors.box.x);
    effective.boxDimensions.height = Scaled(authored.boxDimensions.height, factors.box.y);
    effective.boxDimensions.depth = Scaled(authored.boxDimensions.depth, factors.box.z);
    effective.sphereRadius = Scaled(authored.sphereRadius, factors.sphereRadius);
    effective.capsuleRadius = Scaled(authored.capsuleRadius, factors.capsuleRadius);
    effective.capsuleCylinderHeight = Scaled(authored.capsuleCylinderHeight, factors.capsuleCylinderHeight);
    effective.colliderLocalPositionOffset = {
        Scaled(authored.colliderLocalPositionOffset.x, factors.offset.x),
        Scaled(authored.colliderLocalPositionOffset.y, factors.offset.y),
        Scaled(authored.colliderLocalPositionOffset.z, factors.offset.z)
    };

    return effective;
}

// Diagnostic text for meters and factors: significant digits, so sub-micrometre residuals stay visible.
std::string FormatNumber(float value)
{
    std::ostringstream stream;
    stream.precision(9);
    stream << value;
    return stream.str();
}

struct ValueConversion
{
    bool possible = false;
    float value = 0.0f;
    // |Scaled(value, factor) - stored|; zero means the stored value is reproduced exactly.
    float residual = 0.0f;
};

// The authored value whose Scaled() product reproduces `stored`. Division is the first guess and neighbouring floats
// are searched for an exact product. Float multiplication skips some results, so an exact value does not always
// exist; the nearest product is then accepted only within one unit in the last place of `stored`.
ValueConversion ConvertStoredValue(float stored, float factor)
{
    if(!std::isfinite(stored) || !std::isfinite(factor))
    {
        return {};
    }

    if(factor == 0.0f)
    {
        // Every authored value scales to zero; only a stored zero is reproducible.
        return stored == 0.0f ? ValueConversion{true, 0.0f, 0.0f} : ValueConversion{};
    }

    const float guess = stored / factor;

    if(!std::isfinite(guess))
    {
        return {};
    }

    const auto residualOf = [&](float candidate)
    {
        const float product = Scaled(candidate, factor);
        return std::isfinite(product) ? std::fabs(product - stored) : INFINITY;
    };

    ValueConversion best{true, guess, residualOf(guess)};
    constexpr int neighbourSteps = 8;
    float below = guess;
    float above = guess;

    for(int step = 0; step < neighbourSteps && best.residual > 0.0f; ++step)
    {
        below = std::nextafter(below, -INFINITY);
        above = std::nextafter(above, INFINITY);

        for(const float candidate : {below, above})
        {
            const float residual = residualOf(candidate);

            if(residual < best.residual)
            {
                best = {true, candidate, residual};
            }
        }
    }

    const float storedSpacing = std::nextafter(std::fabs(stored), INFINITY) - std::fabs(stored);

    if(!(best.residual <= storedSpacing))
    {
        return {};
    }

    return best;
}

struct FieldConversion
{
    float* field;
    float factor;
    const char* name;
    bool active;
};

template <typename Component>
ColliderMigrationOutcome LocalizeComponent(Component& component, const Vector3& objectScale, std::string& detail)
{
    detail.clear();
    const ColliderScaleFactors factors = ComputeColliderScaleFactors(objectScale, component.capsuleLocalRotation);
    const ColliderType type = component.colliderType;
    Vector3& offset = component.colliderLocalPositionOffset;

    const FieldConversion conversions[] = {
        {&component.boxDimensions.width, factors.box.x, "box width", type == ColliderType::Box},
        {&component.boxDimensions.height, factors.box.y, "box height", type == ColliderType::Box},
        {&component.boxDimensions.depth, factors.box.z, "box depth", type == ColliderType::Box},
        {&component.sphereRadius, factors.sphereRadius, "sphere radius", type == ColliderType::Sphere},
        {&component.capsuleRadius, factors.capsuleRadius, "capsule radius", type == ColliderType::Capsule},
        {&component.capsuleCylinderHeight, factors.capsuleCylinderHeight, "capsule cylinder height",
            type == ColliderType::Capsule},
        {&offset.x, factors.offset.x, "offset x", true},
        {&offset.y, factors.offset.y, "offset y", true},
        {&offset.z, factors.offset.z, "offset z", true}
    };

    // Resolve every field before changing anything, so an unconvertible collider stays exactly as stored.
    float converted[std::size(conversions)] = {};
    bool nearest = false;

    for(std::size_t index = 0; index < std::size(conversions); ++index)
    {
        const FieldConversion& conversion = conversions[index];
        const ValueConversion result = ConvertStoredValue(*conversion.field, conversion.factor);

        if(!conversion.active)
        {
            // Inactive fields do not shape the current collider; keep the stored value if no conversion exists.
            converted[index] = result.possible ? result.value : *conversion.field;
            continue;
        }

        if(!result.possible)
        {
            detail = std::string("the stored ") + conversion.name + " (" + FormatNumber(*conversion.field) +
                " m) has no object-local value at scale factor " + FormatNumber(conversion.factor);
            return ColliderMigrationOutcome::Unconvertible;
        }

        if(result.residual > 0.0f)
        {
            nearest = true;
            detail += (detail.empty() ? "" : "; ") + std::string(conversion.name) + " differs from its stored value " +
                FormatNumber(*conversion.field) + " m by " + FormatNumber(result.residual) +
                " m (no exact object-local float exists)";
        }

        converted[index] = result.value;
    }

    for(std::size_t index = 0; index < std::size(conversions); ++index)
    {
        *conversions[index].field = converted[index];
    }

    return nearest ? ColliderMigrationOutcome::Nearest : ColliderMigrationOutcome::Exact;
}
}

ColliderScaleFactors ComputeColliderScaleFactors(const Vector3& objectScale, const Vector3& capsuleLocalRotation)
{
    const Vector3 absoluteScale = Absolute(objectScale);
    ColliderScaleFactors factors;
    factors.box = absoluteScale;
    factors.offset = objectScale;

    const bool uniform = absoluteScale.x == absoluteScale.y && absoluteScale.y == absoluteScale.z;

    if(uniform)
    {
        // One exact factor: no rounding from rotated axes, whatever the capsule rotation.
        factors.sphereRadius = absoluteScale.x;
        factors.capsuleRadius = absoluteScale.x;
        factors.capsuleCylinderHeight = absoluteScale.x;
        return factors;
    }

    factors.sphereRadius = std::max({absoluteScale.x, absoluteScale.y, absoluteScale.z});

    const bool unrotatedCapsule =
        capsuleLocalRotation.x == 0.0f && capsuleLocalRotation.y == 0.0f && capsuleLocalRotation.z == 0.0f;

    if(unrotatedCapsule)
    {
        factors.capsuleRadius = std::max(absoluteScale.x, absoluteScale.z);
        factors.capsuleCylinderHeight = absoluteScale.y;
        return factors;
    }

    const Vector3 axis = RotateEuler({0.0f, 1.0f, 0.0f}, capsuleLocalRotation);
    const Vector3 crossU = RotateEuler({1.0f, 0.0f, 0.0f}, capsuleLocalRotation);
    const Vector3 crossW = RotateEuler({0.0f, 0.0f, 1.0f}, capsuleLocalRotation);
    factors.capsuleCylinderHeight = ScaledLength(absoluteScale, axis);
    factors.capsuleRadius = std::max(ScaledLength(absoluteScale, crossU), ScaledLength(absoluteScale, crossW));
    return factors;
}

PhysicsComponent ScaleCollider(const PhysicsComponent& authored, const Vector3& objectScale)
{
    return ScaleComponent(authored, objectScale);
}

TriggerColliderComponent ScaleCollider(const TriggerColliderComponent& authored, const Vector3& objectScale)
{
    return ScaleComponent(authored, objectScale);
}

ColliderMigrationOutcome LocalizeStoredCollider(PhysicsComponent& component, const Vector3& objectScale,
    std::string& detail)
{
    return LocalizeComponent(component, objectScale, detail);
}

ColliderMigrationOutcome LocalizeStoredCollider(TriggerColliderComponent& component, const Vector3& objectScale,
    std::string& detail)
{
    return LocalizeComponent(component, objectScale, detail);
}
}
