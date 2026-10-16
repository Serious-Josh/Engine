#pragma once
#include "assets/PrimitiveMeshAssets.h"
#include "Scene/SceneDocument.h"

// Creates authored data only. Engine stages history and publishes membership separately.
SceneObjectDocument PreparePrimitiveObject(const SceneDocument&, ScenePrimitive, const std::string& meshReference);
