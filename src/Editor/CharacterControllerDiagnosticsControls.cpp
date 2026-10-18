#include "Editor/Editor.h"
#include "DemoContent/FileBackedDemoScene.h"

#include <imgui.h>
#include "ImGuizmo.h"

#include <cmath>

// CM.9 Editor-only Character Controller diagnostics: read-only runtime display, and an
// explicitly activated movement drive that feeds the production fixed-step command API.
// The drive is a debugging aid, not a gameplay input binding.

bool Editor::ViewportOwnsKeyboard() const
{
    const ImGuiIO& io = ImGui::GetIO();
    return m_viewportFocused && !m_openSceneContext && !m_viewportRightGesture &&
        !io.WantTextInput && !io.WantCaptureKeyboard && !ImGui::IsAnyItemActive() &&
        !m_cameraLooking && !ImGuizmo::IsUsing() && !ImGui::IsMouseDown(ImGuiMouseButton_Right) &&
        m_viewportImage.size.x > 0.0f && m_viewportImage.size.y > 0.0f &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

void Editor::SetCharacterDiagnostics(const Physics::CharacterDiagnosticsFrame* frame)
{
    if(!frame)
    {
        m_characterDiagnostics.reset();

        if(m_characterDrive)
        {
            DisableCharacterDrive("Diagnostic drive stopped: no active physics session.");
        }

        return;
    }

    m_characterDiagnostics = *frame;

    if(m_characterDrive && (m_characterDrive->sessionGeneration != frame->sessionGeneration ||
        !frame->Find(m_characterDrive->objectID)))
    {
        DisableCharacterDrive("Diagnostic drive stopped: the session or controller changed.");
    }
}

void Editor::DisableCharacterDrive(std::string reason)
{
    m_characterDrive.reset();
    m_characterDriveStatus = std::move(reason);
}

void Editor::UpdateCharacterDriveInput(const IDemoScene& scene)
{
    if(!m_characterDrive)
    {
        return;
    }

    // The drive follows one selected controller in one session; anything else disables it.
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
    const auto selection = owner ? owner->GetObjectMetadata(m_selectedObject) : std::nullopt;

    if(m_physicsState != PhysicsState::Playing || !selection || selection->objectID != m_characterDrive->objectID ||
        !m_characterDiagnostics || m_characterDiagnostics->sessionGeneration != m_characterDrive->sessionGeneration)
    {
        DisableCharacterDrive("Diagnostic drive stopped: Play ended or the selection changed.");
        return;
    }

    // Held keys are sampled every frame; without keyboard ownership nothing is held, so focus
    // loss or typing stops movement at once and nothing is queued.
    auto& drive = *m_characterDrive;
    drive.velocity = {};

    if(!ViewportOwnsKeyboard())
    {
        return;
    }

    // World-relative: Right/Left = +X/-X, Down/Up = +Z/-Z. Independent of the camera.
    const auto held = [](ImGuiKey key)
    {
        return ImGui::IsKeyDown(key) ? 1.0f : 0.0f;
    };
    const float x = held(ImGuiKey_RightArrow) - held(ImGuiKey_LeftArrow);
    const float z = held(ImGuiKey_DownArrow) - held(ImGuiKey_UpArrow);
    const float length = std::sqrt(x * x + z * z);

    if(length > 0.0f)
    {
        drive.velocity = {x / length * drive.maximumSpeed, 0.0f, z / length * drive.maximumSpeed};
    }

    // A jump is requested only on the key transition and stays pending until a frame runs a step.
    if(ImGui::IsKeyPressed(ImGuiKey_Space, false))
    {
        drive.jumpPending = true;
    }
}

std::optional<CharacterDriveIntent> Editor::TakeCharacterDriveIntent() const
{
    if(!m_characterDrive)
    {
        return std::nullopt;
    }

    return CharacterDriveIntent{m_characterDrive->objectID, m_characterDrive->sessionGeneration,
        m_characterDrive->velocity, m_characterDrive->jumpPending};
}

void Editor::AcknowledgeCharacterDriveJump()
{
    if(m_characterDrive)
    {
        m_characterDrive->jumpPending = false;
    }
}

void Editor::DrawCharacterDriveToggle()
{
    if(m_physicsState != PhysicsState::Playing)
    {
        return;
    }

    ImGui::SameLine();
    bool driving = m_characterDrive.has_value();
    ImGui::BeginDisabled(!driving && (!m_characterDriveCandidate || !m_characterDiagnostics));

    if(ImGui::Checkbox("Drive Controller", &driving))
    {
        if(driving && m_characterDriveCandidate && m_characterDiagnostics)
        {
            m_characterDrive = CharacterDrive{m_characterDriveCandidate->objectID,
                m_characterDiagnostics->sessionGeneration, m_characterDriveCandidate->maximumSpeed};
            m_characterDriveStatus.clear();
        }
        else
        {
            DisableCharacterDrive("Diagnostic drive disabled.");
        }
    }

    ImGui::EndDisabled();

    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Editor diagnostic: drives the selected Character Controller with the arrow keys "
            "(world -Z/+Z/-X/+X) and Space while the viewport has keyboard focus. "
            "Click once to focus without selecting; subsequent clicks select normally. Not a gameplay control.");
    }

    if(m_characterDrive)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.0f, 1.0f), "DRIVING %s", m_characterDrive->objectID.c_str());
    }
}

void Editor::DrawCharacterControllerDiagnostics(const SceneObjectMetadata& selection,
    const Physics::CharacterControllerComponent& component)
{
    if(m_physicsState != PhysicsState::Playing)
    {
        return;
    }

    // Offer this controller to the Play-bar drive toggle (read on the next frame).
    m_characterDriveCandidate = CharacterDriveCandidate{selection.objectID, component.maximumHorizontalSpeed};
    ImGui::SeparatorText("Runtime Diagnostics (Play, read-only)");
    const auto* diagnostics = m_characterDiagnostics ? m_characterDiagnostics->Find(selection.objectID) : nullptr;

    if(!diagnostics)
    {
        ImGui::TextUnformatted("Unavailable: no published runtime state for this controller.");
        return;
    }

    const auto vector = [](const Vector3& value)
    {
        return std::to_string(value.x) + ", " + std::to_string(value.y) + ", " + std::to_string(value.z);
    };
    const auto step = [](const std::optional<std::uint64_t>& value)
    {
        return value ? std::to_string(*value) : std::string("none");
    };
    const char* statusNames[] = {"No movement", "Unobstructed", "Constrained", "Query capacity exceeded",
        "Penetration rejected"};
    const char* jumpNames[] = {"none", "accepted", "ignored (airborne)", "ignored (zero speed)"};
    const char* recoveryNames[] = {"not needed", "recovered", "UNRESOLVED (pose kept)"};

    ImGui::Text("Session %llu, step %llu", static_cast<unsigned long long>(m_characterDiagnostics->sessionGeneration),
        static_cast<unsigned long long>(m_characterDiagnostics->completedSteps));
    ImGui::Text("State: %s", diagnostics->ground.grounded ? "Grounded" : "Airborne");
    ImGui::Text("Runtime position: %s", vector(diagnostics->pose.position).c_str());
    ImGui::Text("Vertical velocity: %.3f m/s", diagnostics->verticalVelocity);

    if(diagnostics->ground.grounded)
    {
        ImGui::Text("Ground normal: %s", vector(diagnostics->ground.normal).c_str());
        ImGui::Text("Ground support: %s", diagnostics->groundObjectID.empty() ? "(controller or unregistered)" :
            diagnostics->groundObjectID.c_str());
    }

    ImGui::Text("Last commands: move step %s, jump step %s", step(diagnostics->lastMovementCommandStep).c_str(),
        step(diagnostics->lastJumpRequestStep).c_str());
    ImGui::Text("Last result: %s; jump %s%s%s", statusNames[static_cast<int>(diagnostics->lastStatus)],
        jumpNames[static_cast<int>(diagnostics->lastJumpOutcome)], diagnostics->stepped ? "; stepped" : "",
        diagnostics->snapped ? "; snapped" : "");
    ImGui::Text("Moving bodies: %s", recoveryNames[static_cast<int>(diagnostics->recovery)]);

    if(!diagnostics->carrierObjectID.empty())
    {
        ImGui::Text("Carried by %s: %s", diagnostics->carrierObjectID.c_str(),
            vector(diagnostics->carriedDisplacement).c_str());
    }

    ImGui::Text("Contacts: %zu (showing %zu)", diagnostics->contactCount, diagnostics->contacts.size());
    ImGui::Text("Trigger observer: %s; owned Trigger Collider: %s",
        diagnostics->observerRegistered ? "registered" : "missing",
        diagnostics->ownsTriggerCollider ? "follows runtime pose" : "none");

    if(!m_characterDriveStatus.empty())
    {
        ImGui::TextWrapped("%s", m_characterDriveStatus.c_str());
    }
}
