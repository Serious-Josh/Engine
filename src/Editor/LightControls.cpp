#include "Editor/Editor.h"
#include "Editor/InspectorNumericDrag.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Math/Math.h"
#include "Scene/SceneLighting.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <array>
#include <cmath>

namespace
{
    struct LightNumericField
    {
        const char* label;
        float LightComponent::* member;
        bool degrees = false;
        float speed = 0.01f;
    };

    // Drag speeds only; ValidateLightComponent is the sole value contract. Order matches
    // Editor::m_lightDragDrafts. The intensity label names the unit of the light's type.
    constexpr std::array<LightNumericField, 5> kLightFields{{
        {"Luminous Flux (lm)", &LightComponent::intensity, false, 10.0f},
        {"Range (m)", &LightComponent::range, false, 0.1f},
        {"Inner Cone Angle (deg)", &LightComponent::innerConeAngle, true, 0.25f},
        {"Outer Cone Angle (deg)", &LightComponent::outerConeAngle, true, 0.25f},
        {"Angular Radius (deg)", &LightComponent::angularRadius, true, 0.01f}
    }};

    constexpr size_t kIntensityField = 0;
    constexpr size_t kRangeField = 1;
    constexpr size_t kAngularRadiusField = 4;

    const char* LightFieldLabel(LightType type, size_t fieldIndex)
    {
        return fieldIndex == kIntensityField && type == LightType::Directional ? "Illuminance (lux)" :
            kLightFields[fieldIndex].label;
    }

    float DisplayedLightValue(const LightComponent& light, const LightNumericField& field)
    {
        const float value = light.*field.member;
        return field.degrees ? ToDegrees(value) : value;
    }

    bool IsFieldShown(LightType type, size_t fieldIndex)
    {
        if(fieldIndex == kIntensityField)
        {
            return true;
        }

        if(fieldIndex == kRangeField)
        {
            return type != LightType::Directional;
        }

        if(fieldIndex == kAngularRadiusField) return type == LightType::Directional;
        return type == LightType::Spot;
    }
}

void Editor::DismissLightManagementPopup()
{
    if(m_lightManagementPopup)
    {
        CloseComponentPopup(m_lightManagementPopup->popupID);
    }

    m_lightManagementPopup.reset();
}

void Editor::ValidateLightManagementPopup(IDemoScene& scene)
{
    if(!m_lightManagementPopup)
    {
        return;
    }

    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    SceneObjectDocument object;

    if(!ComponentPopupMatches(scene, *m_lightManagementPopup) || !owner ||
        !owner->CaptureLightInspection(m_lightManagementPopup->selection.objectID, object) || !object.light)
    {
        DismissLightManagementPopup();
    }
}

size_t Editor::LightEditPayloadBytes(const LightEdit& edit)
{
    const auto pathBytes = [](const std::filesystem::path& path)
    {
        return path.native().capacity() * sizeof(std::filesystem::path::value_type);
    };

    // Light values are stored inline; only the strings and paths allocate.
    return sizeof(SceneHistoryEntry) + sizeof(LightEdit) + edit.objectID.capacity() +
        edit.session.sceneIdentity.capacity() + pathBytes(edit.session.projectIdentity) +
        pathBytes(edit.session.scenePath);
}

bool Editor::CommitObjectLight(IDemoScene& scene, const SceneObjectMetadata& selection,
    const std::optional<LightComponent>& before, const std::optional<LightComponent>& after)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(IsPhysicsEditingLocked() || !owner || !m_historySession ||
        !RenameSessionMatches(scene, *m_historySession) || HasPendingSceneHistoryOperation() ||
        m_sceneOperationPending || HasBlockingObjectCreation())
    {
        m_lightEditStatus = "Light authoring requires a stopped scene without pending edits.";
        return false;
    }

    const auto current = ResolveComponentEditTarget(scene);

    if(!current || current->objectID != selection.objectID || current->sceneID != selection.sceneID ||
        current->runtimeIdentity != selection.runtimeIdentity || current->structureRevision != selection.structureRevision ||
        current->runtimeIndex != selection.runtimeIndex)
    {
        m_lightEditStatus = "Light inspection target is stale or blocked.";
        return false;
    }

    // The complete replacement is validated; the authored value is never clamped or partially changed.
    if(after && !ValidateLightComponent(*after, &m_lightEditStatus))
    {
        m_lightEditStatus += " The authored value was preserved.";
        return false;
    }

    if(before == after)
    {
        // Still checks staleness, but a no-op never records history or dirties the scene.
        return owner->SetObjectLight(selection, before, after, &m_lightEditStatus);
    }

    try
    {
        auto command = std::make_unique<LightEdit>(LightEdit{*m_historySession, selection.objectID, before, after});
        const size_t bytes = LightEditPayloadBytes(*command);

        if(bytes > kSceneHistoryPayloadBytes)
        {
            m_lightEditStatus = "Light edit exceeds the scene history payload limit.";
            return false;
        }

        SceneHistoryEntry entry{std::move(command), m_historyEpoch, bytes};
        // Reserve before mutation; appending the accepted command is noexcept.
        m_sceneHistory.reserve(kSceneHistoryCapacity);

        if(!owner->SetObjectLight(selection, before, after, &m_lightEditStatus))
        {
            return false;
        }

        AppendSceneHistoryEntry(std::move(entry));
        m_lightInspection.reset();
        ++m_lightControlsRevision;
        m_lightEditStatus.clear();
        return true;
    }
    catch(const std::exception& exception)
    {
        m_lightEditStatus = exception.what();
        return false;
    }
}

void Editor::DrawObjectLightControls(IDemoScene& scene)
{
    ValidateLightManagementPopup(scene);
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner)
    {
        return;
    }

    std::optional<SceneObjectMetadata> selection(std::in_place);
    SceneObjectDocument object;

    if(!owner->InspectSelectedObject(m_selectedObject, *selection, object))
    {
        return;
    }

    const bool changedTarget = !m_lightInspection || m_lightInspectionEpoch != m_historyEpoch ||
        m_lightInspection->objectID != selection->objectID ||
        m_lightInspection->runtimeIdentity != selection->runtimeIdentity ||
        m_lightInspection->structureRevision != selection->structureRevision;
    const bool blocked = !ResolveComponentEditTarget(scene, *selection);

    // Any owner, session, history or value change discards drafts (including an unfinished
    // color gesture) and retires widget IDs.
    if(changedTarget || m_lightInspectedValue != object.light || blocked)
    {
        ++m_lightControlsRevision;
        m_lightInspection = selection;
        m_lightInspectionEpoch = m_historyEpoch;
        m_lightInspectedValue = object.light;
        m_lightColorGestureActive = false;

        if(changedTarget)
        {
            m_lightEditStatus.clear();
        }

        if(object.light)
        {
            for(size_t index = 0; index < kLightFields.size(); ++index)
            {
                m_lightDragDrafts[index] = DisplayedLightValue(*object.light, kLightFields[index]);
            }

            m_lightColorDraft = object.light->color;
        }
    }

    if(!object.light)
    {
        m_lightDragDrafts.fill(0.0f);

        if(!m_lightEditStatus.empty())
        {
            ImGui::TextWrapped("%s", m_lightEditStatus.c_str());
        }

        return;
    }

    ImGui::PushID(static_cast<int>(m_historyEpoch));
    ImGui::PushID(selection->objectID.c_str());
    ImGui::PushID("Light");
    ImGui::Separator();
    ImGui::TextUnformatted("Light");
    ImGui::SameLine();
    ImGui::BeginDisabled(blocked);

    if(ImGui::SmallButton("..."))
    {
        m_lightManagementPopup = ComponentPopupTarget{
            *m_historySession, *selection, m_historyEpoch, ImGui::GetID("LightManagement")};
        ImGui::OpenPopup("LightManagement");
    }

    ImGui::EndDisabled();
    bool removed = false;

    if(ImGui::BeginPopup("LightManagement"))
    {
        ValidateLightManagementPopup(scene);
        ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);

        if(m_lightManagementPopup && ImGui::MenuItem("Remove Component"))
        {
            ValidateLightManagementPopup(scene);
            const auto current = ResolveComponentEditTarget(scene);
            SceneObjectDocument latest;

            // Only the Light is removed; Transform and every other component are preserved.
            if(m_lightManagementPopup && current && owner->CaptureLightInspection(current->objectID, latest) &&
                latest.light && CommitObjectLight(scene, *current, latest.light, std::nullopt))
            {
                DismissLightManagementPopup();
                m_lightDragDrafts.fill(0.0f);
                removed = true;
            }
        }

        ImGui::PopItemFlag();
        ImGui::EndPopup();
    }

    if(removed)
    {
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
        return;
    }

    // New numeric IDs prevent a disappearing edit from committing into another component instance.
    ImGui::PushID(static_cast<int>(m_lightControlsRevision));
    ImGui::BeginDisabled(blocked);
    const LightComponent light = *object.light;
    bool enabled = light.enabled;

    // A disabled Light stays authored and editable; it only stops contributing illumination.
    if(ImGui::Checkbox("Enabled", &enabled) && !blocked)
    {
        auto edited = light;
        edited.enabled = enabled;
        CommitObjectLight(scene, *selection, object.light, edited);
    }

    // One history entry per type change; every other field is kept for a later switch back.
    const char* typeNames[] = {"Point", "Spot", "Directional"};
    int typeIndex = static_cast<int>(light.type);

    if(ImGui::Combo("Type", &typeIndex, typeNames, IM_ARRAYSIZE(typeNames)) && !blocked &&
        typeIndex != static_cast<int>(light.type))
    {
        auto edited = light;
        edited.type = static_cast<LightType>(typeIndex);
        CommitObjectLight(scene, *selection, object.light, edited);
    }

    // The widget edits a draft live; the authored color changes once, when the gesture ends
    // (no item active), so a drag or picker session is a single history entry.
    float colorValues[3] = {m_lightColorDraft.x, m_lightColorDraft.y, m_lightColorDraft.z};
    const ImGuiColorEditFlags colorFlags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoAlpha;

    // Every type's colour is a [0, 1] tint; the photometric intensity alone sets brightness.
    if(ImGui::ColorEdit3("Color", colorValues, colorFlags) && !blocked)
    {
        m_lightColorDraft = {colorValues[0], colorValues[1], colorValues[2]};
        m_lightColorGestureActive = true;
    }

    if(m_lightColorGestureActive && !ImGui::IsAnyItemActive())
    {
        m_lightColorGestureActive = false;
        auto edited = light;
        edited.color = m_lightColorDraft;

        if(!CommitObjectLight(scene, *selection, object.light, edited))
        {
            m_lightColorDraft = light.color;
        }
    }

    if(light.type != LightType::Directional)
    {
        bool rangeAttenuation = light.rangeAttenuation;

        if(ImGui::Checkbox("Range Attenuation", &rangeAttenuation) && !blocked)
        {
            auto edited = light;
            edited.rangeAttenuation = rangeAttenuation;
            CommitObjectLight(scene, *selection, object.light, edited);
        }

        if(ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("On: fade smoothly to zero approaching the range. Off: full inverse-square light up to a "
                "hard cutoff at the range.");
        }
    }

    for(size_t index = 0; index < kLightFields.size(); ++index)
    {
        if(!IsFieldShown(light.type, index))
        {
            continue;
        }

        const auto& field = kLightFields[index];
        float& draft = m_lightDragDrafts[index];
        // Unbounded drafts: authoritative validation rejects invalid values without clamping.
        const InspectorDragSpec drag{field.speed};
        const auto result = InspectorDragFloatTransformWidth(LightFieldLabel(light.type, index), draft, drag);

        if(result == InspectorDragResult::Committed && !blocked)
        {
            if(!std::isfinite(draft))
            {
                m_lightEditStatus = "Enter a complete finite number; authored value was preserved.";
            }
            else
            {
                auto edited = light;
                edited.*field.member = field.degrees ? ToRadians(draft) : draft;
                CommitObjectLight(scene, *selection, object.light, edited);
            }
        }

        if(result == InspectorDragResult::Committed || result == InspectorDragResult::Cancelled)
        {
            draft = DisplayedLightValue(light, field);
        }
    }

    ImGui::EndDisabled();

    // Explain the renderer's actual treatment, so shadow ownership is never silent.
    if(light.type == LightType::Directional)
    {
        const auto* shadowOwner = FindDirectionalShadowOwner(owner->m_runtime->GetFileScene().GetDocument());

        if(!light.enabled)
        {
            ImGui::TextDisabled("Disabled: contributes no light.");
        }
        else if(shadowOwner && shadowOwner->id == selection->objectID)
        {
            ImGui::TextDisabled("Scene sun: ray-traced shadows and RTGI bounce.");
        }
        else
        {
            ImGui::TextDisabled("Unshadowed: an earlier Directional Light owns shadows.");
        }
    }
    else
    {
        ImGui::TextDisabled("Unshadowed direct light; visibility-tested RTGI bounce.");
    }

    if(!m_lightEditStatus.empty())
    {
        ImGui::TextWrapped("%s", m_lightEditStatus.c_str());
    }

    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
}
