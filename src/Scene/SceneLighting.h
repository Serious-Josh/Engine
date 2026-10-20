#pragma once

#include "Math/Matrix4.h"
#include "Math/Vector3.h"
#include "Scene/LightComponent.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct SceneDocument;
struct SceneObjectDocument;

// One enabled Light resolved at an accepted world transform. Values only: no scene object,
// component pointer or GPU resource. Direction is the direction light travels (world +Z of the
// object), matching the legacy DirectionalLight::direction convention.
struct ResolvedSceneLight
{
    LightType type = LightType::Point;
    Vector3 position;
    Vector3 direction{0.0f, 0.0f, 1.0f};
    Vector3 color{1.0f, 1.0f, 1.0f};
    float intensity = 0.0f;
    float range = 0.0f;
    float innerConeAngle = 0.0f;
    float outerConeAngle = 0.0f;

    // The owning scene object's stable ID. Identity only: no lighting path reads it (RTXPT.3 RTXDI only).
    std::string objectID;
};

// Frame-local lighting of one committed scene, captured from a coherent document and its accepted
// presentation transforms and passed by value to the renderer.
//
// Legacy migration: while a scene has no Directional Light components, the scene-level
// DirectionalLight (the historical sun) keeps driving the existing sun path unchanged. Once any
// Directional Light component exists, enabled or not, the legacy sun is suppressed so the two can
// never double-illuminate. The shadow owner then is the first enabled Directional Light in scene
// document order; it alone drives the existing sun path (ray-traced shadows, RTGI and reflection
// secondary lighting). No other Light is ever promoted in its place.
struct SceneLightingSnapshot
{
    // Renderer GPU light table capacity. Equal to the scene bound, so a valid scene always fits.
    static constexpr std::uint32_t MaximumAdditionalLights = MaximumSceneLights;

    bool suppressLegacyDirectional = false;

    // The shadow owner, when it is enabled and resolved this frame.
    std::optional<ResolvedSceneLight> sunDirectional;

    // Point, Spot and non-owner Directional Lights in document order: unshadowed direct light.
    std::vector<ResolvedSceneLight> additionalLights;

    // Diagnostics only: names of Lights skipped this frame because their transform is invalid.
    std::vector<std::string> rejectedNames;
};

// The checked world placement of a Light. Point needs only a finite affine position. Spot and
// Directional also need a direction basis: the same checks as CAM.4 Camera views (finite affine,
// no singular axis, no shear, no reflection); positive scale is removed and full roll is kept.
bool ResolveLightWorldPlacement(const Matrix4& world, LightType type, Vector3& position, Vector3& direction,
    std::string* error = nullptr);

// The rigid world (rotation and translation, scale removed) of a Light with a valid basis.
bool BuildLightRigidWorld(const Matrix4& world, Matrix4& rigidWorld, std::string* error = nullptr);

// The Directional Light that owns the existing shadow/RTGI sun path: the first enabled
// Directional Light in document order, or none.
const SceneObjectDocument* FindDirectionalShadowOwner(const SceneDocument& document);

// Editor wireframe model matrices for the overlay's unit shapes. They depict direct-light shape
// and nominal influence only, never RTGI or bounced light. Ranges are drawn at their authored
// size (at most 10 km); vertex counts are fixed, so extreme values stay bounded and finite.
// - Point: the unit Sphere (three great circles) scaled to the range, centered on the Light.
// - Spot: the unit Cone, apex at the Light, axis along world +Z of the rigid world; radial edges
//   have the authored range as length and the ring sits at the authored half-angle.
// - Directional: the unit Arrows at a fixed schematic length, since the light has no range.
inline constexpr float DirectionalLightDisplayLength = 1.5f;
Matrix4 BuildPointLightWireframeModel(const Vector3& position, float range);
Matrix4 BuildSpotLightConeModel(const Matrix4& rigidWorld, float range, float coneHalfAngle);
Matrix4 BuildDirectionalLightArrowsModel(const Matrix4& rigidWorld);

// Builds a whole snapshot from a validated document. worldOf returns an object's accepted world,
// or nothing when the object is not part of the committed runtime scene. A Light whose world is
// missing or invalid is skipped (named in rejectedNames) and the remaining Lights still publish.
// Fails without touching the output when the document exceeds the GPU light table.
bool BuildSceneLightingSnapshot(const SceneDocument& document,
    const std::function<std::optional<Matrix4>(const SceneObjectDocument&)>& worldOf,
    SceneLightingSnapshot& snapshot, std::string* error = nullptr);
