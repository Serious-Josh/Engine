// AIR.4F F2: the Inspector's Mist Emitter section and its history entries. Follows AirMotorControls.cpp: one complete replacement
// per history entry, validated as a whole (never clamped), numeric fields committed when their edit transaction completes.

#include "Editor/Editor.h"
#include "Editor/InspectorNumericDrag.h"
#include "DemoContent/FileBackedDemoScene.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cmath>

namespace
{
struct MistEmitterNumericField
{
    const char* label;
    float speed;
    // Shown and edited in degrees, stored in radians.
    bool degrees = false;
};

// Order matches Editor::m_mistEmitterDragDrafts. Drag speeds only; ValidateMistEmitterComponent is the sole value contract.
constexpr std::array<MistEmitterNumericField, 15> kMistEmitterFields{{
    {"Emission Origin X (m)", 0.02f},
    {"Emission Origin Y (m)", 0.02f},
    {"Emission Origin Z (m)", 0.02f},
    {"Emission Rotation X (deg)", 0.5f, true},
    {"Emission Rotation Y (deg)", 0.5f, true},
    {"Emission Rotation Z (deg)", 0.5f, true},
    {"Radius (m)", 0.02f},
    {"Half Extent X (m)", 0.02f},
    {"Half Extent Y (m)", 0.02f},
    {"Half Extent Z (m)", 0.02f},
    {"Rate (density/s)", 0.05f},
    {"Colour R", 0.01f},
    {"Colour G", 0.01f},
    {"Colour B", 0.01f},
    {"Density", 0.02f}
}};

enum MistEmitterFieldIndex : std::size_t
{
    OriginXField,
    OriginYField,
    OriginZField,
    RotationXField,
    RotationYField,
    RotationZField,
    RadiusField,
    HalfExtentXField,
    HalfExtentYField,
    HalfExtentZField,
    RateField,
    ColorRField,
    ColorGField,
    ColorBField,
    DensityField
};

constexpr float kRadiansPerDegree = 0.0174532925f;

float& FieldOf(MistEmitterComponent& emitter, std::size_t field)
{
    switch(field)
    {
    case OriginXField:
        return emitter.emissionOrigin.x;
    case OriginYField:
        return emitter.emissionOrigin.y;
    case OriginZField:
        return emitter.emissionOrigin.z;
    case RotationXField:
        return emitter.emissionRotation.x;
    case RotationYField:
        return emitter.emissionRotation.y;
    case RotationZField:
        return emitter.emissionRotation.z;
    case RadiusField:
        return emitter.radius;
    case HalfExtentXField:
        return emitter.halfExtents.x;
    case HalfExtentYField:
        return emitter.halfExtents.y;
    case HalfExtentZField:
        return emitter.halfExtents.z;
    case RateField:
        return emitter.rate;
    case ColorRField:
        return emitter.color.x;
    case ColorGField:
        return emitter.color.y;
    case ColorBField:
        return emitter.color.z;
    default:
        return emitter.density;
    }
}

// The field's value as the Inspector shows it (rotations in degrees).
float ValueOf(const MistEmitterComponent& emitter, std::size_t field)
{
    MistEmitterComponent copy = emitter;
    const float stored = FieldOf(copy, field);
    return kMistEmitterFields[field].degrees ? stored / kRadiansPerDegree : stored;
}

float StoredValue(std::size_t field, float shown)
{
    return kMistEmitterFields[field].degrees ? shown * kRadiansPerDegree : shown;
}

// Every field is kept for both shapes, so switching shape never discards values; only the shape's own size is shown.
bool IsFieldShown(MistEmitterShape shape, std::size_t field)
{
    switch(field)
    {
    case RadiusField:
        return shape == MistEmitterShape::Sphere;
    case HalfExtentXField:
    case HalfExtentYField:
    case HalfExtentZField:
        return shape == MistEmitterShape::Box;
    default:
        return true;
    }
}

// What the air field found at the emitter's origin and how far its mist went (read back from the GPU), or why there is no status.
std::string StatusText(const MistEmitterStatus& status, bool& warning)
{
    warning = true;

    switch(status.origin)
    {
    case MistEmitterStatus::Origin::Solid:
        return "The emission origin is inside solid voxel geometry: this emitter emits nothing. Move the origin into open air.";
    case MistEmitterStatus::Origin::Outside:
        return "The emission origin is outside the air window: this emitter emits nothing until the window reaches it.";
    case MistEmitterStatus::Origin::Unknown:
        return "The emission origin is in unknown voxel space (not baked or not resident), which air treats as free; emitting into " +
            std::to_string(status.pieces) + " fluid pieces.";
    case MistEmitterStatus::Origin::Open:
        break;
    }

    warning = status.pieces == 0;

    if(warning)
    {
        return "The origin is in open air, but no fluid piece in the shape is in its straight line of sight: nothing is emitted.";
    }

    return "Emitting into " + std::to_string(status.pieces) + " fluid pieces in the origin's line of sight.";
}
}

void Editor::DismissMistEmitterManagementPopup()
{
    if(m_mistEmitterManagementPopup)
    {
        CloseComponentPopup(m_mistEmitterManagementPopup->popupID);
    }

    m_mistEmitterManagementPopup.reset();
}

void Editor::ValidateMistEmitterManagementPopup(IDemoScene& scene)
{
    if(!m_mistEmitterManagementPopup)
    {
        return;
    }

    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    SceneObjectDocument object;

    if(!ComponentPopupMatches(scene, *m_mistEmitterManagementPopup) || !owner ||
        !owner->CaptureLightInspection(m_mistEmitterManagementPopup->selection.objectID, object) || !object.mistEmitter)
    {
        DismissMistEmitterManagementPopup();
    }
}

size_t Editor::MistEmitterEditPayloadBytes(const MistEmitterEdit& edit)
{
    const auto pathBytes = [](const std::filesystem::path& path)
    {
        return path.native().capacity() * sizeof(std::filesystem::path::value_type);
    };

    // Emitter values are stored inline; only the strings and paths allocate.
    return sizeof(SceneHistoryEntry) + sizeof(MistEmitterEdit) + edit.objectID.capacity() + edit.session.sceneIdentity.capacity() +
        pathBytes(edit.session.projectIdentity) + pathBytes(edit.session.scenePath);
}

bool Editor::CommitObjectMistEmitter(IDemoScene& scene, const SceneObjectMetadata& selection,
    const std::optional<MistEmitterComponent>& before, const std::optional<MistEmitterComponent>& after)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(IsSceneEditingLocked() || !owner || !m_historySession || !RenameSessionMatches(scene, *m_historySession) ||
        HasPendingSceneHistoryOperation() || m_sceneOperationPending || HasBlockingObjectCreation())
    {
        m_mistEmitterEditStatus = "Mist Emitter authoring requires a stopped scene without pending edits.";
        return false;
    }

    const auto current = ResolveComponentEditTarget(scene);

    if(!current || current->objectID != selection.objectID || current->sceneID != selection.sceneID ||
        current->runtimeIdentity != selection.runtimeIdentity || current->structureRevision != selection.structureRevision ||
        current->runtimeIndex != selection.runtimeIndex)
    {
        m_mistEmitterEditStatus = "Mist Emitter inspection target is stale or blocked.";
        return false;
    }

    // The complete replacement is validated; the authored value is never clamped or partially changed.
    if(after && !ValidateMistEmitterComponent(*after, &m_mistEmitterEditStatus))
    {
        m_mistEmitterEditStatus += " The authored value was preserved.";
        return false;
    }

    if(before == after)
    {
        // Still checks staleness, but a no-op never records history or dirties the scene.
        return owner->SetObjectMistEmitter(selection, before, after, &m_mistEmitterEditStatus);
    }

    try
    {
        auto command = std::make_unique<MistEmitterEdit>(MistEmitterEdit{*m_historySession, selection.objectID, before, after});
        const size_t bytes = MistEmitterEditPayloadBytes(*command);

        if(bytes > kSceneHistoryPayloadBytes)
        {
            m_mistEmitterEditStatus = "Mist Emitter edit exceeds the scene history payload limit.";
            return false;
        }

        SceneHistoryEntry entry{std::move(command), m_historyEpoch, bytes};
        // Reserve before mutation; appending the accepted command is noexcept.
        m_sceneHistory.reserve(kSceneHistoryCapacity);

        if(!owner->SetObjectMistEmitter(selection, before, after, &m_mistEmitterEditStatus))
        {
            return false;
        }

        AppendSceneHistoryEntry(std::move(entry));
        m_mistEmitterInspection.reset();
        ++m_mistEmitterControlsRevision;
        m_mistEmitterEditStatus.clear();
        return true;
    }
    catch(const std::exception& exception)
    {
        m_mistEmitterEditStatus = exception.what();
        return false;
    }
}

void Editor::DrawObjectMistEmitterControls(IDemoScene& scene)
{
    ValidateMistEmitterManagementPopup(scene);
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

    const bool changedTarget = !m_mistEmitterInspection || m_mistEmitterInspectionEpoch != m_historyEpoch ||
        m_mistEmitterInspection->objectID != selection->objectID || m_mistEmitterInspection->runtimeIdentity != selection->runtimeIdentity ||
        m_mistEmitterInspection->structureRevision != selection->structureRevision;
    const bool blocked = !ResolveComponentEditTarget(scene, *selection);

    // Any owner, session, history or value change discards drafts and retires widget IDs.
    if(changedTarget || m_mistEmitterInspectedValue != object.mistEmitter || blocked)
    {
        ++m_mistEmitterControlsRevision;
        m_mistEmitterInspection = selection;
        m_mistEmitterInspectionEpoch = m_historyEpoch;
        m_mistEmitterInspectedValue = object.mistEmitter;

        if(changedTarget)
        {
            m_mistEmitterEditStatus.clear();
        }

        if(object.mistEmitter)
        {
            for(std::size_t field = 0; field < kMistEmitterFields.size(); ++field)
            {
                m_mistEmitterDragDrafts[field] = ValueOf(*object.mistEmitter, field);
            }
        }
    }

    if(!object.mistEmitter)
    {
        m_mistEmitterDragDrafts.fill(0.0f);
        return;
    }

    ImGui::PushID(static_cast<int>(m_historyEpoch));
    ImGui::PushID(selection->objectID.c_str());
    ImGui::PushID("MistEmitter");
    ImGui::Separator();
    ImGui::TextUnformatted("Mist Emitter");
    ImGui::SameLine();
    ImGui::BeginDisabled(blocked);

    if(ImGui::SmallButton("..."))
    {
        m_mistEmitterManagementPopup = ComponentPopupTarget{*m_historySession, *selection, m_historyEpoch, ImGui::GetID("MistEmitterManagement")};
        ImGui::OpenPopup("MistEmitterManagement");
    }

    ImGui::EndDisabled();
    bool removed = false;

    if(ImGui::BeginPopup("MistEmitterManagement"))
    {
        ValidateMistEmitterManagementPopup(scene);
        ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);

        if(m_mistEmitterManagementPopup && ImGui::MenuItem("Remove Component"))
        {
            ValidateMistEmitterManagementPopup(scene);
            const auto current = ResolveComponentEditTarget(scene);
            SceneObjectDocument latest;

            // Only the Mist Emitter is removed; Transform and every other component are preserved.
            if(m_mistEmitterManagementPopup && current && owner->CaptureLightInspection(current->objectID, latest) && latest.mistEmitter &&
                CommitObjectMistEmitter(scene, *current, latest.mistEmitter, std::nullopt))
            {
                DismissMistEmitterManagementPopup();
                m_mistEmitterDragDrafts.fill(0.0f);
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
    ImGui::PushID(static_cast<int>(m_mistEmitterControlsRevision));
    ImGui::BeginDisabled(blocked);
    const MistEmitterComponent emitter = *object.mistEmitter;
    bool enabled = emitter.enabled;

    if(ImGui::Checkbox("Enabled", &enabled) && !blocked)
    {
        auto edited = emitter;
        edited.enabled = enabled;
        CommitObjectMistEmitter(scene, *selection, object.mistEmitter, edited);
    }

    const char* shapeNames[] = {"Sphere", "Box"};
    int shapeIndex = static_cast<int>(emitter.shape);

    if(ImGui::Combo("Shape", &shapeIndex, shapeNames, IM_ARRAYSIZE(shapeNames)) && !blocked && shapeIndex != static_cast<int>(emitter.shape))
    {
        auto edited = emitter;
        edited.shape = static_cast<MistEmitterShape>(shapeIndex);
        CommitObjectMistEmitter(scene, *selection, object.mistEmitter, edited);
    }

    for(std::size_t field = 0; field < kMistEmitterFields.size(); ++field)
    {
        if(!IsFieldShown(emitter.shape, field))
        {
            continue;
        }

        float& draft = m_mistEmitterDragDrafts[field];
        // Unbounded drafts: authoritative validation rejects invalid values without clamping.
        const InspectorDragSpec drag{kMistEmitterFields[field].speed};
        const auto result = InspectorDragFloatTransformWidth(kMistEmitterFields[field].label, draft, drag);

        if(result == InspectorDragResult::Committed && !blocked)
        {
            if(!std::isfinite(draft))
            {
                m_mistEmitterEditStatus = "Enter a complete finite number; authored value was preserved.";
            }
            else
            {
                auto edited = emitter;
                FieldOf(edited, field) = StoredValue(field, draft);
                CommitObjectMistEmitter(scene, *selection, object.mistEmitter, edited);
            }
        }

        if(result == InspectorDragResult::Committed || result == InspectorDragResult::Cancelled)
        {
            draft = ValueOf(emitter, field);
        }
    }

    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", emitter.enabled ? "Adds mist (density = rate x density per second, in its colour) to air inside the shape "
        "that the emission origin sees in a straight line; solid geometry is never crossed." : "Disabled: emits no mist.");

    if(emitter.enabled)
    {
        const auto status = m_mistEmitterStatus.find(MistEmitterTag(selection->objectID));

        if(status == m_mistEmitterStatus.end())
        {
            ImGui::TextDisabled("Emission check: needs Simulate air, the emitter in the air window, and at most 32 emitters.");
        }
        else
        {
            bool warning = false;
            const std::string text = StatusText(status->second, warning);

            if(warning)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.25f, 1.0f));
                ImGui::TextWrapped("%s", text.c_str());
                ImGui::PopStyleColor();
            }
            else
            {
                ImGui::TextDisabled("%s", text.c_str());
            }
        }
    }

    if(!m_mistEmitterEditStatus.empty())
    {
        ImGui::TextWrapped("%s", m_mistEmitterEditStatus.c_str());
    }

    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
}
