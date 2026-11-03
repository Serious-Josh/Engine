#include "Editor/Editor.h"
#include "DemoContent/FileBackedDemoScene.h"
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

    bool IsAudioRunning(const Audio::AudioSystem* audio)
    {
        return audio && (audio->GetStatus() == Audio::Status::Active || audio->GetStatus() == Audio::Status::NoOutput);
    }

    // Status lines wrap: the Inspector is often narrower than a full sentence.
    void StatusText(const std::string& text, const ImVec4* color = nullptr)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, color ? *color : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
    }

    // Snapshots share the Studio event namespace but are not playable Audio Source events.
    bool IsSelectableEvent(const Audio::EventInfo& event)
    {
        return event.path.empty() || event.path.starts_with("event:/");
    }
}

Editor::AudioEventDescription Editor::DescribeAudioEvent(const AudioSourceComponent& source,
    const Audio::AudioSystem* audio)
{
    if(source.eventGuid.empty())
    {
        return {"None", {}, false};
    }

    const std::string fallback = source.eventPath.empty() ? source.eventGuid : source.eventPath;

    if(!IsAudioRunning(audio))
    {
        return {fallback, "Audio is unavailable; the event cannot be checked.", false};
    }

    const auto guid = Audio::ParseGuid(source.eventGuid);

    if(!guid || !audio->HasEvent(*guid))
    {
        return {"Missing event (" + fallback + ")", "No loaded bank defines this event; it does not play.", false};
    }

    const std::string path = audio->GetEventPath(*guid);
    const bool flat = !audio->IsEvent3D(*guid);
    return {path.empty() ? fallback : path, flat ? "2D event: plays without spatialization." : std::string(), flat};
}

const SceneObjectDocument* Editor::FindActiveAudioListener(std::span<const SceneObjectDocument> objects)
{
    const auto found = std::find_if(objects.begin(), objects.end(),
        [](const SceneObjectDocument& object) { return object.audioListener && object.audioListener->enabled; });
    return found == objects.end() ? nullptr : &*found;
}

void Editor::DismissAudioManagementPopup()
{
    if(m_audioManagementPopup)
    {
        CloseComponentPopup(m_audioManagementPopup->popupID);
    }

    m_audioManagementPopup.reset();
}

void Editor::ValidateAudioManagementPopup(IDemoScene& scene)
{
    if(!m_audioManagementPopup)
    {
        return;
    }

    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    SceneObjectDocument object;

    if(!ComponentPopupMatches(scene, *m_audioManagementPopup) || !owner ||
        !owner->CaptureLightInspection(m_audioManagementPopup->selection.objectID, object) ||
        !(m_audioManagementKind == AudioComponentKind::Source ? object.audioSource.has_value() :
            object.audioListener.has_value()))
    {
        DismissAudioManagementPopup();
    }
}

size_t Editor::AudioEditPayloadBytes(const AudioComponentsEdit& edit)
{
    const auto pathBytes = [](const std::filesystem::path& path)
    {
        return path.native().capacity() * sizeof(std::filesystem::path::value_type);
    };

    const auto sourceBytes = [](const AudioComponentState& state)
    {
        return state.source ? state.source->eventGuid.capacity() + state.source->eventPath.capacity() : 0;
    };

    return sizeof(SceneHistoryEntry) + sizeof(AudioComponentsEdit) + edit.objectID.capacity() +
        edit.session.sceneIdentity.capacity() + pathBytes(edit.session.projectIdentity) +
        pathBytes(edit.session.scenePath) + sourceBytes(edit.before) + sourceBytes(edit.after);
}

bool Editor::CommitObjectAudio(IDemoScene& scene, const SceneObjectMetadata& selection,
    const AudioComponentState& before, const AudioComponentState& after)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(IsSceneEditingLocked() || !owner || !m_historySession ||
        !RenameSessionMatches(scene, *m_historySession) || HasPendingSceneHistoryOperation() ||
        m_sceneOperationPending || HasBlockingObjectCreation())
    {
        m_audioEditStatus = "Audio authoring requires a stopped scene without pending edits.";
        return false;
    }

    const auto current = ResolveComponentEditTarget(scene);

    if(!current || current->objectID != selection.objectID || current->sceneID != selection.sceneID ||
        current->runtimeIdentity != selection.runtimeIdentity || current->structureRevision != selection.structureRevision ||
        current->runtimeIndex != selection.runtimeIndex)
    {
        m_audioEditStatus = "Audio inspection target is stale or blocked.";
        return false;
    }

    // The complete replacement is validated; the authored value is never partially changed.
    if(after.source && !ValidateAudioSourceComponent(*after.source, &m_audioEditStatus))
    {
        m_audioEditStatus += " The authored value was preserved.";
        return false;
    }

    if(before == after)
    {
        // Still checks staleness, but a no-op never records history or dirties the scene.
        return owner->SetObjectAudio(selection, before, after, &m_audioEditStatus);
    }

    try
    {
        auto command = std::make_unique<AudioComponentsEdit>(
            AudioComponentsEdit{*m_historySession, selection.objectID, before, after});
        const size_t bytes = AudioEditPayloadBytes(*command);

        if(bytes > kSceneHistoryPayloadBytes)
        {
            m_audioEditStatus = "Audio edit exceeds the scene history payload limit.";
            return false;
        }

        SceneHistoryEntry entry{std::move(command), m_historyEpoch, bytes};
        // Reserve before mutation; appending the accepted command is noexcept.
        m_sceneHistory.reserve(kSceneHistoryCapacity);

        if(!owner->SetObjectAudio(selection, before, after, &m_audioEditStatus))
        {
            return false;
        }

        AppendSceneHistoryEntry(std::move(entry));
        m_audioInspection.reset();
        ++m_audioControlsRevision;
        m_audioEditStatus.clear();
        return true;
    }
    catch(const std::exception& exception)
    {
        m_audioEditStatus = exception.what();
        return false;
    }
}

bool Editor::DrawAudioComponentMenu(IDemoScene& scene, const SceneObjectMetadata& selection, AudioComponentKind kind,
    bool blocked)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    const char* popup = kind == AudioComponentKind::Source ? "AudioSourceManagement" : "AudioListenerManagement";
    ImGui::BeginDisabled(blocked);

    if(ImGui::SmallButton("..."))
    {
        m_audioManagementKind = kind;
        m_audioManagementPopup = ComponentPopupTarget{*m_historySession, selection, m_historyEpoch, ImGui::GetID(popup)};
        ImGui::OpenPopup(popup);
    }

    ImGui::EndDisabled();
    bool removed = false;

    if(ImGui::BeginPopup(popup))
    {
        ValidateAudioManagementPopup(scene);
        ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);

        if(m_audioManagementPopup && m_audioManagementKind == kind && ImGui::MenuItem("Remove Component"))
        {
            ValidateAudioManagementPopup(scene);
            const auto current = ResolveComponentEditTarget(scene);
            SceneObjectDocument latest;

            if(m_audioManagementPopup && current && owner && owner->CaptureLightInspection(current->objectID, latest))
            {
                // Only this component is removed; the other audio component and everything else are preserved.
                const AudioComponentState before{latest.audioSource, latest.audioListener};
                AudioComponentState after = before;

                if(kind == AudioComponentKind::Source)
                {
                    after.source.reset();
                }
                else
                {
                    after.listener.reset();
                }

                if(before != after && CommitObjectAudio(scene, *current, before, after))
                {
                    DismissAudioManagementPopup();
                    removed = true;
                }
            }
        }

        ImGui::PopItemFlag();
        ImGui::EndPopup();
    }

    return removed;
}

void Editor::DrawAudioEventPicker(IDemoScene& scene, const SceneObjectMetadata& selection,
    const SceneObjectDocument& object)
{
    const AudioSourceComponent& source = *object.audioSource;
    ImGui::TextUnformatted("Select FMOD event");
    ImGui::SameLine();

    if(ImGui::Button("Close"))
    {
        ImGui::CloseCurrentPopup();
        return;
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##AudioEventSearch", "Search events...", m_audioEventSearch.data(), m_audioEventSearch.size());
    const std::string query = Lowercase(m_audioEventSearch.data());
    std::optional<AudioSourceComponent> chosen;

    if(ImGui::Selectable("None", source.eventGuid.empty(), ImGuiSelectableFlags_DontClosePopups))
    {
        chosen = source;
        chosen->eventGuid.clear();
        chosen->eventPath.clear();
    }

    ImGui::Separator();

    if(!IsAudioRunning(m_audioSystem))
    {
        StatusText("Audio is unavailable (" +
            (m_audioSystem ? std::string(Audio::ToString(m_audioSystem->GetStatus())) : std::string("Disabled")) +
            "); no events can be listed.");
    }
    else
    {
        const auto events = m_audioSystem->GetLoadedEvents();
        size_t listed = 0;

        for(const auto& event : events)
        {
            if(!IsSelectableEvent(event))
            {
                continue;
            }

            const std::string guid = Audio::ToString(event.id);
            // The path is shown; the GUID is what is stored.
            const std::string label = event.path.empty() ? guid : event.path;

            if(!query.empty() && Lowercase(label).find(query) == std::string::npos)
            {
                continue;
            }

            ++listed;
            const bool current = Audio::ParseGuid(source.eventGuid) == std::optional<Audio::Guid>(event.id);
            const std::string item = label + (m_audioSystem->IsEvent3D(event.id) ? "" : "  (2D)") + "##" + guid;

            if(ImGui::Selectable(item.c_str(), current, ImGuiSelectableFlags_DontClosePopups))
            {
                chosen = source;
                chosen->eventGuid = guid;
                chosen->eventPath = event.path;
            }
        }

        if(events.empty())
        {
            StatusText("No events: add FMOD Studio banks to Content/Audio/Banks and refresh the Asset Browser.");
        }
        else if(listed == 0)
        {
            StatusText("No events match the search.");
        }
    }

    if(chosen && CommitObjectAudio(scene, selection, {object.audioSource, object.audioListener},
        {chosen, object.audioListener}))
    {
        ImGui::CloseCurrentPopup();
    }
}

void Editor::DrawAudioSourceControls(IDemoScene& scene, const SceneObjectMetadata& selection,
    const SceneObjectDocument& object, bool blocked)
{
    ImGui::PushID("AudioSource");
    ImGui::Separator();
    ImGui::TextUnformatted("Audio Source");
    ImGui::SameLine();

    if(DrawAudioComponentMenu(scene, selection, AudioComponentKind::Source, blocked))
    {
        ImGui::PopID();
        return;
    }

    // New IDs prevent a disappearing widget from committing into another component instance.
    ImGui::PushID(static_cast<int>(m_audioControlsRevision));
    ImGui::BeginDisabled(blocked);
    const AudioComponentState before{object.audioSource, object.audioListener};
    const AudioSourceComponent source = *object.audioSource;
    bool enabled = source.enabled;

    if(ImGui::Checkbox("Enabled", &enabled) && !blocked)
    {
        auto after = before;
        after.source->enabled = enabled;
        CommitObjectAudio(scene, selection, before, after);
    }

    // Event field: read-only path plus the picker, like the Script asset field.
    const auto description = DescribeAudioEvent(source, m_audioSystem);
    const float buttonWidth = ImGui::GetFrameHeight() * 1.5f;
    ImGui::SetNextItemWidth((std::max)(1.0f, ImGui::CalcItemWidth() - buttonWidth - ImGui::GetStyle().ItemInnerSpacing.x));
    std::string fieldText = description.text;
    ImGui::InputText("##AudioEvent", fieldText.data(), fieldText.size() + 1, ImGuiInputTextFlags_ReadOnly);

    // The field is narrow in small layouts: the tooltip shows the whole name and the stored GUID.
    if(!source.eventGuid.empty() && ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s\n%s", description.text.c_str(), source.eventGuid.c_str());
    }

    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);

    if(ImGui::Button("...##AudioEventPicker", ImVec2(buttonWidth, 0.0f)))
    {
        m_audioEventSearch.fill(0);
        ImGui::OpenPopup("AudioEventPicker");
    }

    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted("Event");
    ImGui::SetNextWindowSize(ImVec2(380.0f, 320.0f), ImGuiCond_Appearing);

    if(ImGui::BeginPopup("AudioEventPicker"))
    {
        if(blocked || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ImGui::CloseCurrentPopup();
        }
        else
        {
            DrawAudioEventPicker(scene, selection, object);
        }

        ImGui::EndPopup();
    }

    bool autoplay = source.autoplay;

    if(ImGui::Checkbox("Autoplay", &autoplay) && !blocked)
    {
        auto after = before;
        after.source->autoplay = autoplay;
        CommitObjectAudio(scene, selection, before, after);
    }

    ImGui::EndDisabled();

    if(!description.status.empty())
    {
        StatusText(description.status);
    }

    if(!source.enabled)
    {
        StatusText("Disabled: does not play during gameplay.");
    }
    else if(source.eventGuid.empty())
    {
        StatusText("No event assigned: does not play during gameplay.");
    }

    ImGui::PopID();
    ImGui::PopID();
}

void Editor::DrawAudioListenerControls(IDemoScene& scene, const SceneObjectMetadata& selection,
    const SceneObjectDocument& object, bool blocked)
{
    ImGui::PushID("AudioListener");
    ImGui::Separator();
    ImGui::TextUnformatted("Audio Listener");
    ImGui::SameLine();

    if(DrawAudioComponentMenu(scene, selection, AudioComponentKind::Listener, blocked))
    {
        ImGui::PopID();
        return;
    }

    ImGui::PushID(static_cast<int>(m_audioControlsRevision));
    ImGui::BeginDisabled(blocked);
    const AudioComponentState before{object.audioSource, object.audioListener};
    bool enabled = object.audioListener->enabled;

    if(ImGui::Checkbox("Enabled", &enabled) && !blocked)
    {
        auto after = before;
        after.listener->enabled = enabled;
        CommitObjectAudio(scene, selection, before, after);
    }

    ImGui::EndDisabled();

    const SceneObjectDocument* active = nullptr;

    if(auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene); owner && owner->m_runtime &&
        owner->m_runtime->GetFileScene().HasDocument())
    {
        active = FindActiveAudioListener(owner->m_runtime->GetFileScene().GetDocument().objects);
    }

    if(!object.audioListener->enabled)
    {
        StatusText(active ? "Disabled: '" + active->name + "' is the active listener." :
            std::string("Disabled: no listener is enabled, so the Main Camera hears."));
    }
    else if(active && active->id == object.id)
    {
        const ImVec4 green(0.45f, 0.85f, 0.45f, 1.0f);
        StatusText("Active listener during gameplay.", &green);
    }
    else if(active)
    {
        StatusText("Inactive: '" + active->name + "' comes first in scene order and takes priority.");
    }

    ImGui::PopID();
    ImGui::PopID();
}

void Editor::DrawObjectAudioControls(IDemoScene& scene)
{
    ValidateAudioManagementPopup(scene);
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

    const AudioComponentState value{object.audioSource, object.audioListener};
    const bool changedTarget = !m_audioInspection || m_audioInspectionEpoch != m_historyEpoch ||
        m_audioInspection->objectID != selection->objectID ||
        m_audioInspection->runtimeIdentity != selection->runtimeIdentity ||
        m_audioInspection->structureRevision != selection->structureRevision;
    const bool blocked = !ResolveComponentEditTarget(scene, *selection);

    if(changedTarget || m_audioInspectedValue != value || blocked)
    {
        ++m_audioControlsRevision;
        m_audioInspection = selection;
        m_audioInspectionEpoch = m_historyEpoch;
        m_audioInspectedValue = value;

        if(changedTarget)
        {
            m_audioEditStatus.clear();
        }
    }

    if(!object.audioSource && !object.audioListener)
    {
        if(!m_audioEditStatus.empty())
        {
            ImGui::TextWrapped("%s", m_audioEditStatus.c_str());
        }

        return;
    }

    ImGui::PushID(static_cast<int>(m_historyEpoch));
    ImGui::PushID(selection->objectID.c_str());

    if(object.audioSource)
    {
        DrawAudioSourceControls(scene, *selection, object, blocked);
    }

    // A removal above changes the object; the listener draws from the next frame's inspection.
    if(object.audioListener && m_audioInspection)
    {
        DrawAudioListenerControls(scene, *selection, object, blocked);
    }

    if(!m_audioEditStatus.empty())
    {
        ImGui::TextWrapped("%s", m_audioEditStatus.c_str());
    }

    ImGui::PopID();
    ImGui::PopID();
}
