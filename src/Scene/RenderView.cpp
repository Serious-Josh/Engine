#include "Scene/RenderView.h"

#include "Scene/Camera.h"
#include "Scene/FileSceneRuntime.h"
#include "Scene/SceneDocument.h"
#include "Scene/SceneHierarchyTransforms.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace
{
    // Shortest admitted world basis column (scale magnitude), matching hierarchy admission.
    constexpr float kMinimumAxisLength = 1.0e-4f;

    // Largest |cos| between normalized world basis columns before the basis counts as sheared.
    // Loose enough for float products of rotated/scaled chains, far below any authored shear.
    constexpr float kMaximumBasisShear = 1.0e-3f;

    bool Fail(std::string* error, const std::string& message)
    {
        if(error)
        {
            *error = message;
        }

        return false;
    }

    bool IsFinite(const Vector3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool IsFinite(const Matrix4& matrix)
    {
        for(const auto& row : matrix.m)
        {
            for(const float value : row)
            {
                if(!std::isfinite(value))
                {
                    return false;
                }
            }
        }

        return true;
    }

    // A rigid view is always invertible; the projection is invertible when its scales and depth
    // terms are finite and nonzero. Matrix4::Inverse returns identity for singular input, so the
    // structure is checked rather than relying on its result alone.
    bool HasCheckedInverse(const Matrix4& projection, const Matrix4& viewProjection)
    {
        const bool projectionInvertible = projection.m[0][0] != 0.0f && projection.m[1][1] != 0.0f &&
            projection.m[2][3] != 0.0f && projection.m[3][2] != 0.0f;
        return projectionInvertible && IsFinite(viewProjection.Inverse());
    }

    // Shared by both adapters: projection from the lens and the actual extent, then final checks.
    bool FinishView(RenderView& view, std::string* error)
    {
        if(view.width == 0 || view.height == 0)
        {
            return Fail(error, "Render view requires a nonzero render extent.");
        }

        view.projection = Matrix4::Perspective(view.verticalFov, view.aspectRatio, view.nearPlane, view.farPlane);
        const Matrix4 viewProjection = view.projection * view.view;

        if(!IsFinite(view.view) || !IsFinite(view.projection) || !HasCheckedInverse(view.projection, viewProjection))
        {
            return Fail(error, "Render view matrices are not finite and invertible.");
        }

        return true;
    }
}

bool BuildFlyRenderView(const Camera& camera, std::uint32_t width, std::uint32_t height, RenderView& view,
    std::string* error)
{
    if((camera.GetTarget() - camera.GetPosition()).Length() <= 0.0f)
    {
        return Fail(error, "The Editor fly camera has no view direction.");
    }

    RenderView candidate;
    candidate.identity = {RenderViewSource::EditorFly};
    candidate.position = camera.GetPosition();
    candidate.forward = (camera.GetTarget() - camera.GetPosition()).Normalized();
    candidate.right = Vector3::Cross({0.0f, 1.0f, 0.0f}, candidate.forward).Normalized();
    candidate.up = Vector3::Cross(candidate.forward, candidate.right);
    candidate.view = camera.GetViewMatrix();
    candidate.verticalFov = camera.GetVerticalFov();
    candidate.nearPlane = camera.GetNearPlane();
    candidate.farPlane = camera.GetFarPlane();
    // The Editor owns the fly aspect (the viewport image); keep it for unchanged rendering.
    candidate.aspectRatio = camera.GetAspectRatio();
    candidate.width = width;
    candidate.height = height;

    if(!IsFinite(candidate.position) || !IsFinite(candidate.forward) || !IsFinite(candidate.right) ||
        !(candidate.aspectRatio > 0.0f) || !FinishView(candidate, error))
    {
        return Fail(error, "The Editor fly camera has no valid view.");
    }

    view = std::move(candidate);
    return true;
}

bool BuildCameraRenderView(const Matrix4& world, const CameraComponent& camera, std::uint32_t width,
    std::uint32_t height, RenderViewIdentity identity, RenderView& view, std::string* error)
{
    std::string lensError;

    if(!ValidateCameraComponent(camera, &lensError))
    {
        return Fail(error, "Invalid Camera lens: " + lensError);
    }

    if(!SceneHierarchy::IsFiniteAffine(world))
    {
        return Fail(error, "Camera world transform is not a finite affine matrix.");
    }

    const Vector3 xAxis{world.m[0][0], world.m[1][0], world.m[2][0]};
    const Vector3 yAxis{world.m[0][1], world.m[1][1], world.m[2][1]};
    const Vector3 zAxis{world.m[0][2], world.m[1][2], world.m[2][2]};

    if(xAxis.Length() < kMinimumAxisLength || yAxis.Length() < kMinimumAxisLength ||
        zAxis.Length() < kMinimumAxisLength)
    {
        return Fail(error, "Camera world transform is singular (an axis has near-zero scale).");
    }

    // Positive scale is removed by normalization; it never changes the lens.
    const Vector3 right = xAxis.Normalized();
    const Vector3 up = yAxis.Normalized();
    const Vector3 forward = zAxis.Normalized();
    const bool sheared = std::abs(Vector3::Dot(right, up)) > kMaximumBasisShear ||
        std::abs(Vector3::Dot(right, forward)) > kMaximumBasisShear ||
        std::abs(Vector3::Dot(up, forward)) > kMaximumBasisShear;

    if(sheared)
    {
        return Fail(error, "Camera world transform is sheared; parent scale must keep its axes orthogonal.");
    }

    // LookAt convention: right = up x forward. A negative triple product is a reflection.
    if(Vector3::Dot(Vector3::Cross(up, forward), right) <= 0.0f)
    {
        return Fail(error, "Camera world transform is reflected (negative scale); use positive scale.");
    }

    RenderView candidate;
    candidate.identity = std::move(identity);
    candidate.position = {world.m[0][3], world.m[1][3], world.m[2][3]};
    candidate.right = right;
    candidate.up = up;
    candidate.forward = forward;
    // Inverse of the rigid world: rows are the basis, translation is -basis . position. Full roll
    // is kept because no global up participates.
    candidate.view = Matrix4::Identity();
    const Vector3 rows[3] = {right, up, forward};

    for(int row = 0; row < 3; ++row)
    {
        candidate.view.m[row][0] = rows[row].x;
        candidate.view.m[row][1] = rows[row].y;
        candidate.view.m[row][2] = rows[row].z;
        candidate.view.m[row][3] = -Vector3::Dot(rows[row], candidate.position);
    }

    candidate.verticalFov = camera.verticalFov;
    candidate.nearPlane = camera.nearPlane;
    candidate.farPlane = camera.farPlane;
    candidate.width = width;
    candidate.height = height;
    candidate.aspectRatio = height == 0 ? 0.0f : static_cast<float>(width) / static_cast<float>(height);

    if(!FinishView(candidate, error))
    {
        return false;
    }

    view = std::move(candidate);
    return true;
}

const SceneObjectDocument* FindGameplayMainCamera(const SceneDocument& document, std::string* error)
{
    const SceneObjectDocument* main = nullptr;

    for(const auto& object : document.objects)
    {
        if(!object.camera || !object.camera->main)
        {
            continue;
        }

        if(main)
        {
            Fail(error, "Multiple Main Cameras: " + main->name + " and " + object.name + ".");
            return nullptr;
        }

        main = &object;
    }

    if(!main)
    {
        Fail(error, "The scene has no Main Camera. Add a Camera and enable Main Camera in its Inspector.");
        return nullptr;
    }

    if(!main->camera->enabled)
    {
        // A disabled Main never falls back to another Camera.
        Fail(error, "Main Camera '" + main->name + "' is disabled. Enable it or make another Camera Main.");
        return nullptr;
    }

    std::string lensError;

    if(!ValidateCameraComponent(*main->camera, &lensError))
    {
        Fail(error, "Main Camera '" + main->name + "' is invalid: " + lensError);
        return nullptr;
    }

    return main;
}

bool ValidateGameplayMainCamera(const SceneDocument& document, std::string* error)
{
    const auto* main = FindGameplayMainCamera(document, error);

    if(!main)
    {
        return false;
    }

    std::unordered_map<std::string, const SceneObjectDocument*> byID;

    for(const auto& object : document.objects)
    {
        byID.emplace(object.id, &object);
    }

    // Authored world: local matrices composed up the parent chain, bounded against cycles.
    Matrix4 world = main->transform.GetModelMatrix();
    const SceneObjectDocument* current = main;

    for(size_t depth = 0; !current->parentID.empty(); ++depth)
    {
        const auto parent = byID.find(current->parentID);

        if(parent == byID.end() || depth >= document.objects.size())
        {
            return Fail(error, "Main Camera '" + main->name + "' has an invalid parent chain.");
        }

        Matrix4 product;

        if(!SceneHierarchy::TryMultiplyAffine(parent->second->transform.GetModelMatrix(), world, product))
        {
            return Fail(error, "Main Camera '" + main->name + "' has a non-finite world transform.");
        }

        world = product;
        current = parent->second;
    }

    RenderView view;
    std::string poseError;

    if(!BuildCameraRenderView(world, *main->camera, 16, 9, {RenderViewSource::SceneCamera}, view, &poseError))
    {
        return Fail(error, "Main Camera '" + main->name + "' cannot be used: " + poseError);
    }

    return true;
}

bool ResolveMainCameraView(const FileSceneRuntime& runtime, std::uint32_t width, std::uint32_t height,
    RenderView& view, std::string* error)
{
    const auto* main = FindGameplayMainCamera(runtime.GetFileScene().GetDocument(), error);

    if(!main)
    {
        return false;
    }

    const auto& mapping = runtime.GetObjectMapping().idToRuntimeIndex;
    const auto found = mapping.find(main->id);
    const auto& objects = runtime.GetScene().GetRenderObjects();

    if(found == mapping.end() || found->second >= objects.size())
    {
        return Fail(error, "Main Camera '" + main->name + "' is not part of the committed runtime scene.");
    }

    const Matrix4 world = objects[found->second].GetPresentationTransform().currentModel;
    std::string poseError;

    if(!BuildCameraRenderView(world, *main->camera, width, height,
        {RenderViewSource::SceneCamera, runtime.GetRuntimeIdentity(), main->id}, view, &poseError))
    {
        return Fail(error, "Main Camera '" + main->name + "' cannot be used: " + poseError);
    }

    return true;
}

bool BuildCameraFrustumOverlayModel(const Matrix4& world, const CameraComponent& camera, std::uint32_t width,
    std::uint32_t height, Matrix4& model, std::string* error)
{
    RenderView view;

    if(!BuildCameraRenderView(world, camera, width, height, {RenderViewSource::SceneCamera}, view, error))
    {
        return false;
    }

    // Bounded schematic depths; the authored near plane stays visible when it is already small.
    const float displayFar = (std::min)(camera.farPlane, CameraFrustumDisplayDepth);
    const float displayNear = (std::min)(camera.nearPlane, displayFar * 0.5f);

    // Rigid world of the validated basis (scale removed), columns right/up/forward/position.
    Matrix4 rigid = Matrix4::Identity();
    const Vector3 columns[4] = {view.right, view.up, view.forward, view.position};

    for(int column = 0; column < 4; ++column)
    {
        rigid.m[0][column] = columns[column].x;
        rigid.m[1][column] = columns[column].y;
        rigid.m[2][column] = columns[column].z;
    }

    // Analytic inverse of Matrix4::Perspective(fov, aspect, displayNear, displayFar): NDC with
    // D3D depth [0, 1] back to camera space, with w = 1 / z > 0 in front of the Camera.
    const float yScale = 1.0f / std::tan(camera.verticalFov * 0.5f);
    const float xScale = yScale / view.aspectRatio;
    const float depthScale = displayFar / (displayFar - displayNear);
    const float depthOffset = -displayNear * displayFar / (displayFar - displayNear);
    Matrix4 inverseProjection{};
    inverseProjection.m[0][0] = 1.0f / xScale;
    inverseProjection.m[1][1] = 1.0f / yScale;
    inverseProjection.m[2][3] = 1.0f;
    inverseProjection.m[3][2] = 1.0f / depthOffset;
    inverseProjection.m[3][3] = -depthScale / depthOffset;

    // Unit box [-0.5, 0.5]^3 to NDC x, y in [-1, 1] and z in [0, 1].
    Matrix4 boxToNdc = Matrix4::Identity();
    boxToNdc.m[0][0] = 2.0f;
    boxToNdc.m[1][1] = 2.0f;
    boxToNdc.m[2][3] = 0.5f;

    const Matrix4 candidate = rigid * inverseProjection * boxToNdc;

    if(!IsFinite(candidate))
    {
        return Fail(error, "Camera frustum schematic is not finite.");
    }

    model = candidate;
    return true;
}
