#pragma once

#include "Scene/SceneDocument.h"

#include <string>

class Scene;

// Applies scene-level settings only.
// Does not create render objects or resolve assets.
bool ApplySceneDocumentSettings(
    const SceneDocument& document,
    Scene& scene,
    std::string* errorMessage = nullptr);
struct SceneObjectMapping;

// Preserves saved aspect ratio: viewport resizing is not a document edit.
// Output is unchanged on failure. Does not mutate Scene or GPU resources.
bool CaptureSceneDocument(
    const SceneDocument& source,
    const Scene& scene,
    const SceneObjectMapping& mapping,
    SceneDocument& captured,
    std::string* errorMessage = nullptr);

bool SceneDocumentsEqual(
    const SceneDocument& left,
    const SceneDocument& right);
