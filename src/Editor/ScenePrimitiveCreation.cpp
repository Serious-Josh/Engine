#include "ScenePrimitiveCreation.h"
#include "SceneObjectDuplication.h"
#include "assets/AssetID.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace
{
// Shared by primitive and empty creation: the existing duplicate-name convention and a
// fresh UUID not used by any object in the document.
void AssignNewObjectIdentity(const SceneDocument& document, SceneObjectDocument& object, const char* objectKind)
{
    const bool nameTaken = std::any_of(document.objects.begin(), document.objects.end(),
        [&](const SceneObjectDocument& existing)
        {
            return existing.name == object.name;
        });

    if(nameTaken)
    {
        object.name = UniqueDuplicateObjectName(object.name, document);
    }

    constexpr unsigned int maximumAttempts = 8;

    for(unsigned int attempt = 0; attempt < maximumAttempts && object.id.empty(); ++attempt)
    {
        const auto id = AssetID::Generate();

        if(!id)
        {
            continue;
        }

        const bool idTaken = std::any_of(document.objects.begin(), document.objects.end(),
            [&](const SceneObjectDocument& existing)
            {
                return existing.id == id->ToString();
            });

        if(!idTaken)
        {
            object.id = id->ToString();
        }
    }

    if(object.id.empty())
    {
        throw std::runtime_error(std::string("Cannot allocate a fresh ") + objectKind + " object identity.");
    }
}

// Fly-view placement shared by Camera and Light creation: position, yaw from the target and
// pitch, unit scale and no roll (the fly view has none). Throws on a non-finite or degenerate
// view instead of inventing an orientation.
void PlaceAtEditorView(SceneObjectDocument& object, const Vector3& viewPosition, const Vector3& viewTarget,
    const char* objectKind)
{
    const Vector3 direction{viewTarget.x - viewPosition.x, viewTarget.y - viewPosition.y,
        viewTarget.z - viewPosition.z};
    const float horizontalLength = std::sqrt(direction.x * direction.x + direction.z * direction.z);
    const float length = std::sqrt(horizontalLength * horizontalLength + direction.y * direction.y);
    const bool finitePose = std::isfinite(viewPosition.x) && std::isfinite(viewPosition.y) &&
        std::isfinite(viewPosition.z) && std::isfinite(length);

    if(!finitePose || length <= 1.0e-6f)
    {
        throw std::invalid_argument(std::string("The Editor view has no valid pose for a new ") + objectKind + ".");
    }

    object.transform.position = viewPosition;
    // Rotation applies X then Y: local +Z becomes (sin y cos x, -sin x, cos y cos x), so the
    // fly view's yaw/pitch map to (-pitch, yaw, 0) and local +Y stays world-up aligned.
    object.transform.rotation = {-std::atan2(direction.y, horizontalLength), std::atan2(direction.x, direction.z), 0.0f};
}
}

SceneObjectDocument PreparePrimitiveObject(const SceneDocument& document, ScenePrimitive primitive,
    const std::string& meshReference)
{
    const auto meshID=AssetID::ParseReference(meshReference);
    if(!meshID || meshID->ToReference()!=meshReference) throw std::invalid_argument("Primitive requires a canonical mesh AssetID.");
    SceneObjectDocument object;
    object.name=PrimitiveMeshAssets::Name(primitive);
    AssignNewObjectIdentity(document, object, "primitive");
    object.meshReference=meshReference;
    object.materialReference="builtin:default";
    // SceneObjectDocument defaults provide identity transform and absent physics.
    return object;
}

SceneObjectDocument PrepareEmptyObject(const SceneDocument& document)
{
    SceneObjectDocument object;
    object.name = "Empty Object";
    AssignNewObjectIdentity(document, object, "empty");

    // Defaults already give an identity transform, empty mesh/material references, no
    // slot bindings and no physics; nothing else is assigned.
    return object;
}

SceneObjectDocument PrepareCameraObject(const SceneDocument& document, const Vector3& viewPosition,
    const Vector3& viewTarget)
{
    SceneObjectDocument object;
    object.name = "Camera";
    PlaceAtEditorView(object, viewPosition, viewTarget, "Camera");
    AssignNewObjectIdentity(document, object, "camera");
    object.camera = CameraComponent{};
    object.camera->main = std::none_of(document.objects.begin(), document.objects.end(),
        [](const SceneObjectDocument& existing) { return existing.camera && existing.camera->main; });
    return object;
}

SceneObjectDocument PrepareLightObject(const SceneDocument& document, LightType type, const Vector3& viewPosition,
    const Vector3& viewTarget)
{
    const char* typeName = LightTypeName(type);

    if(!typeName)
    {
        throw std::invalid_argument("Unsupported Light type for creation.");
    }

    const auto existingLights = std::count_if(document.objects.begin(), document.objects.end(),
        [](const SceneObjectDocument& existing) { return existing.light.has_value(); });

    // Checked before any identity is allocated; commit revalidates the whole scene as well.
    if(existingLights >= static_cast<std::ptrdiff_t>(MaximumSceneLights))
    {
        throw std::runtime_error("The scene already has the maximum of " + std::to_string(MaximumSceneLights) +
            " Light components.");
    }

    SceneObjectDocument object;
    object.name = std::string(typeName) + " Light";
    PlaceAtEditorView(object, viewPosition, viewTarget, "Light");

    // A Point Light at the eye would sit inside its own range sphere; place it ahead instead,
    // where it lights what the view is looking at. Spot and Directional keep the eye position
    // so they shine along the view, like the Camera convention.
    if(type == LightType::Point)
    {
        const Vector3 forward = (viewTarget - viewPosition).Normalized();
        object.transform.position = viewPosition + forward * PointLightCreationDistance;
    }

    AssignNewObjectIdentity(document, object, "light");
    object.light = DefaultLightComponent(type);
    return object;
}
