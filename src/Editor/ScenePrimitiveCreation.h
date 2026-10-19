#pragma once
#include "assets/PrimitiveMeshAssets.h"
#include "Scene/SceneDocument.h"

// Creates authored data only. Engine stages history and publishes membership separately.
SceneObjectDocument PreparePrimitiveObject(const SceneDocument&, ScenePrimitive, const std::string& meshReference);

// A Transform-only object: unique "Empty Object" name, fresh stable ID, identity transform,
// and empty mesh/material references, slot bindings and physics. The empty material
// reference keeps it out of cooked material dependencies.
SceneObjectDocument PrepareEmptyObject(const SceneDocument&);

// A meshless object with a default Camera, named "Camera" under the duplicate-name convention.
// Placed at the fly view (position, yaw from target, pitch; the fly view has no roll) with unit
// scale. Main only when no existing Camera, enabled or not, is Main. Throws on a non-finite or
// degenerate view instead of inventing an orientation.
SceneObjectDocument PrepareCameraObject(const SceneDocument&, const Vector3& viewPosition, const Vector3& viewTarget);

// A meshless object with a Light of the given type and its creation defaults, named
// "Point Light", "Spot Light" or "Directional Light" under the duplicate-name convention.
// Oriented along the fly view (local +Z = view direction, no roll, unit scale). Spot and
// Directional sit at the view position; a Point Light sits PointLightCreationDistance ahead.
// Throws on a non-finite or degenerate view.
inline constexpr float PointLightCreationDistance = 3.0f;
SceneObjectDocument PrepareLightObject(const SceneDocument&, LightType type, const Vector3& viewPosition,
    const Vector3& viewTarget);
