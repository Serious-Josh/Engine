#include "Editor/Editor.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "imgui.h"

#include <algorithm>
#include <system_error>

namespace
{
    // Bounds the listing of a very large Scenes tree; selection still validates any listed scene.
    constexpr std::size_t kMaximumListedScenes = 2048;

    std::string PathText(const std::filesystem::path& path)
    {
        const auto text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    }

    bool IsSceneFile(const std::filesystem::path& path)
    {
        const auto extension = path.extension().native();
        return CompareStringOrdinal(extension.c_str(), -1, L".scene", -1, TRUE) == CSTR_EQUAL;
    }

    // The active editable scene when it has been saved to a file; null otherwise.
    const FileBackedDemoScene* SavedActiveScene(const IDemoScene& demoScene)
    {
        const auto* scene = dynamic_cast<const FileBackedDemoScene*>(&demoScene);

        if(!scene || scene->IsArchiveActivation() || scene->IsUnsaved())
        {
            return nullptr;
        }

        return scene;
    }
}

void Editor::OnStartupSceneChanged(bool succeeded, std::string message)
{
    m_projectSettingsMessage = std::move(message);
    m_projectSettingsMessageIsError = !succeeded;
}

void Editor::ListStartupSceneCandidates()
{
    m_startupSceneCandidates.clear();
    m_startupSceneCandidatesListed = true;

    if(!m_activeProject)
    {
        return;
    }

    // File names only: scene documents are validated when one is chosen, not while listing.
    std::error_code error;
    const auto options = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator entry(m_activeProject->GetPaths().GetScenesDirectory(), options, error);
    const std::filesystem::recursive_directory_iterator end;

    while(!error && entry != end && m_startupSceneCandidates.size() < kMaximumListedScenes)
    {
        std::error_code entryError;
        const auto type = entry->symlink_status(entryError).type();

        if(type == std::filesystem::file_type::symlink || type == std::filesystem::file_type::junction)
        {
            // Links and junctions are not followed; containment is checked again on selection.
            entry.disable_recursion_pending();
        }
        else if(type == std::filesystem::file_type::regular && IsSceneFile(entry->path()))
        {
            m_startupSceneCandidates.push_back(entry->path());
        }

        entry.increment(error);
    }

    std::sort(m_startupSceneCandidates.begin(), m_startupSceneCandidates.end());
}

void Editor::DrawProjectSettings(const IDemoScene& demoScene)
{
    if(!m_showProjectSettings)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(600, 360), ImGuiCond_FirstUseEver);

    if(!ImGui::Begin("Project Settings", &m_showProjectSettings) || !m_activeProject)
    {
        ImGui::End();
        return;
    }

    const auto* manifest = m_activeProject->GetManifest();
    const auto& root = m_activeProject->GetRoot();

    ImGui::TextDisabled("Shared project settings, stored in the project's .engineproj file");
    ImGui::TextWrapped("Name: %s", manifest ? manifest->GetDisplayName().c_str() : "Legacy project");
    ImGui::TextWrapped("Location: %s", PathText(root).c_str());
    ImGui::TextWrapped("Project ID: %s", manifest ? manifest->GetProjectID().ToString().c_str() : "manifestless");

    ImGui::Separator();
    ImGui::TextUnformatted("Game Startup Scene");

    const auto configured = manifest ? manifest->GetStartupScene() : std::filesystem::path{};

    if(configured.empty())
    {
        ImGui::TextWrapped("Not set: Build Game uses the active saved scene.");
    }
    else
    {
        ImGui::TextWrapped("%s (explicit)", PathText(configured).c_str());
    }

    if(!m_startupSceneCandidatesListed)
    {
        ListStartupSceneCandidates();
    }

    const auto* activeScene = SavedActiveScene(demoScene);
    const bool editable = manifest && !m_startupSceneRequest.has_value();
    ImGui::BeginDisabled(!editable);

    const std::string preview = configured.empty() ? "Use active saved scene" : PathText(configured);

    if(ImGui::BeginCombo("Startup scene", preview.c_str()))
    {
        if(ImGui::Selectable("Use active saved scene", configured.empty()) && !configured.empty())
        {
            m_startupSceneRequest = std::filesystem::path{};
        }

        for(const auto& candidate : m_startupSceneCandidates)
        {
            const auto relative = candidate.lexically_relative(root);
            const bool selected = !configured.empty() && ProjectAssetRegistry::PathsEqual(relative, configured);

            if(ImGui::Selectable(PathText(relative).c_str(), selected) && !selected)
            {
                m_startupSceneRequest = candidate;
            }
        }

        ImGui::EndCombo();
    }

    ImGui::BeginDisabled(!activeScene);

    if(ImGui::Button("Use Active Scene"))
    {
        m_startupSceneRequest = activeScene->GetScenePath();
    }

    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(configured.empty());

    if(ImGui::Button("Clear"))
    {
        m_startupSceneRequest = std::filesystem::path{};
    }

    ImGui::EndDisabled();
    ImGui::SameLine();

    if(ImGui::Button("Refresh List"))
    {
        ListStartupSceneCandidates();
    }

    ImGui::EndDisabled();

    if(!manifest)
    {
        ImGui::TextWrapped("This project has no manifest, so shared settings cannot be changed.");
    }

    if(!m_projectSettingsMessage.empty())
    {
        const ImVec4 errorColor(1.0f, 0.45f, 0.4f, 1.0f);
        const ImVec4 infoColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text, m_projectSettingsMessageIsError ? errorColor : infoColor);
        ImGui::TextWrapped("%s", m_projectSettingsMessage.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::End();
}

ProjectFileAssociation::RegistryLocation Editor::FileAssociationLocation() const
{
#if defined(ENGINE_HAS_VERIFICATION)
    if(m_fileAssociationLocationForVerification)
    {
        return *m_fileAssociationLocationForVerification;
    }
#endif

    return ProjectFileAssociation::CurrentUser();
}

void Editor::RefreshFileAssociationStatus()
{
    const auto executable = ProjectFileAssociation::CurrentExecutable();
    m_fileAssociationStatus = ProjectFileAssociation::ReadStatus(FileAssociationLocation(), executable);
    m_fileAssociationEffectiveExecutable = ProjectFileAssociation::QueryEffectiveExecutable();
    m_fileAssociationStatusRead = true;
}

void Editor::DrawFileAssociation()
{
    if(!m_showFileAssociation)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(620, 330), ImGuiCond_FirstUseEver);

    if(!ImGui::Begin("Windows File Association", &m_showFileAssociation))
    {
        ImGui::End();
        return;
    }

    if(!m_fileAssociationStatusRead)
    {
        RefreshFileAssociationStatus();
    }

    const auto executable = ProjectFileAssociation::CurrentExecutable();

    ImGui::TextDisabled("Personal Windows preference for this user; not stored in any project");
    ImGui::TextWrapped("Status: %s", ProjectFileAssociation::Describe(m_fileAssociationStatus).c_str());

    const auto effective = m_fileAssociationEffectiveExecutable.empty() ?
        std::string("none") :
        PathText(m_fileAssociationEffectiveExecutable);
    ImGui::TextWrapped("Windows currently opens .engineproj files with: %s", effective.c_str());
    ImGui::TextWrapped("This Editor: %s", PathText(executable).c_str());

    ImGui::Separator();
    ImGui::BeginDisabled(m_fileAssociationConfirmReplace);

    if(ImGui::Button("Register .engineproj with this Editor"))
    {
        const auto result = ProjectFileAssociation::Register(FileAssociationLocation(), executable, false,
            m_fileAssociationMessage);
        m_fileAssociationConfirmReplace = result == ProjectFileAssociation::ChangeResult::NeedsConfirmation;
        RefreshFileAssociationStatus();
    }

    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_fileAssociationStatus.registeredExecutable.empty() || m_fileAssociationConfirmReplace);

    const bool stale = m_fileAssociationStatus.state == ProjectFileAssociation::State::StaleRegistration;

    if(ImGui::Button(stale ? "Remove Stale Registration" : "Unregister"))
    {
        ProjectFileAssociation::Unregister(FileAssociationLocation(), executable, m_fileAssociationMessage);
        RefreshFileAssociationStatus();
    }

    ImGui::EndDisabled();
    ImGui::SameLine();

    if(ImGui::Button("Refresh"))
    {
        RefreshFileAssociationStatus();
    }

    if(m_fileAssociationConfirmReplace)
    {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_fileAssociationMessage.c_str());

        if(ImGui::Button("Replace for This User"))
        {
            ProjectFileAssociation::Register(FileAssociationLocation(), executable, true,
                m_fileAssociationMessage);
            m_fileAssociationConfirmReplace = false;
            RefreshFileAssociationStatus();
        }

        ImGui::SameLine();

        if(ImGui::Button("Keep Existing"))
        {
            m_fileAssociationConfirmReplace = false;
            m_fileAssociationMessage = "The existing association was kept.";
        }
    }
    else if(!m_fileAssociationMessage.empty())
    {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_fileAssociationMessage.c_str());
    }

    ImGui::Separator();
    ImGui::TextWrapped("Windows may keep using a program chosen in Settings > Apps > Default apps or with "
        "\"Open with > Always\". Engine never changes that protected choice; select Engine Project there if needed.");

    ImGui::End();
}

void Editor::DrawProjectOwnershipLost()
{
    constexpr const char* popupName = "Project ownership lost";

    if(m_projectOwnershipMessage.empty())
    {
        return;
    }

    if(!ImGui::IsPopupOpen(popupName))
    {
        ImGui::OpenPopup(popupName);
    }

    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);

    if(ImGui::BeginPopupModal(popupName, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::PushTextWrapPos(530);
        ImGui::TextUnformatted("The Editor can no longer verify that it exclusively owns this project's manifest. "
            "Saving, building and other project changes are disabled. Other Editors are still kept out.");
        ImGui::Separator();
        ImGui::TextUnformatted(m_projectOwnershipMessage.c_str());
        ImGui::Separator();
        ImGui::TextUnformatted("Retry checks ownership again. Close Editor shuts down without writing to the "
            "project; unsaved scene changes cannot be saved in this state.");
        ImGui::PopTextWrapPos();

        if(ImGui::Button("Retry"))
        {
            m_ownershipAction = OwnershipAction::Retry;
        }

        ImGui::SameLine();

        if(ImGui::Button("Close Editor"))
        {
            m_ownershipAction = OwnershipAction::Close;
        }

        ImGui::EndPopup();
    }
}
