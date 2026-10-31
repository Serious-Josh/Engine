#include "Scene/SceneLighting.h"

#include "Scene/SceneDocument.h"
#include "Scene/SceneHierarchyTransforms.h"

#include <cmath>

namespace
{
    // Same thresholds as CAM.4's Camera view basis (RenderView.cpp), so a transform that can
    // orient a Camera can orient a Light and vice versa.
    constexpr float kMinimumAxisLength = 1.0e-4f;
    constexpr float kMaximumBasisShear = 1.0e-3f;

    bool Fail(std::string* error, const std::string& message)
    {
        if(error)
        {
            *error = message;
        }

        return false;
    }

    // Normalized, orthogonal, right-handed world axes of an affine matrix, or a reason why not.
    bool ExtractCheckedBasis(const Matrix4& world, Vector3& right, Vector3& up, Vector3& forward, std::string* error)
    {
        const Vector3 xAxis{world.m[0][0], world.m[1][0], world.m[2][0]};
        const Vector3 yAxis{world.m[0][1], world.m[1][1], world.m[2][1]};
        const Vector3 zAxis{world.m[0][2], world.m[1][2], world.m[2][2]};

        if(xAxis.Length() < kMinimumAxisLength || yAxis.Length() < kMinimumAxisLength ||
            zAxis.Length() < kMinimumAxisLength)
        {
            return Fail(error, "Light world transform is singular (an axis has near-zero scale).");
        }

        right = xAxis.Normalized();
        up = yAxis.Normalized();
        forward = zAxis.Normalized();
        const bool sheared = std::abs(Vector3::Dot(right, up)) > kMaximumBasisShear ||
            std::abs(Vector3::Dot(right, forward)) > kMaximumBasisShear ||
            std::abs(Vector3::Dot(up, forward)) > kMaximumBasisShear;

        if(sheared)
        {
            return Fail(error, "Light world transform is sheared; parent scale must keep its axes orthogonal.");
        }

        if(Vector3::Dot(Vector3::Cross(up, forward), right) <= 0.0f)
        {
            return Fail(error, "Light world transform is reflected (negative scale); use positive scale.");
        }

        return true;
    }
}

bool ResolveLightWorldPlacement(const Matrix4& world, LightType type, Vector3& position, Vector3& direction,
    std::string* error)
{
    if(!SceneHierarchy::IsFiniteAffine(world))
    {
        return Fail(error, "Light world transform is not a finite affine matrix.");
    }

    const Vector3 candidatePosition{world.m[0][3], world.m[1][3], world.m[2][3]};
    Vector3 candidateDirection{0.0f, 0.0f, 1.0f};

    // A Point Light emits in every direction; its orientation is irrelevant and is not checked.
    if(type != LightType::Point)
    {
        Vector3 right;
        Vector3 up;

        if(!ExtractCheckedBasis(world, right, up, candidateDirection, error))
        {
            return false;
        }
    }

    position = candidatePosition;
    direction = candidateDirection;
    return true;
}

bool BuildLightRigidWorld(const Matrix4& world, Matrix4& rigidWorld, std::string* error)
{
    if(!SceneHierarchy::IsFiniteAffine(world))
    {
        return Fail(error, "Light world transform is not a finite affine matrix.");
    }

    Vector3 right;
    Vector3 up;
    Vector3 forward;

    if(!ExtractCheckedBasis(world, right, up, forward, error))
    {
        return false;
    }

    Matrix4 candidate = Matrix4::Identity();
    const Vector3 columns[4] = {right, up, forward, {world.m[0][3], world.m[1][3], world.m[2][3]}};

    for(int column = 0; column < 4; ++column)
    {
        candidate.m[0][column] = columns[column].x;
        candidate.m[1][column] = columns[column].y;
        candidate.m[2][column] = columns[column].z;
    }

    rigidWorld = candidate;
    return true;
}

Matrix4 BuildPointLightWireframeModel(const Vector3& position, float range)
{
    return Matrix4::Translation(position) * Matrix4::Scale({range, range, range});
}

Matrix4 BuildSpotLightConeModel(const Matrix4& rigidWorld, float range, float coneHalfAngle)
{
    // The unit ring at z = 1 with radius 1 becomes a ring at range * cos(angle) with radius
    // range * sin(angle), so every apex-to-ring edge is exactly range long.
    const float radius = range * std::sin(coneHalfAngle);
    const float length = range * std::cos(coneHalfAngle);

    return rigidWorld * Matrix4::Scale({radius, radius, length});
}

Matrix4 BuildDirectionalLightArrowsModel(const Matrix4& rigidWorld)
{
    const float length = DirectionalLightDisplayLength;

    return rigidWorld * Matrix4::Scale({length, length, length});
}

const SceneObjectDocument* FindDirectionalShadowOwner(const SceneDocument& document)
{
    for(const auto& object : document.objects)
    {
        if(object.light && object.light->enabled && object.light->type == LightType::Directional)
        {
            return &object;
        }
    }

    return nullptr;
}

bool BuildSceneLightingSnapshot(const SceneDocument& document,
    const std::function<std::optional<Matrix4>(const SceneObjectDocument&)>& worldOf,
    SceneLightingSnapshot& snapshot, std::string* error)
{
    SceneLightingSnapshot candidate;
    candidate.lightingSettings = document.lightingSettings;
    const SceneObjectDocument* shadowOwner = FindDirectionalShadowOwner(document);

    // Document order is the deterministic light order; identity is the stable object ID.
    for(const auto& object : document.objects)
    {
        if(!object.light)
        {
            continue;
        }

        const LightComponent& light = *object.light;

        if(!light.enabled)
        {
            continue;
        }

        // A validated document never carries an invalid Light; recheck anyway, since a bad value
        // must never reach the GPU table.
        const auto world = worldOf(object);
        ResolvedSceneLight resolved;

        if(!world || !ValidateLightComponent(light) ||
            !ResolveLightWorldPlacement(*world, light.type, resolved.position, resolved.direction))
        {
            candidate.rejectedNames.push_back(object.name);
            continue;
        }

        resolved.type = light.type;
        resolved.color = light.color;
        resolved.intensity = light.intensity;
        resolved.angularRadius = light.angularRadius;
        resolved.range = light.range;
        resolved.rangeAttenuation = light.rangeAttenuation;
        resolved.innerConeAngle = light.innerConeAngle;
        resolved.outerConeAngle = light.outerConeAngle;
        resolved.objectID = object.id;

        if(&object == shadowOwner)
        {
            candidate.sunDirectional = resolved;
        }
        else
        {
            candidate.additionalLights.push_back(resolved);
        }
    }

    if(candidate.additionalLights.size() > SceneLightingSnapshot::MaximumAdditionalLights)
    {
        return Fail(error, "The scene has more Light components than the renderer light table holds (" +
            std::to_string(SceneLightingSnapshot::MaximumAdditionalLights) + ").");
    }

    snapshot = std::move(candidate);
    return true;
}
