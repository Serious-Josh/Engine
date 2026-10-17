#pragma once
#include "assets/PrimitiveMeshAssets.h"
#include "Scene/SceneDocument.h"

// Creates authored data only. Engine stages history and publishes membership separately.
SceneObjectDocument PreparePrimitiveObject(const SceneDocument&, ScenePrimitive, const std::string& meshReference);

// A Transform-only object: unique "Empty Object" name, fresh stable ID, identity transform,
// and empty mesh/material references, slot bindings and physics. The empty material
// reference keeps it out of cooked material dependencies.
SceneObjectDocument PrepareEmptyObject(const SceneDocument&);
