#pragma once

#include "Math/Matrix4.h"
#include "Math/Vector3.h"
#include "Scene/CameraComponent.h"

#include <cstdint>
#include <optional>
#include <string>

class Camera;
class FileSceneRuntime;
struct SceneDocument;
struct SceneObjectDocument;

// Which viewpoint a frame renders. Session/runtime state only; never authored or persisted.
enum class RenderViewSource : std::uint8_t
{
    EditorFly,
    SceneCamera
};

// A change of identity is a camera cut: renderer-owned temporal histories are reset before the
// frame that first uses the new identity is recorded. Ordinary motion keeps the same identity.
struct RenderViewIdentity
{
    RenderViewSource source = RenderViewSource::EditorFly;

    // SceneCamera only: the runtime scene instance and the Camera object's stable ID.
    std::uint64_t runtimeIdentity = 0;
    std::string objectID;

    // Reserved for explicit future gameplay cuts on the same Camera.
    std::uint64_t cutRevision = 0;

    bool operator==(const RenderViewIdentity&) const = default;
};

// One coherent, frame-local view. Values only: no scene object, component pointer, GPU resource
// or submission authority. Matrices use Engine's row-indexed storage with column-vector math:
// view rows are (right, up, forward), +Z forward, and projection is Matrix4::Perspective.
struct RenderView
{
    RenderViewIdentity identity;

    Matrix4 view;
    Matrix4 projection;
    Vector3 position;
    Vector3 right;
    Vector3 up;
    Vector3 forward;

    float verticalFov = 0.0f;
    float nearPlane = 0.0f;
    float farPlane = 0.0f;
    float aspectRatio = 0.0f;

    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// The Editor fly camera exactly as rendered before CAM.4: its fixed-up LookAt view and its own
// projection (the Editor keeps the aspect current). Rejects non-finite or degenerate results.
bool BuildFlyRenderView(const Camera& camera, std::uint32_t width, std::uint32_t height, RenderView& view,
    std::string* error = nullptr);

// A Camera component at an accepted world transform. The basis keeps full roll, strips positive
// scale and rejects singular, reflected or sheared worlds; aspect comes from width/height only.
bool BuildCameraRenderView(const Matrix4& world, const CameraComponent& camera, std::uint32_t width,
    std::uint32_t height, RenderViewIdentity identity, RenderView& view, std::string* error = nullptr);

// The authored Main designation of a document, checked for gameplay use: exactly one Main, enabled
// and valid. Zero Main Cameras is reported here even though authoring permits it.
const SceneObjectDocument* FindGameplayMainCamera(const SceneDocument& document, std::string* error = nullptr);

// Build Game readiness: the Main Camera above plus a representable authored world basis,
// evaluated from the document hierarchy (no simulation poses).
bool ValidateGameplayMainCamera(const SceneDocument& document, std::string* error = nullptr);

// Resolves the Main Camera of the committed runtime scene from its document, stable object mapping
// and accepted presentation world in one call. Nothing is retained between frames.
bool ResolveMainCameraView(const FileSceneRuntime& runtime, std::uint32_t width, std::uint32_t height,
    RenderView& view, std::string* error = nullptr);

// CAM.5 Editor schematic: the model matrix that maps the Editor's unit wireframe box
// ([-0.5, 0.5]^3, twelve edges) onto a Camera's frustum. Built on BuildCameraRenderView, so the
// basis, roll and rejection rules are identical; FOV is the authored value and aspect comes
// from the given viewport extent. Depth is bounded to a schematic display range (near rectangle
// at the authored near plane, far rectangle at min(far, CameraFrustumDisplayDepth)). The matrix
// is projective; every box corner maps to a point in front of the Camera with positive w.
inline constexpr float CameraFrustumDisplayDepth = 2.0f;
bool BuildCameraFrustumOverlayModel(const Matrix4& world, const CameraComponent& camera, std::uint32_t width,
    std::uint32_t height, Matrix4& model, std::string* error = nullptr);
