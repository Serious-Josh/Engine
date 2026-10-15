#pragma once

#include "Scene/Transform.h"
#include "Scene/DirectionalLight.h"

#include <cstdint>
#include <string>
#include <vector>
#include <map>

struct SceneCameraDocument
{
    Vector3 position{0.0f, 0.0f, -5.0f};
    Vector3 target{0.0f, 0.0f, 0.0f};

    float verticalFov = 1.04719755f;
    float aspectRatio = 16.0f / 9.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
};

struct SceneObjectDocument
{
    std::string id;
    std::string name = "Object";

    Transform transform;

    //persistant asset references
    std::string meshReference;
    std::string materialReference;

    // Canonical material asset references keyed by stable mesh-local slot ID.
    // Missing entries inherit materialReference (or builtin:default when empty).
    std::map<std::string, std::string> materialSlotBindings;
};

struct SceneDocument
{
    static constexpr std::uint32_t kSchemaVersion = 1;

    std::uint32_t schemaVersion = kSchemaVersion;

    std::string id;
    std::string name = "Untitled Scene";
    // Empty selects neutral; otherwise a canonical project texture asset reference.
    std::string environmentReference;

    std::vector<SceneObjectDocument> objects;

    SceneCameraDocument camera;
    DirectionalLight directionalLight;
};

bool ValidateSceneDocument(
    const SceneDocument& document,
    std::string* errorMessage = nullptr);
