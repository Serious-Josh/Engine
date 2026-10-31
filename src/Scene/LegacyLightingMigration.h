#pragma once

#include "Scene/SceneDocument.h"
#include "Math/Math.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

// The scene-level sun of documents before schema 13 / cooked payload 13, in legacy light units.
struct LegacySceneSun
{
    Vector3 direction{0.5f, -0.5f, 1.0f};
    float intensity = 1.0f;
    Vector3 color{1.0f, 1.0f, 1.0f};
    float ambientStrength = 0.15f;
    float angularRadius = ToRadians(0.27f);
};

// Codec boundary only: consumes legacy settings, never retains a second authoritative sun.
// Call once for historical input, never for a current document or ordinary object edits. The resulting
// Directional keeps the legacy intensity; MigrateLegacyLightUnits then converts it with every other Light.
// Returns the legacy ambient strength, which only the migration report records.
inline float MigrateLegacyLighting(SceneDocument& document, const std::optional<LegacySceneSun>& legacy)
{
    if(!legacy) return 0.0f;
    const auto& sun = *legacy;
    const auto finite3 = [](const Vector3& v)
        { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    if(!finite3(sun.direction) || !finite3(sun.color) || !std::isfinite(sun.intensity) || sun.intensity < 0 ||
        !std::isfinite(sun.angularRadius) || sun.angularRadius < 0 ||
        !std::isfinite(sun.ambientStrength) || sun.ambientStrength < 0)
        throw std::runtime_error("Invalid legacy lighting settings.");
    bool hasDirectional = false;
    for(auto& object : document.objects)
    {
        if(object.light && object.light->type == LightType::Directional)
        {
            // Historical components inherited this value from the scene sun, including when
            // a different component later became shadow owner. Preserve IDs, hierarchy and pose.
            object.light->angularRadius = sun.angularRadius;
            hasDirectional = true;
        }
    }
    if(hasDirectional) return sun.ambientStrength; // Historical component precedence, including disabled objects.

    SceneObjectDocument object;
    object.id = "legacy-directional-light";
    for(unsigned suffix = 1; std::any_of(document.objects.begin(), document.objects.end(),
        [&](const auto& existing) { return existing.id == object.id; }); ++suffix)
        object.id = "legacy-directional-light-" + std::to_string(suffix);
    object.name = "Directional Light";
    object.light = DefaultLightComponent(LightType::Directional);
    object.light->enabled = true;
    object.light->color = sun.color;
    object.light->intensity = sun.intensity;
    object.light->angularRadius = sun.angularRadius;
    const float length = sun.direction.Length();
    if(!(length > 0.0f) || !std::isfinite(length))
    {
        // No orientation represents a zero/unnormalizable travel vector. Never silently
        // assign another lit direction. Zero-energy historical suns remain dark and editable.
        if(sun.intensity > 0 && (sun.color.x != 0 || sun.color.y != 0 || sun.color.z != 0))
            throw std::runtime_error("Cannot migrate an emitting legacy sun with an unnormalizable direction.");
    }
    else
    {
        const Vector3 direction = sun.direction.Normalized();
        object.transform.rotation = {std::atan2(-direction.y, std::hypot(direction.x, direction.z)),
            std::atan2(direction.x, direction.z), 0.0f};
    }
    // Default TRS gives root position (0,0,0), unit positive scale and +Z forward.
    document.objects.push_back(std::move(object));
    return sun.ambientStrength;
}
