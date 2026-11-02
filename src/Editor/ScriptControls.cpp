#include "Editor/Editor.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "assets/ProjectScriptAssets.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cctype>

namespace
{
    std::string Lowercase(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });

        return text;
    }

    // Display text for a script reference: its project path, or why it cannot be resolved.
    std::string DescribeScript(const std::string& reference, const ProjectAssetRegistry& registry)
    {
        if(reference.empty())
        {
            return "None";
        }

        const auto id = AssetID::ParseReference(reference);
        const auto* record = id ? registry.Find(*id) : nullptr;

        if(!record || record->type != AssetType::Script)
        {
            return "Missing script (" + reference + ")";
        }

        return record->relativePath.generic_string();
    }
}

void Editor::DismissScriptManagementPopup()
{
    if(m_scriptManagementPopup)
    {
        CloseComponentPopup(m_scriptManagementPopup->popupID);
    }

    m_scriptManagementPopup.reset();
}

void Editor::ValidateScriptManagementPopup(IDemoScene& scene)
{
    if(!m_scriptManagementPopup)
    {
        return;
    }

    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    SceneObjectDocument object;

    if(!ComponentPopupMatches(scene, *m_scriptManagementPopup) || !owner ||
        !owner->CaptureLightInspection(m_scriptManagementPopup->selection.objectID, object) || !object.script)
    {
        DismissScriptManagementPopup();
    }
}

size_t Editor::ScriptEditPayloadBytes(const ScriptEdit& edit)
{
    const auto pathBytes = [](const std::filesystem::path& path)
    {
        return path.native().capacity() * sizeof(std::filesystem::path::value_type);
    };

    const auto scriptBytes = [](const std::optional<ScriptComponent>& script)
    {
        return script ? script->scriptReference.capacity() + script->className.capacity() : 0;
    };

    return sizeof(SceneHistoryEntry) + sizeof(ScriptEdit) + edit.objectID.capacity() +
        edit.session.sceneIdentity.capacity() + pathBytes(edit.session.projectIdentity) +
        pathBytes(edit.session.scenePath) + scriptBytes(edit.before) + scriptBytes(edit.after);
}

bool Editor::CommitObjectScript(IDemoScene& scene, const SceneObjectMetadata& selection,
    const std::optional<ScriptComponent>& before, const std::optional<ScriptComponent>& after)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(IsSceneEditingLocked() || !owner || !m_historySession ||
        !RenameSessionMatches(scene, *m_historySession) || HasPendingSceneHistoryOperation() ||
        m_sceneOperationPending || HasBlockingObjectCreation())
    {
        m_scriptEditStatus = "Script authoring requires a stopped scene without pending edits.";
        return false;
    }

    const auto current = ResolveComponentEditTarget(scene);

    if(!current || current->objectID != selection.objectID || current->sceneID != selection.sceneID ||
        current->runtimeIdentity != selection.runtimeIdentity || current->structureRevision != selection.structureRevision ||
        current->runtimeIndex != selection.runtimeIndex)
    {
        m_scriptEditStatus = "Script inspection target is stale or blocked.";
        return false;
    }

    // The complete replacement is validated; the authored value is never partially changed.
    if(after && !ValidateScriptComponent(*after, &m_scriptEditStatus))
    {
        m_scriptEditStatus += " The authored value was preserved.";
        return false;
    }

    if(before == after)
    {
        // Still checks staleness, but a no-op never records history or dirties the scene.
        return owner->SetObjectScript(selection, before, after, &m_scriptEditStatus);
    }

    try
    {
        auto command = std::make_unique<ScriptEdit>(ScriptEdit{*m_historySession, selection.objectID, before, after});
        const size_t bytes = ScriptEditPayloadBytes(*command);

        if(bytes > kSceneHistoryPayloadBytes)
        {
            m_scriptEditStatus = "Script edit exceeds the scene history payload limit.";
            return false;
        }

        SceneHistoryEntry entry{std::move(command), m_historyEpoch, bytes};
        // Reserve before mutation; appending the accepted command is noexcept.
        m_sceneHistory.reserve(kSceneHistoryCapacity);

        if(!owner->SetObjectScript(selection, before, after, &m_scriptEditStatus))
        {
            return false;
        }

        AppendSceneHistoryEntry(std::move(entry));
        m_scriptInspection.reset();
        ++m_scriptControlsRevision;
        m_scriptEditStatus.clear();
        return true;
    }
    catch(const std::exception& exception)
    {
        m_scriptEditStatus = exception.what();
        return false;
    }
}

void Editor::RefreshScriptClasses(const std::string& reference, const ProjectPaths& paths,
    const ProjectAssetRegistry& registry)
{
    ScriptClassList list;
    list.reference = reference;
    const auto id = AssetID::ParseReference(reference);

    if(!id)
    {
        m_scriptClasses = std::move(list);
        return;
    }

    if(!m_scriptValidator)
    {
        m_scriptValidator = std::make_unique<Scripting::ScriptHost>();

        if(m_scriptValidator->Initialize().status != Scripting::Status::Ready)
        {
            m_scriptValidator.reset();
            list.error = "The script validator could not be initialized.";
            m_scriptClasses = std::move(list);
            return;
        }
    }

    // Validation compiles the saved source closure without running any script code.
    const auto result = ProjectScriptAssets::Validate(*m_scriptValidator, paths, registry, *id);

    if(result.status == Scripting::Status::Compiled)
    {
        list.classes = result.classes;
    }
    else if(!result.diagnostics.empty())
    {
        // The first error, not AngelScript's preceding informational "Compiling ..." line.
        const auto error = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
            [](const Scripting::Diagnostic& diagnostic) { return diagnostic.severity == "Error"; });
        const auto& first = error != result.diagnostics.end() ? *error : result.diagnostics.front();
        list.error = first.section + (first.line > 0 ? "(" + std::to_string(first.line) + "): " : ": ") + first.text;
    }
    else
    {
        list.error = "The script failed validation.";
    }

    m_scriptClasses = std::move(list);
}

void Editor::DrawScriptPickerContents(IDemoScene& scene, const SceneObjectMetadata& selection,
    const ScriptComponent& script, const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    ImGui::TextUnformatted("Select script");
    ImGui::SameLine();

    if(ImGui::Button("Close"))
    {
        ImGui::CloseCurrentPopup();
        return;
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##ScriptSearch", "Search scripts...", m_scriptSearch.data(), m_scriptSearch.size());
    const std::string query = Lowercase(m_scriptSearch.data());
    std::optional<std::string> chosen;

    if(ImGui::Selectable("None", script.scriptReference.empty(), ImGuiSelectableFlags_DontClosePopups))
    {
        chosen = std::string();
    }

    ImGui::Separator();
    auto scripts = registry.GetAssets(AssetType::Script);
    std::sort(scripts.begin(), scripts.end(), [](const AssetRecord& left, const AssetRecord& right)
    {
        return left.relativePath < right.relativePath;
    });

    if(scripts.empty())
    {
        ImGui::TextDisabled("No scripts in Content/Scripts. Add a .as file and press Refresh in the Asset Browser.");
    }

    for(const auto& record : scripts)
    {
        const std::string path = record.relativePath.generic_string();

        if(!query.empty() && Lowercase(path).find(query) == std::string::npos)
        {
            continue;
        }

        const std::string reference = record.id.ToReference();

        if(ImGui::Selectable(path.c_str(), reference == script.scriptReference, ImGuiSelectableFlags_DontClosePopups))
        {
            chosen = reference;
        }
    }

    if(!chosen)
    {
        return;
    }

    // One edit assigns the script and, when it defines exactly one EAScript class, that class.
    ScriptComponent edited = script;
    edited.scriptReference = *chosen;
    RefreshScriptClasses(edited.scriptReference, paths, registry);
    const auto& classes = m_scriptClasses->classes;

    if(classes.size() == 1)
    {
        edited.className = classes.front();
    }
    else if(std::find(classes.begin(), classes.end(), edited.className) == classes.end())
    {
        edited.className.clear();
    }

    if(CommitObjectScript(scene, selection, script, edited))
    {
        ImGui::CloseCurrentPopup();
    }
}

void Editor::DrawObjectScriptControls(IDemoScene& scene, const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    ValidateScriptManagementPopup(scene);
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

    const bool changedTarget = !m_scriptInspection || m_scriptInspectionEpoch != m_historyEpoch ||
        m_scriptInspection->objectID != selection->objectID ||
        m_scriptInspection->runtimeIdentity != selection->runtimeIdentity ||
        m_scriptInspection->structureRevision != selection->structureRevision;
    const bool blocked = !ResolveComponentEditTarget(scene, *selection);

    if(changedTarget || m_scriptInspectedValue != object.script || blocked)
    {
        ++m_scriptControlsRevision;
        m_scriptInspection = selection;
        m_scriptInspectionEpoch = m_historyEpoch;
        m_scriptInspectedValue = object.script;

        if(changedTarget)
        {
            m_scriptEditStatus.clear();
        }

        // A newly inspected script is validated once to list its classes.
        if(object.script && (!m_scriptClasses || m_scriptClasses->reference != object.script->scriptReference))
        {
            RefreshScriptClasses(object.script->scriptReference, paths, registry);
        }
    }

    if(!object.script)
    {
        if(!m_scriptEditStatus.empty())
        {
            ImGui::TextWrapped("%s", m_scriptEditStatus.c_str());
        }

        return;
    }

    ImGui::PushID(static_cast<int>(m_historyEpoch));
    ImGui::PushID(selection->objectID.c_str());
    ImGui::PushID("Script");
    ImGui::Separator();
    ImGui::TextUnformatted("Script");
    ImGui::SameLine();
    ImGui::BeginDisabled(blocked);

    if(ImGui::SmallButton("..."))
    {
        m_scriptManagementPopup = ComponentPopupTarget{
            *m_historySession, *selection, m_historyEpoch, ImGui::GetID("ScriptManagement")};
        ImGui::OpenPopup("ScriptManagement");
    }

    ImGui::EndDisabled();
    bool removed = false;

    if(ImGui::BeginPopup("ScriptManagement"))
    {
        ValidateScriptManagementPopup(scene);
        ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);

        if(m_scriptManagementPopup && ImGui::MenuItem("Remove Component"))
        {
            ValidateScriptManagementPopup(scene);
            const auto current = ResolveComponentEditTarget(scene);
            SceneObjectDocument latest;

            // Only the Script is removed; Transform and every other component are preserved.
            if(m_scriptManagementPopup && current && owner->CaptureLightInspection(current->objectID, latest) &&
                latest.script && CommitObjectScript(scene, *current, latest.script, std::nullopt))
            {
                DismissScriptManagementPopup();
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

    // New IDs prevent a disappearing widget from committing into another component instance.
    ImGui::PushID(static_cast<int>(m_scriptControlsRevision));
    ImGui::BeginDisabled(blocked);
    const ScriptComponent script = *object.script;
    bool enabled = script.enabled;

    // A disabled Script stays authored; gameplay neither compiles nor instantiates it.
    if(ImGui::Checkbox("Enabled", &enabled) && !blocked)
    {
        auto edited = script;
        edited.enabled = enabled;
        CommitObjectScript(scene, *selection, object.script, edited);
    }

    // Script asset field: read-only name plus the picker, like material texture slots.
    const std::string scriptName = DescribeScript(script.scriptReference, registry);
    const float buttonWidth = ImGui::GetFrameHeight() * 1.5f;
    ImGui::SetNextItemWidth((std::max)(1.0f, ImGui::CalcItemWidth() - buttonWidth - ImGui::GetStyle().ItemInnerSpacing.x));
    std::string fieldText = scriptName;
    ImGui::InputText("##ScriptAsset", fieldText.data(), fieldText.size() + 1, ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);

    if(ImGui::Button("...##ScriptPicker", ImVec2(buttonWidth, 0.0f)))
    {
        m_scriptSearch.fill(0);
        ImGui::OpenPopup("ScriptPicker");
    }

    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted("Script");
    ImGui::SetNextWindowSize(ImVec2(360.0f, 320.0f), ImGuiCond_Appearing);

    if(ImGui::BeginPopup("ScriptPicker"))
    {
        if(blocked || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ImGui::CloseCurrentPopup();
        }
        else
        {
            DrawScriptPickerContents(scene, *selection, script, paths, registry);
        }

        ImGui::EndPopup();
    }

    // Class selection lists only instantiable EAScript classes of the validated script.
    const bool classesCurrent = m_scriptClasses && m_scriptClasses->reference == script.scriptReference;
    const std::vector<std::string> noClasses;
    const auto& classes = classesCurrent ? m_scriptClasses->classes : noClasses;
    const bool classValid = std::find(classes.begin(), classes.end(), script.className) != classes.end();
    const std::string preview = script.className.empty() ? "None" :
        classValid || !classesCurrent ? script.className : script.className + " (not found)";
    ImGui::BeginDisabled(script.scriptReference.empty());

    if(ImGui::BeginCombo("Class", preview.c_str()))
    {
        for(const auto& name : classes)
        {
            if(ImGui::Selectable(name.c_str(), name == script.className) && !blocked && name != script.className)
            {
                auto edited = script;
                edited.className = name;
                CommitObjectScript(scene, *selection, object.script, edited);
            }
        }

        ImGui::EndCombo();
    }

    ImGui::EndDisabled();

    if(!script.scriptReference.empty() && ImGui::SmallButton("Refresh"))
    {
        RefreshScriptClasses(script.scriptReference, paths, registry);
    }

    ImGui::EndDisabled();

    if(classesCurrent && !m_scriptClasses->error.empty())
    {
        ImGui::TextWrapped("Script error: %s", m_scriptClasses->error.c_str());
    }
    else if(classesCurrent && classes.empty() && !script.scriptReference.empty())
    {
        ImGui::TextDisabled("This script defines no class deriving from EAScript.");
    }
    else if(!script.enabled)
    {
        ImGui::TextDisabled("Disabled: not compiled or run during gameplay.");
    }

    if(!m_scriptEditStatus.empty())
    {
        ImGui::TextWrapped("%s", m_scriptEditStatus.c_str());
    }

    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
}
