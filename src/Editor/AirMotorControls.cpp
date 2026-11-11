// AIR.3: the Inspector's Air Motor section and its history entries. Follows LightControls.cpp: one complete replacement per
// history entry, validated as a whole (never clamped), numeric fields committed when their edit transaction completes.

#include "Editor/Editor.h"
#include "Editor/InspectorNumericDrag.h"
#include "DemoContent/FileBackedDemoScene.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cmath>

namespace
{
struct AirMotorNumericField
{
    const char* label;
    float speed;
    // Shown and edited in degrees, stored in radians.
    bool degrees = false;
};

// Order matches Editor::m_airMotorDragDrafts. Drag speeds only; ValidateAirMotorComponent is the sole value contract.
constexpr std::array<AirMotorNumericField, 18> kAirMotorFields{{
    {"Strength (m/s^2)", 0.5f},
    {"Falloff", 0.01f},
    {"Half Extent X (m)", 0.05f},
    {"Half Extent Y (m)", 0.05f},
    {"Half Extent Z (m)", 0.05f},
    {"Radius (m)", 0.05f},
    {"Length (m)", 0.05f},
    {"Inward Pull (m/s^2)", 0.5f},
    {"Coupling (1/s)", 0.1f},
    {"Emission Origin X (m)", 0.02f},
    {"Emission Origin Y (m)", 0.02f},
    {"Emission Origin Z (m)", 0.02f},
    {"Emission Semi-Axis X (m)", 0.02f},
    {"Emission Semi-Axis Y (m)", 0.02f},
    {"Emission Semi-Axis Z (m)", 0.02f},
    {"Emission Rotation X (deg)", 0.5f, true},
    {"Emission Rotation Y (deg)", 0.5f, true},
    {"Emission Rotation Z (deg)", 0.5f, true}
}};

enum AirMotorFieldIndex : std::size_t
{
    StrengthField,
    FalloffField,
    HalfExtentXField,
    HalfExtentYField,
    HalfExtentZField,
    RadiusField,
    LengthField,
    InwardPullField,
    CouplingField,
    EmissionOriginXField,
    EmissionOriginYField,
    EmissionOriginZField,
    EmissionSemiAxisXField,
    EmissionSemiAxisYField,
    EmissionSemiAxisZField,
    EmissionRotationXField,
    EmissionRotationYField,
    EmissionRotationZField
};

constexpr float kRadiansPerDegree = 0.0174532925f;

float& FieldOf(AirMotorComponent& motor, std::size_t field)
{
    switch(field)
    {
    case StrengthField:
        return motor.strength;
    case FalloffField:
        return motor.falloff;
    case HalfExtentXField:
        return motor.halfExtents.x;
    case HalfExtentYField:
        return motor.halfExtents.y;
    case HalfExtentZField:
        return motor.halfExtents.z;
    case RadiusField:
        return motor.radius;
    case LengthField:
        return motor.length;
    case InwardPullField:
        return motor.inwardPull;
    case EmissionOriginXField:
        return motor.emissionOrigin.x;
    case EmissionOriginYField:
        return motor.emissionOrigin.y;
    case EmissionOriginZField:
        return motor.emissionOrigin.z;
    case EmissionSemiAxisXField:
        return motor.emissionSemiAxes.x;
    case EmissionSemiAxisYField:
        return motor.emissionSemiAxes.y;
    case EmissionSemiAxisZField:
        return motor.emissionSemiAxes.z;
    case EmissionRotationXField:
        return motor.emissionRotation.x;
    case EmissionRotationYField:
        return motor.emissionRotation.y;
    case EmissionRotationZField:
        return motor.emissionRotation.z;
    default:
        return motor.coupling;
    }
}

// The field's value as the Inspector shows it (rotations in degrees).
float ValueOf(const AirMotorComponent& motor, std::size_t field)
{
    AirMotorComponent copy = motor;
    const float stored = FieldOf(copy, field);
    return kAirMotorFields[field].degrees ? stored / kRadiansPerDegree : stored;
}

// The stored value of a shown value (rotations in radians).
float StoredValue(std::size_t field, float shown)
{
    return kAirMotorFields[field].degrees ? shown * kRadiansPerDegree : shown;
}

// AIR.4D D2.1: an Omni's or Vortex's radius is its influence distance beyond the emission surface or ring, so it is named so.
const char* LabelOf(AirMotorType type, std::size_t field)
{
    const bool shaped = type == AirMotorType::Omni || type == AirMotorType::Vortex;

    if(field == RadiusField && shaped)
    {
        return "Influence Distance (m)";
    }

    return kAirMotorFields[field].label;
}

// Which fields a motor type uses; every field is kept for every type, so switching type never discards values.
bool IsFieldShown(AirMotorType type, std::size_t field)
{
    switch(field)
    {
    case StrengthField:
        return type != AirMotorType::Moving;
    case FalloffField:
        return true;
    case HalfExtentXField:
    case HalfExtentYField:
    case HalfExtentZField:
        return type == AirMotorType::Directional;
    case RadiusField:
        return type != AirMotorType::Directional;
    case LengthField:
        return type == AirMotorType::Cylinder;
    case InwardPullField:
        return type == AirMotorType::Vortex;
    case CouplingField:
        return type == AirMotorType::Moving;
    // A Moving motor emits from its object; its region follows the object.
    case EmissionOriginXField:
    case EmissionOriginYField:
    case EmissionOriginZField:
        return type != AirMotorType::Moving;
    // Directional emits along a ray and Cylinder from a disc of its radius; only Omni and Vortex have an emission shape, and a ring
    // has no third semi-axis.
    case EmissionSemiAxisXField:
    case EmissionSemiAxisYField:
    case EmissionRotationXField:
    case EmissionRotationYField:
    case EmissionRotationZField:
        return type == AirMotorType::Omni || type == AirMotorType::Vortex;
    case EmissionSemiAxisZField:
        return type == AirMotorType::Omni;
    }

    return false;
}

const char* Explanation(AirMotorType type)
{
    switch(type)
    {
    case AirMotorType::Directional:
        return "Pushes air along local +Z inside the box (negative strength pushes along -Z). Air must be in a straight line of sight "
            "from the emission ray (origin to the box's +Z face).";
    case AirMotorType::Omni:
        return "Pushes air outward along the emission ellipsoid's surface normal (negative strength pulls inward), up to the "
            "influence distance beyond the surface; air inside the ellipsoid is not driven. Air must be in a straight line of sight "
            "from an exposed part of the surface. Semi-axes of 0 make a point Omni, pushing radially from its origin.";
    case AirMotorType::Vortex:
        return "Spins air along the emission ring's tangent (counter-clockwise about its local +Z), within the influence distance of "
            "the ring, with an optional pull along the ring's inward normal. Air must be in a straight line of sight from an exposed "
            "part of the ring.";
    case AirMotorType::Moving:
        return "Drags nearby air along with the object's own motion, where the object's position sees it in a straight line.";
    case AirMotorType::Cylinder:
        return "Pushes air along local +Z inside the cylinder, like a fan or jet. Air must be in a straight line of sight from the "
            "emission disc at the origin.";
    }

    return "";
}

// AIR.4D D2: what the air field found at the motor's emission points (read back from the GPU), as a warning; empty when nothing
// needs saying. Blocked points emit nothing; unknown space (no resident bake) is free air to the solver, so it is named separately.
// AIR.4D D2.1: an Omni ellipsoid's or Vortex ring's points are its surface or ring samples; its centre emits nothing, so where the
// centre lies is never a warning (it may sit inside the object the shape surrounds).
std::string EmissionWarning(const AirMotorEmissionStatus& status, const AirMotorComponent& motor)
{
    const AirMotorType type = motor.type;
    const bool shaped = type == AirMotorType::Vortex || (type == AirMotorType::Omni && !IsPointOmni(motor));
    const char* origin = type == AirMotorType::Moving ? "The object's position" : "The emission origin";

    if(shaped)
    {
        const char* shape = type == AirMotorType::Omni ? "surface" : "ring";
        std::string warning;

        if(status.samples > 0 && status.blocked == status.samples)
        {
            return std::string("Every ") + shape + " sample is inside solid voxel geometry (or its outward step is): this motor moves "
                "little or no air. Enlarge or move the emission shape so part of it is in open air.";
        }

        if(status.blocked > 0)
        {
            warning = std::to_string(status.blocked) + " of " + std::to_string(status.samples) + " " + shape + " samples are inside "
                "solid voxel geometry and emit nothing.";
        }

        if(status.unknown > 0)
        {
            warning += warning.empty() ? "" : " ";
            warning += std::to_string(status.unknown) + " of " + std::to_string(status.samples) + " " + shape + " samples are in "
                "unknown voxel space (not baked or not resident), which air treats as free.";
        }

        return warning;
    }

    if(status.samples > 0 && status.blocked == status.samples)
    {
        return std::string("Every emission point is inside solid voxel geometry: this motor moves no air. Move the emission origin "
            "out of the geometry.");
    }

    if(status.origin == AirMotorEmissionStatus::Point::Blocked)
    {
        return std::string(origin) + " is inside solid voxel geometry; force comes only from the " +
            std::to_string(status.samples - status.blocked) + " of " + std::to_string(status.samples) + " emission points outside it.";
    }

    std::string warning;

    if(status.blocked > 0)
    {
        warning = std::to_string(status.blocked) + " of " + std::to_string(status.samples) + " emission points are inside solid voxel "
            "geometry and emit nothing.";
    }

    if(status.origin == AirMotorEmissionStatus::Point::Unknown || status.unknown > 0)
    {
        warning += warning.empty() ? "" : " ";
        warning += std::to_string(status.unknown) + " of " + std::to_string(status.samples) + " emission points are in unknown voxel "
            "space (not baked or not resident), which air treats as free.";
    }

    return warning;
}
}

void Editor::DismissAirMotorManagementPopup()
{
    if(m_airMotorManagementPopup)
    {
        CloseComponentPopup(m_airMotorManagementPopup->popupID);
    }

    m_airMotorManagementPopup.reset();
}

void Editor::ValidateAirMotorManagementPopup(IDemoScene& scene)
{
    if(!m_airMotorManagementPopup)
    {
        return;
    }

    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    SceneObjectDocument object;

    if(!ComponentPopupMatches(scene, *m_airMotorManagementPopup) || !owner ||
        !owner->CaptureLightInspection(m_airMotorManagementPopup->selection.objectID, object) || !object.airMotor)
    {
        DismissAirMotorManagementPopup();
    }
}

size_t Editor::AirMotorEditPayloadBytes(const AirMotorEdit& edit)
{
    const auto pathBytes = [](const std::filesystem::path& path)
    {
        return path.native().capacity() * sizeof(std::filesystem::path::value_type);
    };

    // Motor values are stored inline; only the strings and paths allocate.
    return sizeof(SceneHistoryEntry) + sizeof(AirMotorEdit) + edit.objectID.capacity() + edit.session.sceneIdentity.capacity() +
        pathBytes(edit.session.projectIdentity) + pathBytes(edit.session.scenePath);
}

bool Editor::CommitObjectAirMotor(IDemoScene& scene, const SceneObjectMetadata& selection, const std::optional<AirMotorComponent>& before,
    const std::optional<AirMotorComponent>& after)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(IsSceneEditingLocked() || !owner || !m_historySession || !RenameSessionMatches(scene, *m_historySession) ||
        HasPendingSceneHistoryOperation() || m_sceneOperationPending || HasBlockingObjectCreation())
    {
        m_airMotorEditStatus = "Air Motor authoring requires a stopped scene without pending edits.";
        return false;
    }

    const auto current = ResolveComponentEditTarget(scene);

    if(!current || current->objectID != selection.objectID || current->sceneID != selection.sceneID ||
        current->runtimeIdentity != selection.runtimeIdentity || current->structureRevision != selection.structureRevision ||
        current->runtimeIndex != selection.runtimeIndex)
    {
        m_airMotorEditStatus = "Air Motor inspection target is stale or blocked.";
        return false;
    }

    // The complete replacement is validated; the authored value is never clamped or partially changed.
    if(after && !ValidateAirMotorComponent(*after, &m_airMotorEditStatus))
    {
        m_airMotorEditStatus += " The authored value was preserved.";
        return false;
    }

    if(before == after)
    {
        // Still checks staleness, but a no-op never records history or dirties the scene.
        return owner->SetObjectAirMotor(selection, before, after, &m_airMotorEditStatus);
    }

    try
    {
        auto command = std::make_unique<AirMotorEdit>(AirMotorEdit{*m_historySession, selection.objectID, before, after});
        const size_t bytes = AirMotorEditPayloadBytes(*command);

        if(bytes > kSceneHistoryPayloadBytes)
        {
            m_airMotorEditStatus = "Air Motor edit exceeds the scene history payload limit.";
            return false;
        }

        SceneHistoryEntry entry{std::move(command), m_historyEpoch, bytes};
        // Reserve before mutation; appending the accepted command is noexcept.
        m_sceneHistory.reserve(kSceneHistoryCapacity);

        if(!owner->SetObjectAirMotor(selection, before, after, &m_airMotorEditStatus))
        {
            return false;
        }

        AppendSceneHistoryEntry(std::move(entry));
        m_airMotorInspection.reset();
        ++m_airMotorControlsRevision;
        m_airMotorEditStatus.clear();
        return true;
    }
    catch(const std::exception& exception)
    {
        m_airMotorEditStatus = exception.what();
        return false;
    }
}

void Editor::DrawObjectAirMotorControls(IDemoScene& scene)
{
    ValidateAirMotorManagementPopup(scene);
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

    const bool changedTarget = !m_airMotorInspection || m_airMotorInspectionEpoch != m_historyEpoch ||
        m_airMotorInspection->objectID != selection->objectID || m_airMotorInspection->runtimeIdentity != selection->runtimeIdentity ||
        m_airMotorInspection->structureRevision != selection->structureRevision;
    const bool blocked = !ResolveComponentEditTarget(scene, *selection);

    // Any owner, session, history or value change discards drafts and retires widget IDs.
    if(changedTarget || m_airMotorInspectedValue != object.airMotor || blocked)
    {
        ++m_airMotorControlsRevision;
        m_airMotorInspection = selection;
        m_airMotorInspectionEpoch = m_historyEpoch;
        m_airMotorInspectedValue = object.airMotor;

        if(changedTarget)
        {
            m_airMotorEditStatus.clear();
        }

        if(object.airMotor)
        {
            for(std::size_t field = 0; field < kAirMotorFields.size(); ++field)
            {
                m_airMotorDragDrafts[field] = ValueOf(*object.airMotor, field);
            }
        }
    }

    if(!object.airMotor)
    {
        m_airMotorDragDrafts.fill(0.0f);
        return;
    }

    ImGui::PushID(static_cast<int>(m_historyEpoch));
    ImGui::PushID(selection->objectID.c_str());
    ImGui::PushID("AirMotor");
    ImGui::Separator();
    ImGui::TextUnformatted("Air Motor");
    ImGui::SameLine();
    ImGui::BeginDisabled(blocked);

    if(ImGui::SmallButton("..."))
    {
        m_airMotorManagementPopup = ComponentPopupTarget{*m_historySession, *selection, m_historyEpoch, ImGui::GetID("AirMotorManagement")};
        ImGui::OpenPopup("AirMotorManagement");
    }

    ImGui::EndDisabled();
    bool removed = false;

    if(ImGui::BeginPopup("AirMotorManagement"))
    {
        ValidateAirMotorManagementPopup(scene);
        ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);

        if(m_airMotorManagementPopup && ImGui::MenuItem("Remove Component"))
        {
            ValidateAirMotorManagementPopup(scene);
            const auto current = ResolveComponentEditTarget(scene);
            SceneObjectDocument latest;

            // Only the Air Motor is removed; Transform and every other component are preserved.
            if(m_airMotorManagementPopup && current && owner->CaptureLightInspection(current->objectID, latest) && latest.airMotor &&
                CommitObjectAirMotor(scene, *current, latest.airMotor, std::nullopt))
            {
                DismissAirMotorManagementPopup();
                m_airMotorDragDrafts.fill(0.0f);
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
    ImGui::PushID(static_cast<int>(m_airMotorControlsRevision));
    ImGui::BeginDisabled(blocked);
    const AirMotorComponent motor = *object.airMotor;
    bool enabled = motor.enabled;

    if(ImGui::Checkbox("Enabled", &enabled) && !blocked)
    {
        auto edited = motor;
        edited.enabled = enabled;
        CommitObjectAirMotor(scene, *selection, object.airMotor, edited);
    }

    // One history entry per type change; every other field is kept for a later switch back.
    const char* typeNames[] = {"Directional", "Omni", "Vortex", "Moving", "Cylinder"};
    int typeIndex = static_cast<int>(motor.type);

    if(ImGui::Combo("Type", &typeIndex, typeNames, IM_ARRAYSIZE(typeNames)) && !blocked && typeIndex != static_cast<int>(motor.type))
    {
        auto edited = motor;
        edited.type = static_cast<AirMotorType>(typeIndex);
        CommitObjectAirMotor(scene, *selection, object.airMotor, edited);
    }

    for(std::size_t field = 0; field < kAirMotorFields.size(); ++field)
    {
        if(!IsFieldShown(motor.type, field))
        {
            continue;
        }

        float& draft = m_airMotorDragDrafts[field];
        // Unbounded drafts: authoritative validation rejects invalid values without clamping.
        const InspectorDragSpec drag{kAirMotorFields[field].speed};
        const auto result = InspectorDragFloatTransformWidth(LabelOf(motor.type, field), draft, drag);

        if(result == InspectorDragResult::Committed && !blocked)
        {
            if(!std::isfinite(draft))
            {
                m_airMotorEditStatus = "Enter a complete finite number; authored value was preserved.";
            }
            else
            {
                auto edited = motor;
                FieldOf(edited, field) = StoredValue(field, draft);
                CommitObjectAirMotor(scene, *selection, object.airMotor, edited);
            }
        }

        if(result == InspectorDragResult::Committed || result == InspectorDragResult::Cancelled)
        {
            draft = ValueOf(motor, field);
        }
    }

    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", motor.enabled ? Explanation(motor.type) : "Disabled: moves no air.");

    if(motor.enabled)
    {
        const auto status = m_airMotorEmission.find(AirMotorTag(selection->objectID));

        if(status == m_airMotorEmission.end())
        {
            ImGui::TextDisabled("Emission check: needs Simulate air, with the motor reaching the air window.");
        }
        else if(const std::string warning = EmissionWarning(status->second, motor); !warning.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.25f, 1.0f));
            ImGui::TextWrapped("%s", warning.c_str());
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::TextDisabled("Emission: all %u points in open air.", status->second.samples);
        }
    }

    if(!m_airMotorEditStatus.empty())
    {
        ImGui::TextWrapped("%s", m_airMotorEditStatus.c_str());
    }

    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
}
