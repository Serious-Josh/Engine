#include "assets/ProjectAssetRename.h"
#include "assets/ProjectAssetDeletion.h"
#include "assets/ProjectAssetResolver.h"
#include "Core/Log.h"
#include "Editor.h"
#include "Debug/DebugUI.h"

#include "Math/Math.h"

#include "DemoContent/IDemoScene.h"
#include "DemoContent/SphereGridScene.h"
#include "DemoContent/StressTestScene.h"
#include "DemoContent/DemoSceneManager.h"
#include "DemoContent/FileBackedDemoScene.h"

#include "Project/ProjectLocator.h"
#include "Project/ProjectPaths.h"

#include "Renderer/Mesh.h"

#include "Scene/DirectionalLight.h"
#include "Scene/Scene.h"

#include "imgui.h"
#include <type_traits>
#include "imgui_internal.h"
#include "ImGuizmo.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace
{
    struct PickingRay
    {
        Vector3 origin;
        Vector3 direction;

        float maxDistance = 0.0f;
    };

    Vector3 TransformPoint(
        const Matrix4& matrix,
        const Vector3& point)
    {
        return {matrix.m[0][0] * point.x + matrix.m[0][1] * point.y + matrix.m[0][2] * point.z + matrix.m[0][3],
                matrix.m[1][0] * point.x + matrix.m[1][1] * point.y + matrix.m[1][2] * point.z + matrix.m[1][3],
                matrix.m[2][0] * point.x + matrix.m[2][1] * point.y + matrix.m[2][2] * point.z + matrix.m[2][3]};
    }

    Vector3 TransformDirection(
        const Matrix4& matrix,
        const Vector3& direction)
    {
        return {matrix.m[0][0] * direction.x + matrix.m[0][1] * direction.y + matrix.m[0][2] * direction.z,
                matrix.m[1][0] * direction.x + matrix.m[1][1] * direction.y + matrix.m[1][2] * direction.z,
                matrix.m[2][0] * direction.x + matrix.m[2][1] * direction.y + matrix.m[2][2] * direction.z};
    }

    bool UnprojectPoint(
        const Matrix4& inverseViewProjection,
        float x,
        float y,
        float z,
        Vector3& result)
    {
        const float w = inverseViewProjection.m[3][0] * x + inverseViewProjection.m[3][1] * y +
                        inverseViewProjection.m[3][2] * z + inverseViewProjection.m[3][3];

        if (!std::isfinite(w) || std::abs(w) < 0.000001f)
        {
            return false;
        }

        const float inverseW = 1.0f / w;

        result = {(inverseViewProjection.m[0][0] * x + inverseViewProjection.m[0][1] * y +
                   inverseViewProjection.m[0][2] * z + inverseViewProjection.m[0][3]) *
                      inverseW,
                  (inverseViewProjection.m[1][0] * x + inverseViewProjection.m[1][1] * y +
                   inverseViewProjection.m[1][2] * z + inverseViewProjection.m[1][3]) *
                      inverseW,
                  (inverseViewProjection.m[2][0] * x + inverseViewProjection.m[2][1] * y +
                   inverseViewProjection.m[2][2] * z + inverseViewProjection.m[2][3]) *
                      inverseW};

        return std::isfinite(result.x) && std::isfinite(result.y) && std::isfinite(result.z);
    }

    bool BuildPickingRay(
        const Camera& camera,
        float mouseX,
        float mouseY,
        float viewportWidth,
        float viewportHeight,
        PickingRay& ray)
    {
        if (viewportWidth <= 0.0f || viewportHeight <= 0.0f || !std::isfinite(mouseX) || !std::isfinite(mouseY))
        {
            return false;
        }

        const float x = 2.0f * mouseX / viewportWidth - 1.0f;
        const float y = 1.0f - 2.0f * mouseY / viewportHeight;

        const Matrix4 viewProjection = camera.GetProjectionMatrix() * camera.GetViewMatrix();
        const Matrix4 inverseViewProjection = viewProjection.Inverse();

        Vector3 nearPoint;
        Vector3 farPoint;

        if (!UnprojectPoint(inverseViewProjection, x, y, 0.0f, nearPoint) ||
            !UnprojectPoint(inverseViewProjection, x, y, 1.0f, farPoint))
        {
            return false;
        }

        const Vector3 difference = farPoint - nearPoint;
        const float distance = difference.Length();

        if (!std::isfinite(distance) || distance <= 0.000001f)
        {
            return false;
        }

        ray.origin = nearPoint;
        ray.direction = difference * (1.0f / distance);
        ray.maxDistance = distance;

        return true;
    }

    bool IntersectAABB(
        const Vector3& origin,
        const Vector3& direction,
        const MeshBounds& bounds,
        float maxDistance,
        float& hitDistance)
    {
        if (!bounds.valid)
        {
            return false;
        }

        float tMin = 0.0f;

        float tMax = maxDistance;

        const float origins[3] = {origin.x, origin.y, origin.z};

        const float directions[3] = {direction.x, direction.y, direction.z};
        const float minimums[3] = {bounds.min.x, bounds.min.y, bounds.min.z};
        const float maximums[3] = {bounds.max.x, bounds.max.y, bounds.max.z};

        for (int axis = 0; axis < 3; ++axis)
        {
            if (std::abs(directions[axis]) < 0.00000001f)
            {
                if (origins[axis] < minimums[axis] || origins[axis] > maximums[axis])
                {
                    return false;
                }

                continue;
            }

            const float inverseDirection = 1.0f / directions[axis];

            float t1 = (minimums[axis] - origins[axis]) * inverseDirection;

            float t2 = (maximums[axis] - origins[axis]) * inverseDirection;

            if (t1 > t2)
            {
                (std::swap)(t1, t2);
            }

            tMin = (std::max)(tMin, t1);

            tMax = (std::min)(tMax, t2);

            if (tMin > tMax)
            {
                return false;
            }
        }

        hitDistance = tMin;
        return true;
    }

    bool IntersectObject(
        const PickingRay& ray,
        const RenderObject& object,
        float& hitDistance)
    {
        if (!object.mesh || !object.mesh->GetBounds().valid)
        {
            return false;
        }

        // Reject singular transforms before calling Matrix4::Inverse(),
        // which otherwise silently returns identity.
        const Vector3& scale = object.transform.scale;

        if (!std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z) ||
            std::abs(scale.x * scale.y * scale.z) < 0.000001f)
        {
            return false;
        }

        const Matrix4 inverseModel = object.transform.GetModelMatrix().Inverse();

        const Vector3 localOrigin = TransformPoint(inverseModel, ray.origin);

        // Do not normalize: preserve the world-ray parameter t.

        const Vector3 localDirection = TransformDirection(inverseModel, ray.direction);

        return IntersectAABB(
            localOrigin,
            localDirection,
            object.mesh->GetBounds(), ray.maxDistance, hitDistance);
    }

    bool IsFinitePosition(const Vector3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    struct RaycastVector
    {
        double x, y, z;

        explicit RaycastVector(const Vector3& value) : x(value.x), y(value.y), z(value.z) {}
        RaycastVector(double xValue, double yValue, double zValue) : x(xValue), y(yValue), z(zValue) {}

        RaycastVector operator-(const RaycastVector& other) const
        {
            return {x - other.x, y - other.y, z - other.z};
        }

        RaycastVector Cross(const RaycastVector& other) const
        {
            return {y * other.z - z * other.y, z * other.x - x * other.z, x * other.y - y * other.x};
        }

        double Dot(const RaycastVector& other) const
        {
            return x * other.x + y * other.y + z * other.z;
        }
    };

    bool IntersectTriangle(const PickingRay& ray, const Vector3& a, const Vector3& b,
        const Vector3& c, double& nearestDistance)
    {
        if(!IsFinitePosition(a) || !IsFinitePosition(b) || !IsFinitePosition(c))
        {
            return false;
        }

        // Two-sided Moller-Trumbore intersection in double precision. Work in
        // world space so distance is comparable across nonuniform/mirrored scales.
        const RaycastVector edge1 = RaycastVector(b) - RaycastVector(a);
        const RaycastVector edge2 = RaycastVector(c) - RaycastVector(a);
        const RaycastVector direction(ray.direction);
        const auto cross = direction.Cross(edge2);
        const double determinant = edge1.Dot(cross);
        const double area = std::sqrt(edge1.Cross(edge2).Dot(edge1.Cross(edge2)));

        if(!std::isfinite(area) || area == 0 || !std::isfinite(determinant) ||
            std::abs(determinant) <= area * 1e-12)
        {
            return false;
        }

        const auto offset = RaycastVector(ray.origin) - RaycastVector(a);
        const double u = offset.Dot(cross) / determinant;

        if(!std::isfinite(u) || u < 0 || u > 1)
        {
            return false;
        }

        const auto offsetCross = offset.Cross(edge1);
        const double v = direction.Dot(offsetCross) / determinant;
        const double distance = edge2.Dot(offsetCross) / determinant;

        if(!std::isfinite(v) || v < 0 || u + v > 1 || !std::isfinite(distance) ||
            distance <= 0 || distance > nearestDistance)
        {
            return false;
        }

        nearestDistance = distance;
        return true;
    }

    bool RaycastObject(const PickingRay& ray, const RenderObject& object,
        const ImportedMesh& geometry, double& nearestDistance)
    {
        const Matrix4 model = object.transform.GetModelMatrix();

        for(const auto& row : model.m)
        {
            for(float value : row)
            {
                if(!std::isfinite(value))
                {
                    return false;
                }
            }
        }

        if(!object.mesh || !object.mesh->GetBounds().valid)
        {
            return false;
        }

        const auto& localBounds = object.mesh->GetBounds();
        MeshBounds worldBounds;

        // Transform all corners instead of inverting the model: broad phase
        // remains conservative for tiny, nonuniform, and mirrored scales.
        for(unsigned int corner = 0; corner < 8; ++corner)
        {
            const Vector3 local{corner & 1 ? localBounds.max.x : localBounds.min.x,
                corner & 2 ? localBounds.max.y : localBounds.min.y,
                corner & 4 ? localBounds.max.z : localBounds.min.z};
            const Vector3 world = TransformPoint(model, local);

            if(!IsFinitePosition(world))
            {
                return false;
            }

            if(!worldBounds.valid)
            {
                worldBounds.min = world;
                worldBounds.max = world;
                worldBounds.valid = true;
                continue;
            }

            worldBounds.min.x = (std::min)(worldBounds.min.x, world.x);
            worldBounds.min.y = (std::min)(worldBounds.min.y, world.y);
            worldBounds.min.z = (std::min)(worldBounds.min.z, world.z);

            worldBounds.max.x = (std::max)(worldBounds.max.x, world.x);
            worldBounds.max.y = (std::max)(worldBounds.max.y, world.y);
            worldBounds.max.z = (std::max)(worldBounds.max.z, world.z);
        }

        float boundsDistance = 0;

        if(!IntersectAABB(ray.origin, ray.direction, worldBounds, ray.maxDistance, boundsDistance) ||
            !std::isfinite(boundsDistance) ||
            boundsDistance > nearestDistance)
        {
            return false;
        }

        bool hit = false;

        for(size_t i = 0; i + 2 < geometry.indices.size(); i += 3)
        {
            const auto a = geometry.indices[i];
            const auto b = geometry.indices[i + 1];
            const auto c = geometry.indices[i + 2];

            if(a >= geometry.vertices.size() || b >= geometry.vertices.size() || c >= geometry.vertices.size())
            {
                continue;
            }

            const auto worldPosition = [&](uint32_t index)
            {
                const auto& position = geometry.vertices[index].position;
                return TransformPoint(model, {position[0], position[1], position[2]});
            };

            if(IntersectTriangle(ray, worldPosition(a), worldPosition(b), worldPosition(c), nearestDistance))
            {
                hit = true;
            }
        }

        return hit;
    }

    bool IntersectPlacementPlane(const PickingRay& ray, const Vector3& point, const Vector3& normal,
        double& distance)
    {
        const double denominator = RaycastVector(normal).Dot(RaycastVector(ray.direction));

        if(!std::isfinite(denominator) || std::abs(denominator) < 1e-8)
        {
            return false;
        }

        distance = (RaycastVector(point) - RaycastVector(ray.origin)).Dot(RaycastVector(normal)) / denominator;
        return std::isfinite(distance) && distance > 0 && distance <= ray.maxDistance;
    }

    bool FindMeshDropPosition(const FileBackedDemoScene& owner, float mouseX, float mouseY,
        float width, float height, Vector3& position)
    {
        const auto& scene = owner.GetScene();
        const auto& camera = scene.GetCamera();
        const Vector3 forward = (camera.GetTarget() - camera.GetPosition()).Normalized();

        if(!IsFinitePosition(forward) || forward.Length() < 0.5f ||
            Vector3::Cross({0, 1, 0}, forward).Length() < 1e-6f)
        {
            return false;
        }

        PickingRay ray;

        if(!BuildPickingRay(camera, mouseX, mouseY, width, height, ray))
        {
            return false;
        }

        double distance = ray.maxDistance;
        bool hit = false;

        for(const auto& object : scene.GetRenderObjects())
        {
            const auto* geometry = object.mesh ? owner.GetMeshGeometry(*object.mesh) : nullptr;

            if(geometry && RaycastObject(ray, object, *geometry, distance))
            {
                hit = true;
            }
        }

        // Camera::GetViewMatrix establishes +Y up. Ground is the XZ plane at
        // y=0. Last fallback is perpendicular to view, 10 engine units ahead.
        if(!hit && !IntersectPlacementPlane(ray, {}, {0, 1, 0}, distance) &&
            !IntersectPlacementPlane(ray, camera.GetPosition() + forward * 10.0f, forward, distance))
        {
            return false;
        }

        position = {static_cast<float>(ray.origin.x + double(ray.direction.x) * distance),
            static_cast<float>(ray.origin.y + double(ray.direction.y) * distance),
            static_cast<float>(ray.origin.z + double(ray.direction.z) * distance)};
        return IsFinitePosition(position) &&
            RaycastVector(position - camera.GetPosition()).Dot(RaycastVector(forward)) > 0;
    }

    size_t PickObject(
        const Scene& scene,
        float mouseX,
        float mouseY,
        float viewportWidth,
        float viewportHeight)
    {
        PickingRay ray{};

        if (!BuildPickingRay(scene.GetCamera(), mouseX, mouseY, viewportWidth, viewportHeight, ray))
        {
            return (std::numeric_limits<size_t>::max)();
        }

        const auto& objects = scene.GetRenderObjects();

        size_t closestObject = (std::numeric_limits<size_t>::max)();

        float closestDistance = ray.maxDistance;

        for (size_t i = 0; i < objects.size(); ++i)
        {
            float hitDistance = 0.0f;

            if (IntersectObject(ray, objects[i], hitDistance) && hitDistance <= closestDistance)
            {
                closestDistance = hitDistance;
                closestObject = i;
            }
        }

        return closestObject;
    }
}

bool Editor::RequestApplicationClose()
{
    if(m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending || m_fileRequest)
    {
        return false;
    }

    if(!m_inspectorPanel.HasUnsavedChanges())
    {
        return true;
    }

    m_materialLeaveAction = MaterialLeaveAction::Close;
    return false;
}

bool Editor::TakeApplicationCloseApproval()
{
    return std::exchange(m_applicationCloseApproved, false);
}

std::optional<Editor::FileRequest> Editor::TakeFileRequest()
{
    if(m_fileRequest && m_fileRequest->type != FileRequestType::Save && m_inspectorPanel.HasUnsavedChanges())
    {
        if(m_materialLeaveAction == MaterialLeaveAction::None)
        {
            m_materialLeaveAction = MaterialLeaveAction::Scene;
        }

        return std::nullopt;
    }

    if(m_materialLeaveAction != MaterialLeaveAction::None)
    {
        return std::nullopt;
    }

    return std::exchange(
        m_fileRequest,
        std::nullopt);
}

std::optional<AssetBrowser::MaterialCreationRequest> Editor::TakeMaterialCreationRequest()
{
    return m_assetBrowser.TakeMaterialCreationRequest();
}

void Editor::OnMaterialCreationCompleted(
    const std::filesystem::path& relativePath,
    const std::string& error)
{
    m_assetBrowser.OnMaterialCreationCompleted(relativePath, error);
}

bool Editor::HasFileRequest(FileRequestType type) const noexcept
{
    return m_fileRequest && m_fileRequest->type == type;
}

void Editor::DrawMainMenu(
    const IDemoScene& demoScene,
    DemoSceneManager& sceneManager,
    DebugUI& debugUI)
{
    if (!ImGui::BeginMainMenuBar())
    {
        return;
    }

    if (ImGui::BeginMenu("File"))
    {
        ImGui::BeginDisabled(m_sceneOperationPending || m_fileRequest.has_value() || m_materialLeaveAction != MaterialLeaveAction::None);

        if (ImGui::MenuItem("New"))
        {
            m_fileRequest = FileRequest{FileRequestType::New, {}};
        }

        const bool fileBacked = dynamic_cast<const FileBackedDemoScene*>(&demoScene) != nullptr;
        ImGui::BeginDisabled(!fileBacked);

        if (ImGui::MenuItem("Save", "Ctrl+S"))
        {
            m_fileRequest = FileRequest{FileRequestType::Save, {}};
        }

        ImGui::EndDisabled();

        if (ImGui::BeginMenu("Load"))
        {
            const ProjectPaths paths(ProjectLocator::GetDefaultProjectRoot());
            std::vector<std::filesystem::path> scenes;
            std::error_code error;
            std::filesystem::directory_iterator iterator(
                paths.GetScenesDirectory(),
                error);
            const std::filesystem::directory_iterator end;

            while (!error && iterator != end)
            {
                std::error_code entryError;

                if (iterator->is_regular_file(entryError) && !entryError && iterator->path().extension() == ".scene")
                {
                    scenes.push_back(iterator->path());
                }

                iterator.increment(error);
            }

            std::sort(scenes.begin(), scenes.end(), [](const auto& left, const auto& right)
            {
                return left.filename().native() < right.filename().native();
            });

            for (const auto& scene : scenes)
            {
                const std::string filename = scene.filename().string();

                if (ImGui::MenuItem(filename.c_str()))
                {
                    m_fileRequest = FileRequest{FileRequestType::Load, scene};
                }
            }

            if (error)
            {
                ImGui::TextDisabled("Scenes directory unavailable or incomplete");
            }
            else if (scenes.empty())
            {
                ImGui::TextDisabled("No scene files found");
            }

            ImGui::EndMenu();
        }

        ImGui::EndDisabled();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Editor"))
    {
        ImGui::MenuItem("Scene Inspector", nullptr, &m_showEditorPanel);
        ImGui::BeginDisabled(m_materialLeaveAction != MaterialLeaveAction::None);

        if(ImGui::MenuItem("Inspector", nullptr, m_workspace.InspectorVisible()))
        {
            if(m_workspace.InspectorVisible() && m_inspectorPanel.HasUnsavedChanges())
            {
                m_materialLeaveAction = MaterialLeaveAction::Hide;
            }
            else
            {
                m_workspace.InspectorVisible() = !m_workspace.InspectorVisible();
            }
        }

        ImGui::EndDisabled();
        ImGui::MenuItem("Asset Browser", nullptr, &m_workspace.AssetBrowserVisible());
        ImGui::Separator();

        const std::string_view activeSceneName = sceneManager.GetActiveSceneName();

        ImGui::BeginDisabled(m_sceneOperationPending || m_fileRequest.has_value() || m_materialLeaveAction != MaterialLeaveAction::None);

        if (ImGui::BeginCombo("Demo Scene", activeSceneName.data()))
        {
            for (const DemoSceneEntry& entry : sceneManager.GetAvailableScenes())
            {
                const bool isSelected = entry.name == activeSceneName;

                if (ImGui::Selectable(entry.name.c_str(), isSelected))
                {
                    m_fileRequest = FileRequest{FileRequestType::Demo, {}, entry.name};
                }

                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }

            ImGui::EndCombo();
        }

        ImGui::EndDisabled();

        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Debug"))
    {
        debugUI.DrawMenu();
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

void Editor::DrawSceneViewport(
    std::uint64_t sceneTextureHandle,
    unsigned int sceneWidth,
    unsigned int sceneHeight)
{
    m_viewportImage = {};
    m_viewportHovered = false;
    const auto& rectangle = m_workspace.GetSceneViewport();

    if(rectangle.size.x <= 0.0f || rectangle.size.y <= 0.0f)
    {
        return;
    }

    ImGui::SetNextWindowPos(rectangle.position, ImGuiCond_Always);
    ImGui::SetNextWindowSize(rectangle.size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing;

    if(ImGui::Begin("Scene Viewport", nullptr, flags))
    {
        EditorWorkspace::KeepCurrentWindowBehindFloatingUI();

        if(sceneTextureHandle != 0 && sceneWidth != 0 && sceneHeight != 0)
        {
            const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
            const ImVec2 logicalSize(static_cast<float>(sceneWidth) / framebufferScale.x,
                                     static_cast<float>(sceneHeight) / framebufferScale.y);
            const float scale = (std::min)(rectangle.size.x / logicalSize.x, rectangle.size.y / logicalSize.y);
            const ImVec2 size(logicalSize.x * scale, logicalSize.y * scale);
            const ImVec2 position(rectangle.position.x + (rectangle.size.x - size.x) * 0.5f,
                                  rectangle.position.y + (rectangle.size.y - size.y) * 0.5f);
            m_viewportImage = {position, size};
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const bool inside = mouse.x >= position.x && mouse.y >= position.y &&
                mouse.x < position.x + size.x && mouse.y < position.y + size.y;
            m_viewportHovered = inside && !m_workspace.IsSplitterDragging() &&
                !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) &&
                !ImGui::IsAnyItemActive() && !ImGui::GetDragDropPayload() &&
                !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);

            ImGui::GetWindowDrawList()->AddImage(
                ImTextureRef(static_cast<ImTextureID>(sceneTextureHandle)),
                position,
                ImVec2(position.x + size.x, position.y + size.y));

            if(!m_meshCreationStatus.empty())
            {
                ImGui::GetWindowDrawList()->AddText(ImVec2(position.x + 12.0f, position.y + 12.0f),
                    IM_COL32(240, 220, 170, 255), m_meshCreationStatus.c_str());
            }

            if(!m_sceneObjectDeletionStatus.empty())
            {
                const float offset = m_meshCreationStatus.empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing();
                ImGui::GetWindowDrawList()->AddText(ImVec2(position.x + 12.0f, position.y + 12.0f + offset),
                    IM_COL32(240, 220, 170, 255), m_sceneObjectDeletionStatus.c_str());
            }

            if(!m_sceneHistoryStatus.empty())
            {
                const int lines = static_cast<int>(!m_meshCreationStatus.empty()) +
                    static_cast<int>(!m_sceneObjectDeletionStatus.empty());
                ImGui::GetWindowDrawList()->AddText(ImVec2(position.x + 12.0f,
                    position.y + 12.0f + lines * ImGui::GetTextLineHeightWithSpacing()),
                    IM_COL32(240, 220, 170, 255), m_sceneHistoryStatus.c_str());
            }
        }
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

const EditorWorkspaceRect& Editor::PrepareWorkspace(
    IDemoScene& demoScene,
    DemoSceneManager& sceneManager,
    DebugUI& debugUI)
{
    DrawMainMenu(demoScene, sceneManager, debugUI);
    m_workspace.Update();
    return m_workspace.GetSceneViewport();
}

void Editor::ActivateObjectInspection(
    IDemoScene& demoScene, const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    m_objectInspectionIdentity.reset();
    m_inspectorTarget = InspectorTarget::None;
    m_inspectorPanel.LoadSelection({}, paths, registry);
    const auto& scene = static_cast<const IDemoScene&>(demoScene).GetScene();

    if(m_selectedObject >= scene.GetRenderObjects().size())
    {
        return;
    }

    ObjectInspectionIdentity identity{&scene, scene.GetStructureRevision()};

    if(const auto* fileScene = dynamic_cast<const FileBackedDemoScene*>(&demoScene))
    {
        const auto metadata = fileScene->GetObjectMetadata(m_selectedObject);

        if(!metadata)
        {
            return;
        }

        identity.runtimeIdentity = metadata->runtimeIdentity;
        identity.objectID = metadata->objectID;
        identity.sceneID = metadata->sceneID;
    }

    m_objectInspectionIdentity = std::move(identity);
    m_inspectorTarget = InspectorTarget::SceneObject;
}

std::optional<SceneObjectInspectorView> Editor::ResolveObjectInspection(
    const IDemoScene& demoScene, const ProjectPaths& paths, const ProjectAssetRegistry& registry) const
{
    const auto& scene = demoScene.GetScene();
    const auto& objects = scene.GetRenderObjects();

    if(!m_objectInspectionIdentity || m_objectInspectionIdentity->scene != &scene ||
       m_objectInspectionIdentity->revision != scene.GetStructureRevision() || m_selectedObject >= objects.size())
    {
        return std::nullopt;
    }

    const auto& object = objects[m_selectedObject];
    SceneObjectInspectorView view{"Object " + std::to_string(m_selectedObject), object.transform,
        object.mesh ? "Runtime mesh (document reference unavailable)" : "No mesh",
        "Runtime material (document reference unavailable)"};
    const auto* fileScene = dynamic_cast<const FileBackedDemoScene*>(&demoScene);

    if(object.mesh)
    {
        for(const auto& slot : object.mesh->GetMetadata().materialSlots)
        {
            view.materialSlots.push_back({slot.id, slot.name, "Runtime material", {}, false,
                object.GetSlotMaterial(slot.id) != nullptr});
        }
    }

    if(!fileScene)
    {
        return view;
    }

    const auto metadata = fileScene->GetObjectMetadata(m_selectedObject);

    if(!metadata || metadata->runtimeIdentity != m_objectInspectionIdentity->runtimeIdentity ||
       metadata->structureRevision != m_objectInspectionIdentity->revision ||
       metadata->objectID != m_objectInspectionIdentity->objectID || metadata->sceneID != m_objectInspectionIdentity->sceneID)
    {
        return std::nullopt;
    }

    const auto label = [&](const std::string& reference, AssetType type) -> std::string
    {
        if(reference.empty())
        {
            return type == AssetType::Mesh ? "No mesh" : "Default Material";
        }

        if(reference == "builtin:sphere" && type == AssetType::Mesh)
        {
            return "Sphere";
        }

        if(reference == "builtin:default" && type == AssetType::Material)
        {
            return "Default Material";
        }

        const auto id = AssetID::ParseReference(reference);
        const auto* record = id ? registry.Find(*id) : nullptr;

        if(!record || record->type != type)
        {
            return type == AssetType::Mesh ? "Unavailable mesh reference" : "Unavailable material reference";
        }

        const auto path = record->relativePath.generic_string();
        return ProjectAssetResolver::Resolve(paths, registry, *id).status == AssetAvailability::Available ?
            record->relativePath.stem().generic_string() : "Unavailable: " + path;
    };

    view.metadata = metadata;
    view.name = metadata->name;
    view.mesh = label(metadata->meshReference, AssetType::Mesh);
    view.material = label(metadata->materialReference, AssetType::Material);
    view.meshReference = metadata->meshReference;

    for(auto& slot : view.materialSlots)
    {
        const auto binding = metadata->materialSlotBindings.find(slot.id);
        slot.explicitBinding = binding != metadata->materialSlotBindings.end();
        slot.materialReference = slot.explicitBinding ? binding->second : metadata->materialReference;
        slot.materialName = label(slot.materialReference, AssetType::Material);
    }

    return view;
}

void Editor::Draw(
    IDemoScene& demoScene,
    DemoSceneManager& sceneManager,
    DebugUI& debugUI,
    std::uint64_t sceneTextureHandle,
    unsigned int sceneWidth,
    unsigned int sceneHeight,
    float deltaTime,
    const ProjectPaths& projectPaths,
    const ProjectAssetRegistry& assetRegistry,
    const std::optional<MaterialPreviewImage>& materialPreview)
{

    Scene& scene = demoScene.GetScene();
    const std::string activeSceneName(sceneManager.GetActiveSceneName());
    const auto* dragScene = dynamic_cast<const FileBackedDemoScene*>(&demoScene);
    const auto dragRuntimeIdentity = dragScene ? dragScene->GetRuntimeIdentity() : 0;

    if(dragRuntimeIdentity != m_meshDragRuntimeIdentity)
    {
        m_assetBrowser.InvalidateMeshDragIdentity();
        m_meshDragRuntimeIdentity = dragRuntimeIdentity;
    }

    UpdateSceneHistorySession(demoScene, projectPaths, activeSceneName);

    const uint64_t structureRevision = scene.GetStructureRevision();

    if (m_selectionSceneName != activeSceneName || m_selectionStructureRevision != structureRevision)
    {
        ResetTransformGestures();

        m_selectedObject = InvalidObjectSelection;
        m_selectionSceneName = activeSceneName;
        m_selectionStructureRevision = structureRevision;
    }

    const Scene& constScene = scene;
    const auto& objects = constScene.GetRenderObjects();

    if (m_selectedObject >= objects.size())
    {
        m_selectedObject = InvalidObjectSelection;
    }

    (void)debugUI;
    m_assetBrowser.SetThumbnailProject(projectPaths, assetRegistry);
    ImGui::BeginDisabled(m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending);

    if(m_workspace.AssetBrowserVisible())
    {
        m_assetBrowser.Draw(m_workspace.GetAssetBrowser());
    }
    else
    {
        m_assetBrowser.PauseMaterialThumbnails();
    }

    ImGui::EndDisabled();

    const bool assetSelectionIntent = m_assetBrowser.TakeSelectionIntent();

    const bool assetTargetChanged = m_inspectorTarget != InspectorTarget::Asset ||
        m_assetBrowser.GetSelection() != m_inspectedAssetSelection;

    if(m_materialLeaveAction == MaterialLeaveAction::None && assetTargetChanged &&
       (assetSelectionIntent || (m_inspectorTarget == InspectorTarget::Asset &&
        m_assetBrowser.GetSelection() != m_inspectedAssetSelection)))
    {
        if(m_inspectorPanel.HasUnsavedChanges())
        {
            m_pendingAssetSelection = m_assetBrowser.GetSelection();
            m_materialLeaveAction = MaterialLeaveAction::Selection;
        }
        else
        {
            m_inspectedAssetSelection = m_assetBrowser.GetSelection();
            m_inspectorPanel.LoadSelection(m_inspectedAssetSelection, projectPaths, assetRegistry);
            m_inspectorTarget = m_inspectedAssetSelection.empty() ? InspectorTarget::None : InspectorTarget::Asset;
            m_objectInspectionIdentity.reset();
        }

        m_objectInspectionRequested = false;
    }

    if(m_objectInspectionRequested && m_materialLeaveAction == MaterialLeaveAction::None)
    {
        m_objectInspectionRequested = false;

        if(m_inspectorPanel.HasUnsavedChanges())
        {
            m_materialLeaveAction = MaterialLeaveAction::ObjectSelection;
        }
        else
        {
            ActivateObjectInspection(demoScene, projectPaths, assetRegistry);
        }
    }

    const auto objectView = m_inspectorTarget == InspectorTarget::SceneObject ?
        ResolveObjectInspection(demoScene, projectPaths, assetRegistry) : std::nullopt;

    if(m_inspectorTarget == InspectorTarget::SceneObject && !objectView)
    {
        m_inspectorTarget = InspectorTarget::None;
        m_objectInspectionIdentity.reset();
    }

    // A disappearing control may never receive IsItemDeactivatedAfterEdit. Finalize
    // against its original object, never against the newly selected object.
    if(m_transformFieldEditing && (m_transformFieldUndoObject != m_selectedObject ||
       m_transformFieldScene != &scene || m_transformFieldRevision != scene.GetStructureRevision() ||
       !objectView || !m_workspace.InspectorVisible() || m_materialLeaveAction != MaterialLeaveAction::None ||
       m_sceneOperationPending))
    {
        FinishTransformFieldEdit(demoScene);
    }

    const bool disableInspector = m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending;

    // This ImGui version's color picker restores alpha whenever a disabled scope exists,
    // including BeginDisabled(false), whose alpha backup may never have been initialized.
    if(disableInspector)
    {
        ImGui::BeginDisabled();
    }

    if(m_materialAssignmentSelection && (!objectView || !objectView->metadata ||
       objectView->metadata->runtimeIdentity != m_materialAssignmentSelection->runtimeIdentity ||
       objectView->metadata->objectID != m_materialAssignmentSelection->objectID ||
       objectView->metadata->structureRevision != m_materialAssignmentSelection->structureRevision))
    {
        m_materialAssignmentStatus.clear();
    }

    if(objectView || !m_workspace.InspectorVisible() || disableInspector)
    {
        m_inspectorPanel.DismissTexturePicker();
    }

    if(m_workspace.InspectorVisible())
    {
        if(objectView)
        {
            m_inspectorPanel.DrawSceneObject(m_workspace.GetInspector(), *objectView, [this, &demoScene]()
            {
                DrawSelectedObjectTransformControls(demoScene);
            }, projectPaths, assetRegistry, m_materialAssignmentPending, m_materialAssignmentStatus);

            if(!m_materialAssignmentPending && !m_inspectorPanel.GetSlotPickerStatus().empty())
            {
                m_materialAssignmentSelection = objectView->metadata;
                m_materialAssignmentStatus = m_inspectorPanel.GetSlotPickerStatus();
            }

            if(auto request = m_inspectorPanel.TakeSlotAssignmentRequest())
            {
                m_materialAssignmentSelection = request->selection;
                m_materialAssignmentSlotID = request->slotID;
                m_materialAssignmentRequest = MaterialAssignmentRequest{
                    request->selection, {}, request->slotID, request->materialID};

                m_materialAssignmentPending = true;
                m_materialAssignmentStatus = "Pending material-slot assignment.";
            }
        }
        else
        {
            m_inspectorPanel.Draw(
                m_workspace.GetInspector(), projectPaths, assetRegistry,
                materialPreview,
                m_materialLeaveAction != MaterialLeaveAction::None);
        }
    }

    if(disableInspector)
    {
        ImGui::EndDisabled();
    }

    if(!objectView || !m_workspace.InspectorVisible())
    {
        m_inspectorPanel.ReleaseSceneObjectPreview();
        m_inspectorPanel.DismissSceneMaterialPicker();
    }

    if(m_inspectorPanel.TakeRefreshRequest() && m_materialLeaveAction == MaterialLeaveAction::None &&
       m_inspectorTarget == InspectorTarget::Asset)
    {
        if(m_inspectorPanel.HasUnsavedChanges())
        {
            m_materialLeaveAction = MaterialLeaveAction::Refresh;
        }
        else
        {
            m_inspectorPanel.LoadSelection(m_inspectedAssetSelection, projectPaths, assetRegistry);
        }
    }

    if(m_materialLeaveAction != MaterialLeaveAction::None)
    {
        const auto decision = m_inspectorPanel.DrawUnsavedConfirmation(projectPaths, assetRegistry);

        if(decision == InspectorPanel::LeaveDecision::Proceed)
        {
            switch(m_materialLeaveAction)
            {
            case MaterialLeaveAction::Selection:
                m_inspectedAssetSelection = m_pendingAssetSelection;
                m_inspectorPanel.LoadSelection(m_inspectedAssetSelection, projectPaths, assetRegistry);
                m_inspectorTarget = m_inspectedAssetSelection.empty() ? InspectorTarget::None : InspectorTarget::Asset;
                m_objectInspectionIdentity.reset();
                break;
            case MaterialLeaveAction::ObjectSelection:
                ActivateObjectInspection(demoScene, projectPaths, assetRegistry);
                break;
            case MaterialLeaveAction::Refresh:
                m_inspectorPanel.LoadSelection(m_inspectedAssetSelection, projectPaths, assetRegistry);
                break;
            case MaterialLeaveAction::Hide:
                m_workspace.InspectorVisible() = false;
                break;
            case MaterialLeaveAction::Close:
                m_applicationCloseApproved = true;
                break;
            default:
                break;
            }

            m_materialLeaveAction = MaterialLeaveAction::None;
        }
        else if(decision == InspectorPanel::LeaveDecision::Cancel)
        {
            if(m_materialLeaveAction == MaterialLeaveAction::Selection)
            {
                m_assetBrowser.RestoreSelection(m_inspectedAssetSelection);
            }

            if(m_materialLeaveAction == MaterialLeaveAction::Scene)
            {
                m_fileRequest.reset();
            }

            m_materialLeaveAction = MaterialLeaveAction::None;
        }
    }

    for(auto& working : m_inspectorPanel.TakeWorkingMaterialRevisions(
        m_workspace.InspectorVisible() && m_inspectorTarget == InspectorTarget::Asset))
    {
        const auto* fileScene = dynamic_cast<const FileBackedDemoScene*>(&demoScene);
        const auto versions = fileScene ? fileScene->GetMaterialVersions(working.projectIdentity, working.assetID) : std::nullopt;

        if(versions && !versions->versions.empty())
        {
            m_workingMaterialRevisions.push_back({std::move(working), *versions});
        }
    }

    for(auto& saved : m_inspectorPanel.TakeSavedMaterialRevisions())
    {
        // Bind the owned receipt to this scene instance, never to an object selection.
        const auto* fileScene = dynamic_cast<const FileBackedDemoScene*>(&demoScene);

        if(fileScene)
        {
            const auto versions = fileScene->GetMaterialVersions(saved.projectIdentity, saved.assetID);

            if(versions && !versions->versions.empty())
            {
                m_savedMaterialRevisions.push_back({std::move(saved), versions->catalogIdentity,
                    fileScene->GetRuntimeIdentity(), scene.GetStructureRevision()});
            }
        }
    }

    if(const auto savedSource = m_inspectorPanel.TakeSavedMaterialSource())
    {
        m_assetBrowser.InvalidateMaterialSource(*savedSource);
    }

    m_assetBrowser.DrawRenameModal(m_inspectorPanel.HasUnsavedChanges() ||
        m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending);

    m_assetBrowser.DrawDeletionConfirmation(m_inspectorPanel.HasUnsavedChanges() ||
        m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending);

    DrawSceneViewport(sceneTextureHandle, sceneWidth, sceneHeight);
    DrawMeshDropTarget(demoScene, projectPaths, assetRegistry);

    const ImGuiIO& io = ImGui::GetIO();

    if(ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        m_viewportFocused = m_viewportHovered;
    }

    if(io.AppFocusLost || m_sceneOperationPending || m_viewportImage.size.x <= 0.0f || m_workspace.IsSplitterDragging())
    {
        m_viewportFocused = false;
        m_cameraLooking = false;
    }

    if(m_viewportFocused && !io.WantTextInput && !ImGuizmo::IsUsing() && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
    {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false))
        {
            m_gizmoMode = GizmoMode::Translate;
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_E, false))
        {
            m_gizmoMode = GizmoMode::Rotate;
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_R, false))
        {
            m_gizmoMode = GizmoMode::Scale;
        }
    }

    if (m_showEditorPanel)
    {
        if (ImGui::Begin("Scene Inspector", &m_showEditorPanel))
        {
            if (auto* sphereGridScene = dynamic_cast<SphereGridScene*>(&demoScene))
            {
                DrawSphereGridControls(*sphereGridScene);

                ImGui::Separator();
            }

            if (auto* stressTestScene = dynamic_cast<StressTestScene*>(&demoScene))
            {
                DrawStressTestControls(*stressTestScene);

                ImGui::Separator();
            }

            DrawObjectControls(demoScene);

            ImGui::Separator();

            DrawCamera(demoScene);

            ImGui::Separator();

            DrawDirectionalLight(demoScene);

        }

        ImGui::End();
    }

    // Save is an application shortcut even when a property input owns keyboard
    // focus. Do not gate it on WantCaptureKeyboard or consume text-edit input.

    if (!m_sceneOperationPending && !m_fileRequest && m_materialLeaveAction == MaterialLeaveAction::None &&
        dynamic_cast<const FileBackedDemoScene*>(&demoScene))
    {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            m_fileRequest = FileRequest{FileRequestType::Save, {}};
        }
    }

    // Projection is driven by the actual render extent, including after Inspector controls.
    scene.GetCamera().SetAspectRatio(static_cast<float>(sceneWidth) / static_cast<float>(sceneHeight));

    HandleTransformUndo(demoScene);
    HandleTransformRedo(demoScene);

    UpdateCameraLook(demoScene, m_viewportImage.size.x, m_viewportImage.size.y);
    UpdateCameraMovement(demoScene, deltaTime);
    DrawTransformGizmo(demoScene, m_viewportImage.size.x, m_viewportImage.size.y);
    HandleViewportSelection(demoScene, m_viewportImage.size.x, m_viewportImage.size.y);
    DrawSceneObjectDeletionControls(demoScene);
}

void Editor::DrawSphereGridControls(SphereGridScene& demoScene)
{
    if (!ImGui::CollapsingHeader("Diagnostics", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    const char* modeNames[] = {"None", "Rotate Light", "Move Sphere", "Orbit Camera"};

    int selectedMode = static_cast<int>(demoScene.GetUpdateMode());

    if (ImGui::Combo("Update Mode", &selectedMode, modeNames, IM_ARRAYSIZE(modeNames)))
    {
        demoScene.SetUpdateMode(static_cast<DemoUpdateMode>(selectedMode));
    }

    DemoUpdateMode mode = demoScene.GetUpdateMode();

    if (mode == DemoUpdateMode::RotateLight)
    {
        float speed = demoScene.GetLightRotationSpeed();

        if (ImGui::DragFloat("Rotation Speed", &speed, 0.05f, 0.0f, 10.0f, "%.2f"))
        {
            demoScene.SetLightRotationSpeed(speed);
        }

        float verticalAngleDegrees = demoScene.GetLightVerticalAngle() * (180.0f / 3.14159265358979323846f);

        if (ImGui::SliderFloat("Light Angle", &verticalAngleDegrees, 0.0f, 90.0f, "%.1f deg"))
        {
            demoScene.SetLightVerticalAngle(ToRadians(verticalAngleDegrees));
        }
    }
    else if (mode == DemoUpdateMode::OrbitCamera)
    {
        float speed = demoScene.GetCameraOrbitSpeed();

        if (ImGui::DragFloat("Orbit Speed", &speed, 0.05f, 0.0f, 10.0f, "%.2f"))
        {
            demoScene.SetCameraOrbitSpeed(speed);
        }
    }
}

void Editor::DrawStressTestControls(StressTestScene& demoScene)
{
    if (!ImGui::CollapsingHeader("Stress Test", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    constexpr unsigned int gridSizes[] = {10, 20, 30, 40, 50};

    constexpr const char* gridLabels[] = {"10 x 10", "20 x 20", "30 x 30", "40 x 40", "50 x 50"};

    const unsigned int currentGridSize = demoScene.GetGridSize();

    int selectedIndex = 0;

    for (int i = 0; i < IM_ARRAYSIZE(gridSizes); ++i)
    {
        if (gridSizes[i] == currentGridSize)
        {
            selectedIndex = i;
            break;
        }
    }

    if (ImGui::Combo("Grid Size", &selectedIndex, gridLabels, IM_ARRAYSIZE(gridLabels)))
    {
        demoScene.SetGridSize(gridSizes[selectedIndex]);
    }

    ImGui::Text("Sphere instances: %u", currentGridSize * currentGridSize);
}

void Editor::DrawDirectionalLight(IDemoScene& demoScene)
{
    if (!ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    DirectionalLight& light = demoScene.GetScene().GetDirectionalLight();

    ImGui::DragFloat3("Direction", &light.direction.x, 0.01f, -1.0f, 1.0f);

    ImGui::DragFloat("Intensity", &light.intensity, 0.05f, 0.0f, 100.0f);

    ImGui::DragFloat("Ambient Strength", &light.ambientStrength, 0.01f, 0.0f, 1.0f);

    float angularRadiusDegrees = light.angularRadius * (180.0f / 3.14159265358979323846f);

    if (ImGui::DragFloat("Angular Radius", &angularRadiusDegrees, 0.01f, 0.0f, 90.0f, "%.2f deg"))
    {
        light.angularRadius = ToRadians(angularRadiusDegrees);
    }

    if (ImGui::Button("Default##DirectionalLight"))
    {
        demoScene.ResetDirectionalLight();
    }
}

void Editor::DrawObjectControls(
    IDemoScene& demoScene)
{
    Scene& scene = demoScene.GetScene();

    const auto& objects = scene.GetRenderObjects();

    if (!ImGui::CollapsingHeader("Objects", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    const std::string preview =
        m_selectedObject == InvalidObjectSelection ? "None" : "Object " + std::to_string(m_selectedObject);

    if (ImGui::BeginCombo("Selected Object", preview.c_str()))
    {
        const bool noneSelected = m_selectedObject == InvalidObjectSelection;

        if (ImGui::Selectable("None", noneSelected))
        {
            m_selectedObject = InvalidObjectSelection;
            m_objectInspectionRequested = true;
        }

        for (size_t i = 0; i < objects.size(); ++i)
        {
            const std::string label = "Object " + std::to_string(i);

            const bool selected = m_selectedObject == i;

            if (ImGui::Selectable(label.c_str(), selected))
            {
                m_selectedObject = i;
                m_objectInspectionRequested = true;
            }

            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    ImGui::Text("Objects: %zu", objects.size());

}

void Editor::DrawSelectedObjectTransformControls(IDemoScene& demoScene)
{
    Scene& scene = demoScene.GetScene();
    const auto& objects = static_cast<const Scene&>(scene).GetRenderObjects();

    if(m_selectedObject >= objects.size())
    {
        return;
    }

    Transform transform = objects[m_selectedObject].transform;

    bool transformChanged = false;

    const Transform originalTransform = transform;

    bool fieldActivated = false;

    bool fieldDeactivated = false;

    ImGui::TextUnformatted("Transform");
    ImGui::PushID(&scene);
    ImGui::PushID(static_cast<int>(m_selectedObject));
    ImGui::PushID("SelectedObjectTransform");

    transformChanged |= ImGui::DragFloat3("Position", &transform.position.x, 0.05f);

    fieldActivated |= ImGui::IsItemActivated();
    fieldDeactivated |= ImGui::IsItemDeactivatedAfterEdit();

    float rotationDegrees[3] = {ToDegrees(transform.rotation.x), ToDegrees(transform.rotation.y),
                                ToDegrees(transform.rotation.z)};

    if(ImGui::DragFloat3("Rotation", rotationDegrees, 0.05))
    {
        transform.rotation =
            Vector3{ToRadians(rotationDegrees[0]), ToRadians(rotationDegrees[1]), ToRadians(rotationDegrees[2])};

        transformChanged = true;
    }

    fieldActivated |= ImGui::IsItemActivated();
    fieldDeactivated |= ImGui::IsItemDeactivatedAfterEdit();

    if(ImGui::DragFloat3("Scale", &transform.scale.x, 0.01f, 0.01f, 100.0f))
    {
        transform.scale.x = (std::max)(0.01f, transform.scale.x);
        transform.scale.y = (std::max)(0.01f, transform.scale.y);
        transform.scale.z = (std::max)(0.01f, transform.scale.z);

        transformChanged = true;
    }

    fieldActivated |= ImGui::IsItemActivated();
    fieldDeactivated |= ImGui::IsItemDeactivatedAfterEdit();

    if(fieldActivated && !m_transformFieldEditing)
    {
        m_transformFieldEditing = true;
        m_transformFieldUndoObject = m_selectedObject;
        m_transformFieldUndoStart = originalTransform;
        m_transformFieldScene = &scene;

        m_transformFieldRevision = scene.GetStructureRevision();
        m_fieldHistoryTarget = CaptureHistoryTarget(demoScene, m_selectedObject);
        m_fieldHistoryEpoch = m_historyEpoch;
    }

    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();

    const auto finite = [](const Vector3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };

    if(transformChanged && finite(transform.position) && finite(transform.rotation) && finite(transform.scale))
    {
        scene.SetRenderObjectTransform(m_selectedObject, transform);
    }

    if(fieldDeactivated && m_transformFieldEditing)
    {
        FinishTransformFieldEdit(demoScene);
    }
}

void Editor::FinishTransformFieldEdit(IDemoScene& demoScene)
{
    Scene& scene = demoScene.GetScene();

    if(m_transformFieldEditing && m_fieldHistoryTarget && m_transformFieldScene == &scene &&
        m_transformFieldRevision == scene.GetStructureRevision())
    {
        const auto index = ResolveHistoryTarget(demoScene, *m_fieldHistoryTarget);

        if(index)
        {
            RecordTransformEdit(*m_fieldHistoryTarget, m_fieldHistoryEpoch, m_transformFieldUndoStart,
                static_cast<const Scene&>(scene).GetRenderObjects()[*index].transform);
        }
    }

    m_transformFieldEditing = false;
    m_transformFieldUndoObject = InvalidObjectSelection;
    m_transformFieldScene = nullptr;
    m_transformFieldRevision = 0;

    m_fieldHistoryTarget.reset();
}

void Editor::DrawTransformGizmo(
    IDemoScene& demoScene,
    float viewportWidth,
    float viewportHeight)
{
    if (m_selectedObject == InvalidObjectSelection || viewportWidth <= 0.0f || viewportHeight <= 0.0f)
    {
        return;
    }

    Scene& scene = demoScene.GetScene();

    const Scene& constScene = scene;
    const auto& objects = constScene.GetRenderObjects();

    if (m_selectedObject >= objects.size())
    {
        return;
    }

    Transform transform = objects[m_selectedObject].transform;

    const Camera& camera = scene.GetCamera();

    Matrix4 view = camera.GetViewMatrix().Transpose();

    Matrix4 projection = camera.GetProjectionMatrix().Transpose();
    Matrix4 model = transform.GetModelMatrix().Transpose();

    // Append to the image's window: separate root windows have independent display ordering.
    // The first Begin in DrawSceneViewport owns placement, sizing and style for this frame.
    const ImGuiWindowFlags viewportFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::Begin("Scene Viewport", nullptr, viewportFlags);

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGui::GetWindowDrawList()->PushClipRect(m_viewportImage.position,
        ImVec2(m_viewportImage.position.x + viewportWidth, m_viewportImage.position.y + viewportHeight), true);
    ImGuizmo::SetRect(m_viewportImage.position.x, m_viewportImage.position.y, viewportWidth, viewportHeight);
    const bool allowGizmo = !ImGui::GetIO().AppFocusLost && !m_sceneOperationPending &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && !m_cameraLooking &&
        !ImGui::GetDragDropPayload();
    ImGuizmo::Enable(allowGizmo && !m_workspace.IsSplitterDragging() && (m_viewportHovered || ImGuizmo::IsUsing()));

    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;

    switch (m_gizmoMode)
    {
    case GizmoMode::Translate:
        operation = ImGuizmo::TRANSLATE;
        break;
    case GizmoMode::Rotate:
        operation = ImGuizmo::ROTATE;
        break;
    case GizmoMode::Scale:
        operation = ImGuizmo::SCALE;
        break;
    }

    ImGuizmo::Manipulate(&view.m[0][0], &projection.m[0][0], operation, ImGuizmo::WORLD, &model.m[0][0]);
    ImGui::GetWindowDrawList()->PopClipRect();

    ImGui::End();

    const bool gizmoUsing = ImGuizmo::IsUsing();

    if (gizmoUsing && !m_gizmoWasUsing)
    {
        m_gizmoUndoObject = m_selectedObject;
        m_gizmoUndoStart = transform;
        m_gizmoHistoryTarget = CaptureHistoryTarget(demoScene, m_selectedObject);
        m_gizmoHistoryEpoch = m_historyEpoch;
    }

    if (gizmoUsing)
    {
        float translation[3];
        float rotationDegrees[3];
        float scale[3];

        ImGuizmo::DecomposeMatrixToComponents(&model.m[0][0], translation, rotationDegrees, scale);

        bool changed = false;

        switch (m_gizmoMode)
        {
        case GizmoMode::Translate:
        {
            const Vector3 newPosition{translation[0], translation[1], translation[2]};

            if (newPosition.x != transform.position.x || newPosition.y != transform.position.y ||
                newPosition.z != transform.position.z)
            {
                transform.position = newPosition;

                changed = true;
            }

            break;
        }

        case GizmoMode::Rotate:
        {
            const Vector3 newRotation{ToRadians(rotationDegrees[0]), ToRadians(rotationDegrees[1]),
                                      ToRadians(rotationDegrees[2])};

            transform.rotation = newRotation;
            changed = true;
            break;
        }

        case GizmoMode::Scale:
        {
            transform.scale =
                Vector3{(std::max)(0.01f, scale[0]), (std::max)(0.01f, scale[1]), (std::max)(0.01f, scale[2])};

            changed = true;
            break;
        }
        }

        if (changed)
        {
            scene.SetRenderObjectTransform(m_selectedObject, transform);
        }
    }

    if(!gizmoUsing && m_gizmoWasUsing && m_gizmoHistoryTarget)
    {
        const auto index = ResolveHistoryTarget(demoScene, *m_gizmoHistoryTarget);

        if(index)
        {
            RecordTransformEdit(*m_gizmoHistoryTarget, m_gizmoHistoryEpoch, m_gizmoUndoStart,
                static_cast<const Scene&>(scene).GetRenderObjects()[*index].transform);
        }

        m_gizmoUndoObject = InvalidObjectSelection;
        m_gizmoHistoryTarget.reset();
    }

    m_gizmoWasUsing = gizmoUsing;
}

void Editor::HandleViewportSelection(
    IDemoScene& demoScene,
    float viewportWidth,
    float viewportHeight)
{
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();

    if(!m_viewportHovered || m_sceneOperationPending || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) ||
        ImGuizmo::IsUsing())
    {
        return;
    }

    if (m_selectedObject != InvalidObjectSelection && ImGuizmo::IsOver())
    {
        return;
    }

    const ImVec2 mouse(io.MousePos.x - m_viewportImage.position.x, io.MousePos.y - m_viewportImage.position.y);

    if (mouse.x < 0.0f || mouse.y < 0.0f || mouse.x >= viewportWidth || mouse.y >= viewportHeight)
    {
        return;
    }

    const Scene& scene = demoScene.GetScene();

    m_selectedObject = PickObject(scene, mouse.x, mouse.y, viewportWidth, viewportHeight);
    m_objectInspectionRequested = true;
}

void Editor::ResetTransformGestures()
{
    m_gizmoWasUsing = false;
    m_gizmoUndoObject = InvalidObjectSelection;
    m_gizmoHistoryTarget.reset();

    m_transformFieldEditing = false;
    m_transformFieldUndoObject = InvalidObjectSelection;
    m_transformFieldScene = nullptr;
    m_transformFieldRevision = 0;

    m_fieldHistoryTarget.reset();
}

void Editor::HandleTransformUndo(IDemoScene& demoScene)
{
    const auto& io = ImGui::GetIO();

    if(io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false) && !HistoryReplayBlocked())
    {
        ReplaySceneHistory(demoScene, false);
    }
}

void Editor::HandleTransformRedo(IDemoScene& demoScene)
{
    const auto& io = ImGui::GetIO();
    const bool pressed = io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Y, false) ||
        (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)));

    if(pressed && !HistoryReplayBlocked())
    {
        ReplaySceneHistory(demoScene, true);
    }
}

void Editor::UpdateCameraLook(
    IDemoScene& demoScene,
    float viewportWidth,
    float viewportHeight)
{
    const ImGuiIO& io = ImGui::GetIO();

    (void)viewportWidth;
    (void)viewportHeight;

    if(!ImGui::IsMouseDown(ImGuiMouseButton_Right) || io.AppFocusLost || !m_viewportFocused ||
       ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
    {
        if (m_cameraLooking)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        }

        m_cameraLooking = false;
        return;
    }

    if (!m_cameraLooking)
    {
        if(!m_viewportHovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Right) || m_sceneOperationPending)
        {
            return;
        }

        if (ImGuizmo::IsUsing())
        {
            return;
        }

        Camera& camera = demoScene.GetScene().GetCamera();

        const Vector3 direction = camera.GetTarget() - camera.GetPosition();
        const float horizontalLength = std::sqrt(direction.x * direction.x + direction.z * direction.z);

        m_cameraYaw = std::atan2(direction.x, direction.z);

        m_cameraPitch = std::atan2(direction.y, horizontalLength);

        CaptureViewportRightClick(demoScene);
        m_cameraLooking = true;

        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }

    ImGui::SetMouseCursor(ImGuiMouseCursor_None);

    ImVec2 delta = io.MouseDelta;

    if(!m_viewportRightDragged)
    {
        if(!ImGui::IsMouseDragging(ImGuiMouseButton_Right))
        {
            return;
        }

        // Apply the accumulated motion once the gesture becomes camera navigation.
        // Motion on the initial press can precede the click and must not rotate the camera.
        delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right, 0.0f);
        m_viewportRightDragged = true;
    }

    constexpr float sensitivity = 0.003f;
    m_cameraYaw += delta.x * sensitivity;
    m_cameraPitch -= delta.y * sensitivity;

    m_cameraPitch = (std::clamp)(m_cameraPitch, -1.55334f, 1.55334f);

    const Vector3 direction{std::sin(m_cameraYaw) * std::cos(m_cameraPitch), std::sin(m_cameraPitch),
                            std::cos(m_cameraYaw) * std::cos(m_cameraPitch)};

    Camera& camera = demoScene.GetScene().GetCamera();

    camera.SetTarget(camera.GetPosition() + direction);
}

void Editor::UpdateCameraMovement(
    IDemoScene& demoScene,
    float deltaTime)
{
    if (!m_cameraLooking || deltaTime <= 0.0f)
    {
        return;
    }

    Camera& camera = demoScene.GetScene().GetCamera();

    const Vector3 forward{std::sin(m_cameraYaw) * std::cos(m_cameraPitch), std::sin(m_cameraPitch),
                          std::cos(m_cameraYaw) * std::cos(m_cameraPitch)};

    const Vector3 right{std::cos(m_cameraYaw), 0.0f, -std::sin(m_cameraYaw)};

    const Vector3 up{0.0f, 1.0f, 0.0f};

    Vector3 movement{0.0f, 0.0f, 0.0f};

    if (ImGui::IsKeyDown(ImGuiKey_W))
    {
        movement = movement + forward;
    }

    if (ImGui::IsKeyDown(ImGuiKey_S))
    {
        movement = movement - forward;
    }

    if (ImGui::IsKeyDown(ImGuiKey_D))
    {
        movement = movement + right;
    }

    if (ImGui::IsKeyDown(ImGuiKey_A))
    {
        movement = movement - right;
    }

    if (ImGui::IsKeyDown(ImGuiKey_E) || ImGui::IsKeyDown(ImGuiKey_Space))
    {
        movement = movement + up;
    }

    if (ImGui::IsKeyDown(ImGuiKey_Q))
    {
        movement = movement - up;
    }

    const float length = movement.Length();

    if (length <= 0.000001f)
    {
        return;
    }

    m_viewportRightDragged = true;
    movement = movement * (1.0f / length);

    constexpr float normalSpeed = 5.0f;
    constexpr float fastSpeed = 15.0f;

    const bool shiftHelp = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);

    const float speed = shiftHelp ? fastSpeed : normalSpeed;

    const Vector3 offset = movement * (speed * deltaTime);

    camera.SetPosition(camera.GetPosition() + offset);

    camera.SetTarget(camera.GetTarget() + offset);
}

void Editor::DrawCamera(IDemoScene& demoScene)
{
    if (!ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    Camera& camera = demoScene.GetScene().GetCamera();

    Vector3 position = camera.GetPosition();

    if (ImGui::DragFloat3("Position", &position.x, 0.05f))
    {
        camera.SetPosition(position);
    }

    Vector3 target = camera.GetTarget();

    if (ImGui::DragFloat3("Target", &target.x, 0.05f))
    {
        camera.SetTarget(target);
    }

    if (ImGui::Button("Default##Camera"))
    {
        demoScene.ResetCamera();
    }
}

void Editor::SetSceneOperationPending(bool pending) noexcept
{
    m_sceneOperationPending = pending;
}

Editor::SceneChangeDecision Editor::DrawSceneChangeConfirmation(const std::string& error)
{
    constexpr const char* title = "Unsaved scene changes";

    if (!ImGui::IsPopupOpen(title))
    {
        ImGui::OpenPopup(title);
    }

    SceneChangeDecision decision = SceneChangeDecision::None;

    bool confirmationOpen = true;

    if (ImGui::BeginPopupModal(title, &confirmationOpen, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Save changes before leaving this scene?");

        if (!error.empty())
        {
            ImGui::TextWrapped("%s", error.c_str());
        }

        if (ImGui::Button("Save"))
        {
            decision = SceneChangeDecision::Save;
        }

        ImGui::SameLine();

        if (ImGui::Button("Discard"))
        {
            decision = SceneChangeDecision::Discard;
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel"))
        {
            decision = SceneChangeDecision::Cancel;
        }

        // Save failure is handled by Engine and reopens the modal next frame.

        if (decision != SceneChangeDecision::None)
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if(!confirmationOpen)
    {
        decision = SceneChangeDecision::Cancel;
    }

    return decision;
}

void Editor::SuspendViewportInteraction()
{
    m_viewportRightGesture = false;
    m_viewportRightObject.reset();
    m_viewportContextObject.reset();
    m_openViewportContext = false;

    m_cameraLooking = false;
    m_viewportFocused = false;
    m_viewportHovered = false;
    m_viewportImage = {};
    ImGuizmo::Enable(false);
}

void Editor::OnSceneActivated()
{
    ClearSceneHistory();
    ResetSceneInteractions();
}

void Editor::ResetSceneInteractions()
{
    m_viewportRightGesture = false;
    m_viewportRightObject.reset();
    m_viewportContextObject.reset();
    m_openViewportContext = false;

    m_sceneObjectDeletionRequest.reset();
    m_sceneObjectDeletionPending.reset();
    m_sceneObjectDeletionStatusObject.reset();
    m_sceneObjectDeletionStatus.clear();

    m_assetBrowser.InvalidateMeshDragIdentity();
    m_meshCreationRequest.reset();
    m_workingMaterialRevisions.clear();
    m_inspectorPanel.ResetWorkingMaterialNotifications();
    m_savedMaterialRevisions.clear();
    m_materialAssignmentRequest.reset();
    m_materialAssignmentSelection.reset();
    m_materialAssignmentSlotID.reset();
    m_inspectorPanel.DismissSceneMaterialPicker();
    m_materialAssignmentPending = false;
    m_materialAssignmentStatus.clear();
    m_objectInspectionRequested = false;
    m_objectInspectionIdentity.reset();

    if(m_inspectorTarget == InspectorTarget::SceneObject)
    {
        m_inspectorTarget = InspectorTarget::None;
    }

    if(m_materialLeaveAction == MaterialLeaveAction::ObjectSelection)
    {
        m_materialLeaveAction = MaterialLeaveAction::None;
    }

    m_cameraLooking = false;
    m_viewportFocused = false;

    // Different files may share a display name and structure revision.
    ResetTransformGestures();
    m_selectedObject = InvalidObjectSelection;
    m_selectionSceneName.clear();
    m_selectionStructureRevision = 0;
}

void Editor::ImportFileDrop(
    const std::vector<std::filesystem::path>& files,
    float screenX,
    float screenY,
    const ProjectPaths& paths,
    ProjectAssetRegistry& registry)
{
    if(m_sceneOperationPending || m_materialLeaveAction != MaterialLeaveAction::None)
    {
        Log::Info("Texture drop rejected while an editor operation is pending.");
        return;
    }

    m_assetBrowser.ImportFileDrop(files, screenX, screenY, paths, registry);
}

void Editor::ProcessAssetDeletion(const ProjectPaths& paths, ProjectAssetRegistry& registry)
{
    const auto request = m_assetBrowser.TakeDeletionRequest();

    if(!request)
    {
        return;
    }

    std::string error;

    if(m_inspectorPanel.HasUnsavedChanges() || m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending)
    {
        error = "Deletion rejected while editor changes or a scene operation are pending.";
    }
    else
    {
        ProjectAssetDeletion::RemoveFile(paths, registry, *request, error);
    }

    m_assetBrowser.OnDeletionCompleted(*request, error);

    if(error.empty())
    {
        Log::Info("Deleted project file: " + request->string());
    }
    else
    {
        Log::Error("Project file deletion failed: " + error);
    }

    if(error.empty() && m_inspectedAssetSelection == *request)
    {
        m_inspectedAssetSelection.clear();
        m_pendingAssetSelection.clear();

        if(m_inspectorTarget == InspectorTarget::Asset)
        {
            m_inspectorPanel.LoadSelection({}, paths, registry);
            m_inspectorTarget = InspectorTarget::None;
        }
    }
}

void Editor::ProcessAssetRename(
    const ProjectPaths& paths,
    ProjectAssetRegistry& registry,
    const std::filesystem::path& activeScenePath)
{
    const auto request = m_assetBrowser.TakeRenameRequest();

    if(!request)
    {
        return;
    }

    std::string error;
    std::filesystem::path destination;

    if(m_inspectorPanel.HasUnsavedChanges() || m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending)
    {
        error = "Rename rejected while editor changes or a scene operation are pending.";
    }
    else if(!activeScenePath.empty() &&
        _wcsicmp(std::filesystem::absolute(request->source).lexically_normal().c_str(),
            std::filesystem::absolute(activeScenePath).lexically_normal().c_str()) == 0)
    {
        error = "The active scene retains its Save path. Open another scene before renaming this file.";
    }
    else
    {
        ProjectAssetRename::RenameFile(paths, registry, request->source, request->stem, destination, error);
    }

    m_assetBrowser.OnRenameCompleted(request->source, destination, error);

    if(!error.empty())
    {
        Log::Error("Project file rename failed: " + error);
    }
    else
    {
        Log::Info("Renamed project file: " + request->source.string() + " -> " + destination.string());

        if(m_inspectedAssetSelection == request->source && request->source != destination)
        {
            m_inspectedAssetSelection = destination;
            m_pendingAssetSelection.clear();

            if(m_inspectorTarget == InspectorTarget::Asset)
            {
                m_inspectorPanel.LoadSelection(destination, paths, registry);
            }
        }
    }
}

std::optional<Editor::MaterialAssignmentRequest> Editor::TakeMaterialAssignmentRequest()
{
    auto request = std::move(m_materialAssignmentRequest);
    m_materialAssignmentRequest.reset();
    return request;
}

void Editor::OnMaterialAssignmentCompleted(const SceneObjectMetadata& selection, bool succeeded, std::string message)
{
    if(m_materialAssignmentSelection && m_materialAssignmentSelection->runtimeIdentity == selection.runtimeIdentity &&
       m_materialAssignmentSelection->objectID == selection.objectID &&
       m_materialAssignmentSelection->structureRevision == selection.structureRevision)
    {
        m_materialAssignmentPending = false;
        m_materialAssignmentStatus = (succeeded ? "Assigned: " : "Not assigned: ") + std::move(message);

        if(m_materialAssignmentSlotID)
        {
            m_inspectorPanel.OnSlotAssignmentCompleted(selection, *m_materialAssignmentSlotID,
                succeeded, m_materialAssignmentStatus);
        }
    }
}

std::vector<Editor::SceneMaterialSavedRevision> Editor::TakeSavedMaterialRevisions()
{
    return std::exchange(m_savedMaterialRevisions, {});
}

std::vector<Editor::SceneMaterialWorkingRevision> Editor::TakeWorkingMaterialRevisions()
{
    return std::exchange(m_workingMaterialRevisions, {});
}

void Editor::PublishCreatedObjectSelection(
    const Scene& scene, SceneObjectMetadata metadata, std::string activeSceneName) noexcept
{
    // Metadata was resolved through the candidate's persistent-ID mapping before
    // renderer initialization; no allocation or fallible lookup follows publication.
    OnSceneActivated();
    m_selectedObject = metadata.runtimeIndex;
    m_selectionSceneName = std::move(activeSceneName);
    m_selectionStructureRevision = scene.GetStructureRevision();

    ObjectInspectionIdentity identity{&scene, scene.GetStructureRevision()};
    identity.runtimeIdentity = metadata.runtimeIdentity;
    identity.objectID = std::move(metadata.objectID);
    identity.sceneID = std::move(metadata.sceneID);

    m_objectInspectionIdentity = std::move(identity);
    m_inspectorTarget = InspectorTarget::SceneObject;
    m_objectInspectionRequested = true;
}

void Editor::DrawMeshDropTarget(const IDemoScene& scene, const ProjectPaths& paths,
    const ProjectAssetRegistry& registry)
{
    const ImGuiPayload* dragging = ImGui::GetDragDropPayload();

    if(!dragging || !dragging->IsDataType(MeshDragPayloadType) ||
        m_viewportImage.size.x <= 0.0f || m_viewportImage.size.y <= 0.0f)
    {
        return;
    }

    if(ImGui::GetIO().AppFocusLost || m_workspace.IsSplitterDragging() || ImGuizmo::IsUsing() ||
        m_gizmoWasUsing || m_cameraLooking || m_sceneOperationPending || m_fileRequest ||
        HasBlockingObjectCreation() || HasPendingSceneHistoryOperation() || m_meshCreationRequest ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
    {
        return;
    }

    const ImRect image(m_viewportImage.position, ImVec2(
        m_viewportImage.position.x + m_viewportImage.size.x,
        m_viewportImage.position.y + m_viewportImage.size.y));
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    if(!image.Contains(mouse))
    {
        return;
    }

    // The viewport intentionally has NoInputs. This ImGui target accepts a
    // rectangle without requiring a hovered input window, preserving picking.
    ImGui::Begin("Scene Viewport");

    if(ImGui::BeginDragDropTargetViewport(ImGui::GetMainViewport(), &image))
    {
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(MeshDragPayloadType);

        if(payload && payload->IsDelivery() && m_meshDropDeliveryFrame != ImGui::GetFrameCount())
        {
            m_meshDropDeliveryFrame = ImGui::GetFrameCount();
            const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
            MeshDragPayload meshPayload;
            std::string error;
            std::optional<AssetRecord> record;

            if(!owner)
            {
                error = "Mesh drops require an active file-backed scene.";
            }
            else if(payload->DataSize != sizeof(MeshDragPayload) || !payload->Data)
            {
                error = "Mesh drop has an invalid payload size.";
            }
            else
            {
                std::memcpy(&meshPayload, payload->Data, sizeof(meshPayload));
                record = m_assetBrowser.ValidateMeshDrag(meshPayload, paths, registry, error);
            }

            Transform placement;

            if(record && !FindMeshDropPosition(*owner, mouse.x - image.Min.x, mouse.y - image.Min.y,
                image.GetWidth(), image.GetHeight(), placement.position))
            {
                record.reset();
                error = "No finite forward placement is available at this cursor position.";
            }

            if(record)
            {
                m_meshCreationRequest = MeshCreationRequest{record->id.ToReference(),
                    record->relativePath.stem().string(), placement, owner->GetRuntimeIdentity()};
                m_meshCreationStatus = "Creating mesh at cursor...";
            }
            else
            {
                OnMeshCreationCompleted(false, error);
            }
        }

        ImGui::EndDragDropTarget();
    }

    ImGui::End();
}

std::optional<Editor::MeshCreationRequest> Editor::TakeMeshCreationRequest()
{
    return std::exchange(m_meshCreationRequest, std::nullopt);
}

void Editor::OnMeshCreationCompleted(bool succeeded, const std::string& error)
{
    m_meshCreationStatus = succeeded ? "Mesh created at cursor. Save the scene to persist it." :
        "Mesh drop rejected: " + error;

    if(!succeeded)
    {
        Log::Warning(m_meshCreationStatus);
    }
}


bool Editor::SceneObjectDeletionBlocked() const
{
    return HasPendingSceneHistoryOperation() || m_sceneOperationPending || m_sceneObjectDeletionPending || m_sceneObjectDeletionRequest ||
        m_fileRequest || m_meshCreationRequest || m_materialAssignmentRequest ||
        m_materialLeaveAction != MaterialLeaveAction::None || HasBlockingObjectCreation() ||
        m_workspace.IsSplitterDragging() || ImGui::GetDragDropPayload() || ImGui::GetIO().AppFocusLost ||
        m_viewportImage.size.x <= 0.0f || m_viewportImage.size.y <= 0.0f;
}

bool Editor::SceneObjectDeletionMatches(const IDemoScene& scene, const SceneObjectMetadata& selection) const
{
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
    const auto current = owner ? owner->GetObjectMetadata(selection.objectID) : std::nullopt;

    return current && m_selectedObject == current->runtimeIndex &&
        current->sceneID == selection.sceneID && current->runtimeIdentity == selection.runtimeIdentity &&
        current->structureRevision == selection.structureRevision && current->runtimeIndex == selection.runtimeIndex;
}

void Editor::CaptureViewportRightClick(const IDemoScene& scene)
{
    m_viewportRightObject.reset();
    m_viewportRightGesture = true;
    m_viewportRightDragged = false;

    if(SceneObjectDeletionBlocked() || ImGuizmo::IsOver() || ImGui::GetIO().WantTextInput)
    {
        return;
    }

    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);

    if(!owner)
    {
        return;
    }

    const auto mouse = ImGui::GetIO().MousePos;
    const auto index = PickObject(scene.GetScene(), mouse.x - m_viewportImage.position.x,
        mouse.y - m_viewportImage.position.y, m_viewportImage.size.x, m_viewportImage.size.y);

    if(index != InvalidObjectSelection)
    {
        m_viewportRightObject = owner->GetObjectMetadata(index);
    }
}

void Editor::FinishViewportRightClick(const IDemoScene& scene)
{
    if(!m_viewportRightGesture || !ImGui::IsMouseReleased(ImGuiMouseButton_Right))
    {
        return;
    }

    m_viewportRightGesture = false;
    auto selected = std::exchange(m_viewportRightObject, std::nullopt);
    const auto& io = ImGui::GetIO();
    const bool dragged = m_viewportRightDragged ||
        io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] >= io.MouseDragThreshold * io.MouseDragThreshold;

    if(!selected || dragged || !m_viewportHovered || SceneObjectDeletionBlocked() ||
        ImGuizmo::IsUsing() || ImGuizmo::IsOver() || ImGui::IsAnyItemActive() || io.WantTextInput ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    {
        return;
    }

    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
    const auto current = owner ? owner->GetObjectMetadata(selected->objectID) : std::nullopt;

    if(!current || current->runtimeIdentity != selected->runtimeIdentity ||
        current->sceneID != selected->sceneID || current->structureRevision != selected->structureRevision)
    {
        return;
    }

    const auto index = PickObject(scene.GetScene(), io.MousePos.x - m_viewportImage.position.x,
        io.MousePos.y - m_viewportImage.position.y, m_viewportImage.size.x, m_viewportImage.size.y);

    if(index != current->runtimeIndex)
    {
        return;
    }

    m_selectedObject = current->runtimeIndex;
    m_objectInspectionRequested = true;
    m_viewportContextObject = *current;
    m_openViewportContext = true;
}

void Editor::QueueSceneObjectDeletion(const IDemoScene& scene, const SceneObjectMetadata& selection)
{
    if(SceneObjectDeletionBlocked() || !SceneObjectDeletionMatches(scene, selection))
    {
        return;
    }

    m_sceneObjectDeletionRequest = selection;
    m_sceneObjectDeletionPending = selection;
    m_sceneObjectDeletionStatusObject = selection;
    m_sceneObjectDeletionStatus = "Deleting scene object...";
}

void Editor::DrawSceneObjectDeletionControls(IDemoScene& scene)
{
    if(m_sceneObjectDeletionStatusObject && !SceneObjectDeletionMatches(scene, *m_sceneObjectDeletionStatusObject))
    {
        m_sceneObjectDeletionStatusObject.reset();
        m_sceneObjectDeletionStatus.clear();
    }

    const auto& io = ImGui::GetIO();

    if(io.AppFocusLost || !m_viewportFocused || SceneObjectDeletionBlocked())
    {
        m_viewportRightGesture = false;
        m_viewportRightObject.reset();
    }

    FinishViewportRightClick(scene);

    const bool keyboardAllowed = m_viewportFocused && !SceneObjectDeletionBlocked() &&
        !m_openViewportContext && !m_viewportRightGesture &&
        !io.WantTextInput && !io.WantCaptureKeyboard && !ImGui::IsAnyItemActive() &&
        !m_cameraLooking && !ImGuizmo::IsUsing() && !ImGui::IsMouseDown(ImGuiMouseButton_Right) &&
        m_viewportImage.size.x > 0.0f && m_viewportImage.size.y > 0.0f &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

    if(keyboardAllowed && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
        const auto selected = owner ? owner->GetObjectMetadata(m_selectedObject) : std::nullopt;

        if(selected && m_selectionStructureRevision == selected->structureRevision)
        {
            QueueSceneObjectDeletion(scene, *selected);
        }
    }

    // Use the viewport's stable popup ID even though its image window ignores mouse input.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::Begin("Scene Viewport", nullptr, flags);

    if(m_openViewportContext)
    {
        ImGui::OpenPopup("SceneObjectContext");
        m_openViewportContext = false;
    }

    if(ImGui::BeginPopup("SceneObjectContext"))
    {
        if(!m_viewportContextObject || SceneObjectDeletionBlocked() ||
            !SceneObjectDeletionMatches(scene, *m_viewportContextObject))
        {
            ImGui::CloseCurrentPopup();
        }
        else
        {
            ImGui::TextUnformatted(m_viewportContextObject->name.c_str());
            ImGui::Separator();

            if(ImGui::MenuItem("Delete"))
            {
                QueueSceneObjectDeletion(scene, *m_viewportContextObject);
            }
        }

        ImGui::EndPopup();
    }

    if(!ImGui::IsPopupOpen("SceneObjectContext"))
    {
        m_viewportContextObject.reset();
    }

    ImGui::End();
}

std::optional<SceneObjectMetadata> Editor::TakeSceneObjectDeletionRequest()
{
    return std::exchange(m_sceneObjectDeletionRequest, std::nullopt);
}

void Editor::OnSceneObjectDeletionCompleted(const IDemoScene& scene, const SceneObjectMetadata& selection,
    bool succeeded, const std::string& error)
{
    if(!m_sceneObjectDeletionPending ||
        m_sceneObjectDeletionPending->objectID != selection.objectID ||
        m_sceneObjectDeletionPending->runtimeIdentity != selection.runtimeIdentity ||
        m_sceneObjectDeletionPending->structureRevision != selection.structureRevision ||
        m_sceneObjectDeletionPending->sceneID != selection.sceneID)
    {
        return;
    }

    m_sceneObjectDeletionPending.reset();

    if(succeeded || !SceneObjectDeletionMatches(scene, selection))
    {
        m_sceneObjectDeletionStatusObject.reset();
        m_sceneObjectDeletionStatus.clear();
        return;
    }

    m_sceneObjectDeletionStatusObject = selection;
    m_sceneObjectDeletionStatus = "Object deletion failed: " + error;
    Log::Warning(m_sceneObjectDeletionStatus);
}

namespace
{
    bool SameHistoryTransform(const Transform& left, const Transform& right)
    {
        const auto same = [](const Vector3& a, const Vector3& b)
        {
            return a.x == b.x && a.y == b.y && a.z == b.z;
        };
        return same(left.position, right.position) && same(left.rotation, right.rotation) &&
            same(left.scale, right.scale);
    }
}

void Editor::ClearSceneHistory()
{
    // Session invalidation explicitly cancels requests; late results carry the old epoch.
    m_pendingHistoryReplay.reset();
    m_historyReplayRequest.reset();
    m_stagedDeletionHistory.reset();

    m_sceneHistory.clear();
    m_historyCursor = 0;
    m_historyPayloadBytes = 0;
    m_historySession.reset();

    ++m_historyEpoch;
    m_historyObservedScene = nullptr;
    m_historyRuntimeIdentity = 0;
    m_historyStructureRevision = 0;

    m_sceneHistoryStatus.clear();
    ResetTransformGestures();
}

void Editor::ReportSceneHistoryFailure(const std::string& error)
{
    m_sceneHistoryStatus = "Scene history: " + error;
    Log::Warning(m_sceneHistoryStatus);
}

bool Editor::UpdateSceneHistorySession(const IDemoScene& demoScene, const ProjectPaths& paths,
    const std::string& name)
{
    std::error_code error;
    const auto project = std::filesystem::canonical(paths.GetRoot(), error);
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&demoScene);

    if(error || (owner && (owner->GetPersistentSceneID().empty() ||
        std::filesystem::canonical(owner->GetProjectRoot(), error) != project || error)))
    {
        ClearSceneHistory();
        ReportSceneHistoryFailure("Cannot establish the current project/scene identity; replay is disabled.");
        return false;
    }

    const std::string identity = owner ? std::string(owner->GetPersistentSceneID()) : name;
    const auto path = owner ? owner->GetScenePath().lexically_normal() : std::filesystem::path{};
    const auto runtime = owner ? owner->GetRuntimeIdentity() : 0;
    const auto& scene = demoScene.GetScene();
    const auto revision = scene.GetStructureRevision();
    const bool changed = m_historySession && (m_historySession->projectIdentity != project ||
        m_historySession->sceneIdentity != identity || m_historySession->scenePath != path ||
        m_historyObservedScene != &scene || m_historyRuntimeIdentity != runtime ||
        m_historyStructureRevision != revision);

    if(changed)
    {
        ClearSceneHistory();
    }

    if(!m_historySession)
    {
        if(m_historyEpoch == 0)
        {
            ++m_historyEpoch;
        }

        m_historySession = SceneHistorySession{project, path, identity, m_historyEpoch};
        m_historyObservedScene = &scene;
        m_historyRuntimeIdentity = runtime;
        m_historyStructureRevision = revision;
    }

    return true;
}

bool Editor::RebindSceneHistoryAfterStructureChange(const IDemoScene& demoScene)
{
    // Precondition: a future supported structural command has completed its checked
    // Renderer publication. Never use this to acknowledge arbitrary structural edits.
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&demoScene);
    std::error_code error;
    const auto project = owner ? std::filesystem::canonical(owner->GetProjectRoot(), error) :
        std::filesystem::path{};

    if(!owner || !m_historySession || error || project != m_historySession->projectIdentity ||
        owner->GetPersistentSceneID() != m_historySession->sceneIdentity ||
        owner->GetScenePath().lexically_normal() != m_historySession->scenePath)
    {
        ReportSceneHistoryFailure("Structural publication does not belong to the current editing session.");
        return false;
    }

    m_historyObservedScene = &demoScene.GetScene();
    m_historyRuntimeIdentity = owner->GetRuntimeIdentity();
    m_historyStructureRevision = demoScene.GetScene().GetStructureRevision();
    return true;
}

std::optional<Editor::HistoryTarget> Editor::CaptureHistoryTarget(const IDemoScene& demoScene, size_t index)
{
    const auto& scene = demoScene.GetScene();

    if(!m_historySession || m_historyObservedScene != &scene ||
        m_historyStructureRevision != scene.GetStructureRevision() || index >= scene.GetRenderObjects().size())
    {
        ReportSceneHistoryFailure("Cannot record a transform against stale scene mappings.");
        return std::nullopt;
    }

    if(const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&demoScene))
    {
        const auto metadata = owner->GetObjectMetadata(index);

        if(!metadata || metadata->runtimeIdentity != m_historyRuntimeIdentity ||
            metadata->sceneID != m_historySession->sceneIdentity)
        {
            ReportSceneHistoryFailure("Cannot identify the edited object's persistent ID.");
            return std::nullopt;
        }

        return PersistentHistoryTarget{metadata->objectID};
    }

    return ProceduralHistoryTarget{index};
}

std::optional<size_t> Editor::ResolveHistoryTarget(const IDemoScene& demoScene, const HistoryTarget& target)
{
    const auto& scene = demoScene.GetScene();
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&demoScene);

    if(!m_historySession || m_historySession->epoch != m_historyEpoch || m_historyObservedScene != &scene ||
        m_historyStructureRevision != scene.GetStructureRevision() ||
        (owner && m_historyRuntimeIdentity != owner->GetRuntimeIdentity()))
    {
        ReportSceneHistoryFailure("Scene bindings changed; the history cursor was preserved.");
        return std::nullopt;
    }

    if(const auto* persistent = std::get_if<PersistentHistoryTarget>(&target))
    {
        const auto metadata = owner ? owner->GetObjectMetadata(persistent->objectID) : std::nullopt;

        if(!metadata || metadata->sceneID != m_historySession->sceneIdentity)
        {
            ReportSceneHistoryFailure("Target object '" + persistent->objectID + "' cannot be resolved; cursor preserved.");
            return std::nullopt;
        }

        return metadata->runtimeIndex;
    }

    const auto index = std::get<ProceduralHistoryTarget>(target).objectIndex;

    if(owner || index >= scene.GetRenderObjects().size())
    {
        ReportSceneHistoryFailure("Procedural target cannot be resolved; the history cursor was preserved.");
        return std::nullopt;
    }

    return index;
}

void Editor::TruncateHistoryRedo()
{
    for(size_t index = m_historyCursor; index < m_sceneHistory.size(); ++index)
    {
        m_historyPayloadBytes -= m_sceneHistory[index].payloadBytes;
    }

    m_sceneHistory.erase(m_sceneHistory.begin() + m_historyCursor, m_sceneHistory.end());
}

void Editor::RecordTransformEdit(HistoryTarget target, std::uint64_t epoch,
    const Transform& before, const Transform& after)
{
    if(SameHistoryTransform(before, after))
    {
        return;
    }

    if(HasPendingSceneHistoryOperation())
    {
        ReportSceneHistoryFailure("A deferred history operation is pending; transform recording was rejected.");
        return;
    }

    if(!m_historySession || epoch != m_historySession->epoch)
    {
        ReportSceneHistoryFailure("Completed transform belongs to a different editing session.");
        return;
    }

    SceneHistoryEntry entry{TransformEdit{std::move(target), before, after}, epoch, sizeof(SceneHistoryEntry)};
    const auto& edit = std::get<TransformEdit>(entry.command);

    if(const auto* persistent = std::get_if<PersistentHistoryTarget>(&edit.target))
    {
        // Capacity accounts conservatively for retained string storage, including SSO.
        entry.payloadBytes += persistent->objectID.capacity() + 1;
    }

    m_sceneHistory.reserve(kSceneHistoryCapacity);
    TruncateHistoryRedo();

    if(entry.payloadBytes > kSceneHistoryPayloadBytes)
    {
        ReportSceneHistoryFailure("Transform command exceeds the 4 MiB history payload limit.");
        return;
    }

    AppendSceneHistoryEntry(std::move(entry));
}

void Editor::AppendSceneHistoryEntry(SceneHistoryEntry entry) noexcept
{
    static_assert(std::is_nothrow_move_constructible_v<SceneHistoryEntry>);
    static_assert(std::is_nothrow_move_assignable_v<SceneHistoryEntry>);

    TruncateHistoryRedo();

    while(m_historyCursor > 0 && (m_sceneHistory.size() >= kSceneHistoryCapacity ||
        m_historyPayloadBytes + entry.payloadBytes > kSceneHistoryPayloadBytes))
    {
        m_historyPayloadBytes -= m_sceneHistory.front().payloadBytes;
        m_sceneHistory.erase(m_sceneHistory.begin());
        --m_historyCursor;
    }

    m_historyPayloadBytes += entry.payloadBytes;
    m_sceneHistory.push_back(std::move(entry));
    m_historyCursor = m_sceneHistory.size();
    m_sceneHistoryStatus.clear();
}

bool Editor::HistoryReplayBlocked() const
{
    const auto& io = ImGui::GetIO();
    return HasPendingSceneHistoryOperation() || io.WantTextInput || io.AppFocusLost || ImGui::IsAnyItemActive() ||
        ImGuizmo::IsUsing() || m_gizmoWasUsing || m_transformFieldEditing || m_cameraLooking ||
        ImGui::IsMouseDown(ImGuiMouseButton_Right) || m_viewportRightGesture ||
        m_sceneOperationPending || m_sceneObjectDeletionPending || m_sceneObjectDeletionRequest ||
        m_materialAssignmentPending || m_materialAssignmentRequest || m_meshCreationRequest || m_fileRequest ||
        m_materialLeaveAction != MaterialLeaveAction::None || m_workspace.IsSplitterDragging() ||
        ImGui::GetDragDropPayload() ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

void Editor::ReplaySceneHistory(IDemoScene& demoScene, bool redo)
{
    if((redo && m_historyCursor == m_sceneHistory.size()) || (!redo && m_historyCursor == 0))
    {
        return;
    }

    const auto& entry = m_sceneHistory[redo ? m_historyCursor : m_historyCursor - 1];

    if(!m_historySession || entry.sessionEpoch != m_historySession->epoch)
    {
        ReportSceneHistoryFailure("Command belongs to another load session; cursor preserved.");
        return;
    }

    const auto* ownedDeletion = std::get_if<std::unique_ptr<ObjectDeletion>>(&entry.command);

    if(ownedDeletion && *ownedDeletion)
    {
        const auto* deletion = ownedDeletion->get();
        const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&demoScene);

        if(!owner || !CanPreserveSceneHistory(*owner, deletion->session.projectIdentity, deletion->session.epoch))
        {
            ReportSceneHistoryFailure("Deletion replay does not belong to the current editing session.");
            return;
        }

        HistoryReplayIdentity identity{AllocateSceneHistoryOperationID(), m_historyEpoch, redo,
            redo ? deletion->selectionAfter : deletion->selectionBefore};
        // Copy the outbound payload before locking the cursor. No command addresses escape.
        HistoryReplayRequest request{identity, deletion->object, deletion->documentIndex};
        m_historyReplayRequest = std::move(request);
        m_pendingHistoryReplay = PendingHistoryReplay{std::move(identity), m_historyCursor};
        return;
    }

    const auto& edit = std::get<TransformEdit>(entry.command);
    const auto index = ResolveHistoryTarget(demoScene, edit.target);

    if(!index)
    {
        return;
    }

    demoScene.GetScene().SetRenderObjectTransform(*index, redo ? edit.after : edit.before);

    if(redo)
    {
        ++m_historyCursor;
    }
    else
    {
        --m_historyCursor;
    }

    m_sceneHistoryStatus.clear();
}

bool Editor::CanPreserveSceneHistory(const IDemoScene& demoScene,
    const std::filesystem::path& canonicalProject, std::uint64_t epoch) const
{
    if(epoch != m_historyEpoch)
    {
        return false;
    }

    if(!m_historySession)
    {
        return m_sceneHistory.empty();
    }

    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&demoScene);
    return owner && m_historySession->epoch == epoch &&
        m_historySession->projectIdentity == canonicalProject &&
        m_historySession->sceneIdentity == owner->GetPersistentSceneID() &&
        m_historySession->scenePath == owner->GetScenePath().lexically_normal() &&
        m_historyRuntimeIdentity == owner->GetRuntimeIdentity() &&
        m_historyObservedScene == &owner->GetScene() &&
        m_historyStructureRevision == owner->GetScene().GetStructureRevision();
}

void Editor::PublishRestoredObjectSelection(const FileBackedDemoScene& owner,
    std::optional<SceneObjectMetadata> selection, std::string activeSceneName) noexcept
{
    const auto& scene = owner.GetScene();
    m_historyObservedScene = &scene;
    m_historyRuntimeIdentity = owner.GetRuntimeIdentity();
    m_historyStructureRevision = scene.GetStructureRevision();

    m_inspectorPanel.DismissTexturePicker();
    m_inspectorPanel.ReleaseSceneObjectPreview();
    SuspendViewportInteraction();
    ResetSceneInteractions();

    m_selectionSceneName = std::move(activeSceneName);
    m_selectionStructureRevision = scene.GetStructureRevision();

    if(!selection)
    {
        return;
    }

    m_selectedObject = selection->runtimeIndex;
    ObjectInspectionIdentity identity{&scene, scene.GetStructureRevision()};
    identity.runtimeIdentity = selection->runtimeIdentity;
    identity.objectID = std::move(selection->objectID);
    identity.sceneID = std::move(selection->sceneID);

    m_objectInspectionIdentity = std::move(identity);
    m_inspectorTarget = InspectorTarget::SceneObject;
    m_objectInspectionRequested = true;
}

bool Editor::HasPendingSceneHistoryOperation() const noexcept
{
    return m_pendingHistoryReplay.has_value() || m_stagedDeletionHistory.has_value();
}

bool Editor::MatchesPendingHistoryReplay(const HistoryReplayIdentity& identity) const noexcept
{
    return m_pendingHistoryReplay && identity.epoch == m_historyEpoch &&
        m_pendingHistoryReplay->identity.operationID == identity.operationID &&
        m_pendingHistoryReplay->identity.epoch == identity.epoch &&
        m_pendingHistoryReplay->identity.redo == identity.redo &&
        m_pendingHistoryReplay->identity.selectionID == identity.selectionID &&
        m_pendingHistoryReplay->cursor == m_historyCursor;
}

std::optional<Editor::HistoryReplayRequest> Editor::TakeSceneHistoryReplayRequest()
{
    return std::exchange(m_historyReplayRequest, std::nullopt);
}

size_t Editor::ObjectDeletionPayloadBytes(const ObjectDeletion& deletion)
{
    const auto bytes = [](const std::string& text) { return text.capacity() + 1; };
    const auto& object = deletion.object;
    size_t size = sizeof(SceneHistoryEntry) + sizeof(ObjectDeletion) + bytes(object.id) + bytes(object.name) +
        bytes(object.meshReference) + bytes(object.materialReference);

    for(const auto& [slot, reference] : object.materialSlotBindings)
    {
        size += sizeof(std::pair<const std::string, std::string>) + 4 * sizeof(void*) + bytes(slot) + bytes(reference);
    }

    size += bytes(deletion.session.sceneIdentity) +
        (deletion.session.projectIdentity.native().capacity() + 1) * sizeof(std::filesystem::path::value_type) +
        (deletion.session.scenePath.native().capacity() + 1) * sizeof(std::filesystem::path::value_type);

    if(deletion.selectionBefore)
    {
        size += bytes(*deletion.selectionBefore);
    }

    if(deletion.selectionAfter)
    {
        size += bytes(*deletion.selectionAfter);
    }

    return size;
}

bool Editor::StageObjectDeletionHistory(SceneObjectDocument object, size_t documentIndex,
    std::optional<std::string> selectionBefore, std::optional<std::string> selectionAfter,
    std::uint64_t operationID, std::uint64_t epoch, bool replay, std::string& error)
{
    if(!m_historySession || epoch != m_historyEpoch || m_historySession->epoch != epoch)
    {
        error = "Deletion history cannot establish the current editing session.";
        return false;
    }

    if(replay)
    {
        if(!m_pendingHistoryReplay || m_pendingHistoryReplay->identity.operationID != operationID ||
            !m_pendingHistoryReplay->identity.redo || m_pendingHistoryReplay->identity.epoch != epoch ||
            m_pendingHistoryReplay->cursor != m_historyCursor || m_historyCursor >= m_sceneHistory.size())
        {
            error = "Redo deletion operation identity is stale.";
            return false;
        }

        const auto* owned = std::get_if<std::unique_ptr<ObjectDeletion>>(&m_sceneHistory[m_historyCursor].command);
        const auto* deletion = owned ? owned->get() : nullptr;

        if(!deletion || deletion->documentIndex != documentIndex || deletion->object.id != object.id ||
            deletion->object.name != object.name || !SameHistoryTransform(deletion->object.transform, object.transform) ||
            deletion->object.meshReference != object.meshReference ||
            deletion->object.materialReference != object.materialReference ||
            deletion->object.materialSlotBindings != object.materialSlotBindings)
        {
            error = "Redo deletion target changed outside its history command; cursor preserved.";
            return false;
        }

        return true;
    }

    if(HasPendingSceneHistoryOperation())
    {
        error = "Another scene history operation is pending.";
        return false;
    }

    ObjectDeletion deletion{std::move(object), documentIndex, std::move(selectionBefore),
        std::move(selectionAfter), *m_historySession};
    const auto bytes = ObjectDeletionPayloadBytes(deletion);

    if(bytes > kSceneHistoryPayloadBytes)
    {
        error = "Deletion rejected: its undo snapshot exceeds the 4 MiB history payload limit.";
        ReportSceneHistoryFailure(error);
        return false;
    }

    m_sceneHistory.reserve(kSceneHistoryCapacity);
    m_stagedDeletionHistory = StagedDeletionHistory{
        SceneHistoryEntry{std::make_unique<ObjectDeletion>(std::move(deletion)), epoch, bytes}, operationID};
    return true;
}

void Editor::CompleteObjectDeletionHistory(std::uint64_t operationID, std::uint64_t epoch,
    bool succeeded, const std::string& error)
{
    if(m_pendingHistoryReplay && m_pendingHistoryReplay->identity.operationID == operationID &&
        m_pendingHistoryReplay->identity.epoch == epoch)
    {
        // The identity borrow remains valid until CompleteSceneHistoryReplay resets
        // pending state; it is not accessed afterward. No success-path allocation.
        CompleteSceneHistoryReplay(m_pendingHistoryReplay->identity, succeeded, error);
        return;
    }

    if(!m_stagedDeletionHistory || m_stagedDeletionHistory->operationID != operationID ||
        m_stagedDeletionHistory->entry.sessionEpoch != epoch || epoch != m_historyEpoch)
    {
        return;
    }

    if(succeeded)
    {
        auto entry = std::move(m_stagedDeletionHistory->entry);
        m_stagedDeletionHistory.reset();
        AppendSceneHistoryEntry(std::move(entry));
        return;
    }

    m_stagedDeletionHistory.reset();
    ReportSceneHistoryFailure(error);
}

void Editor::CompleteSceneHistoryReplay(const HistoryReplayIdentity& identity,
    bool succeeded, const std::string& error)
{
    if(!MatchesPendingHistoryReplay(identity))
    {
        return;
    }

    if(succeeded)
    {
        m_historyCursor = identity.redo ? m_historyCursor + 1 : m_historyCursor - 1;
        m_sceneHistoryStatus.clear();
    }
    else
    {
        ReportSceneHistoryFailure("Deferred undo/redo failed: " + error);
    }

    m_pendingHistoryReplay.reset();
    m_historyReplayRequest.reset();
}

void Editor::InvalidateSceneHistoryRedoAfterEdit() noexcept
{
    if(!HasPendingSceneHistoryOperation())
    {
        TruncateHistoryRedo();
    }
}
