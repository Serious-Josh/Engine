#include "Editor/ProjectBrowser.h"
#include "Project/ProjectCreation.h"
#include "imgui.h"

#include <Windows.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace
{
    std::string PathText(const std::filesystem::path& path)
    {
        const auto text = path.u8string();
        return {text.begin(), text.end()};
    }

    std::optional<std::filesystem::path> Browse(bool folder, void* window, std::string& error)
    {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        if(FAILED(initialized))
        {
            error = "Cannot initialize the native file dialog.";
            return std::nullopt;
        }

        struct Cleanup
        {
            ~Cleanup()
            {
                CoUninitialize();
            }
        } cleanup;
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));

        if(SUCCEEDED(result))
        {
            result = dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
        }

        if(SUCCEEDED(result) && !folder)
        {
            const COMDLG_FILTERSPEC filter{L"Engine project (*.engineproj)", L"*.engineproj"};
            result = dialog->SetFileTypes(1, &filter);
        }

        if(SUCCEEDED(result))
        {
            result = dialog->SetTitle(folder ? L"Project Destination" : L"Open Engine Project");
        }

        if(SUCCEEDED(result))
        {
            result = dialog->Show(static_cast<HWND>(window));
        }

        if(result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        {
            return std::nullopt;
        }

        Microsoft::WRL::ComPtr<IShellItem> selected;

        if(SUCCEEDED(result))
        {
            result = dialog->GetResult(&selected);
        }

        PWSTR path = nullptr;

        if(SUCCEEDED(result))
        {
            result = selected->GetDisplayName(SIGDN_FILESYSPATH, &path);
        }

        if(FAILED(result))
        {
            error = "Native selection failed (HRESULT " + std::to_string(result) + ").";
            return std::nullopt;
        }

        std::filesystem::path value(path);
        CoTaskMemFree(path);
        return value;
    }

    // Read-only availability probe, never takes writable ownership or alters recents.
    bool Available(const RecentProject& recent)
    {
        try
        {
            std::error_code error;

            if(!std::filesystem::is_regular_file(recent.manifestPath, error) || error ||
                std::filesystem::file_size(recent.manifestPath, error) > 1024 * 1024 || error)
            {
                return false;
            }

            std::ifstream input(recent.manifestPath, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(input)), {});
            std::string reason;
            const auto manifest = ProjectManifest::Parse(text, reason);
            return input.good() && manifest && manifest->GetProjectID().ToString() == recent.projectID;
        }
        catch(const std::exception&)
        {
            return false;
        }
    }
}

std::optional<ProjectBrowser::Request> ProjectBrowser::Draw(const EditorPreferences& preferences, void* window, bool creationOnly)
{
    if(!m_initialized)
    {
        PWSTR documents = nullptr;

        if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents)))
        {
            const auto destination = PathText(std::filesystem::path(documents) / L"Engine Projects");
            std::memcpy(m_destination.data(), destination.data(), (std::min)(destination.size(), m_destination.size() - 1));
            CoTaskMemFree(documents);
        }

        m_initialized = true;
    }

    std::optional<Request> request;
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Engine Project Browser", nullptr, ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::Spacing();
    ImGui::TextUnformatted("ENGINE  /  PROJECTS");
    if(creationOnly && ImGui::Button("Cancel New Project"))
    {
        request = Request{Action::Cancel, {}, {}, {}, {}};
    }
    ImGui::TextDisabled("Create something new, or pick up where you left off.");
    ImGui::Spacing();
    ImGui::Separator();

    if(ImGui::BeginChild("Project actions", ImVec2(0, 260), ImGuiChildFlags_Borders))
    {
        ImGui::TextUnformatted("New Project");
        ImGui::SetNextItemWidth((std::min)(520.0f, ImGui::GetContentRegionAvail().x));
        ImGui::InputText("Name", m_name.data(), m_name.size());
        ImGui::SetNextItemWidth((std::max)(100.0f, ImGui::GetContentRegionAvail().x - 180));
        ImGui::InputText("Destination", m_destination.data(), m_destination.size());
        ImGui::SameLine();

        if(ImGui::Button("Browse..."))
        {
            if(const auto path = Browse(true, window, message))
            {
                const auto text = PathText(*path);
                m_destination.fill(0);
                std::memcpy(m_destination.data(), text.data(), (std::min)(text.size(), m_destination.size() - 1));
            }
        }

        const auto destination = std::filesystem::path(std::u8string(
            m_destination.data(), m_destination.data() + std::strlen(m_destination.data())));
        std::string validation;
        const bool valid = ProjectCreation::Validate(m_name.data(), destination, validation);
        ImGui::TextDisabled("A new folder named after your project will be created here.");
        ImGui::BeginDisabled(!valid);

        if(ImGui::Button("Create Project", ImVec2(150, 0)))
        {
            request = Request{Action::Create, destination, {}, m_name.data(), {}};
        }

        ImGui::EndDisabled();

        if(!valid)
        {
            ImGui::TextWrapped("%s", validation.c_str());
        }

        if(!creationOnly)
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextUnformatted("Open an existing project");

            if(ImGui::Button("Open Project...", ImVec2(150, 0)))
            {
                if(const auto path = Browse(false, window, message))
                {
                    request = Request{Action::Open, *path, {}, {}, {}};
                }
            }
        }
    }

    ImGui::EndChild();

    if(!message.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", message.c_str());
        ImGui::PopStyleColor();
    }

    if(creationOnly)
    {
        ImGui::End();
        return request;
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Recent Projects");
    ImGui::BeginChild("Recent list");
    const auto& recents = preferences.GetRecentProjects();

    if(m_recentSnapshot != recents || ImGui::GetTime() >= m_nextRecentProbe)
    {
        m_recentSnapshot = recents;
        m_recentAvailable.clear();

        for(const auto& recent : recents)
        {
            m_recentAvailable.push_back(Available(recent));
        }

        m_nextRecentProbe = ImGui::GetTime() + 2.0;
    }

    if(recents.empty())
    {
        ImGui::TextDisabled("Projects you open will appear here.");
    }

    for(std::size_t index = 0; index < recents.size(); ++index)
    {
        const auto& recent = recents[index];
        ImGui::PushID(static_cast<int>(index));
        ImGui::Separator();
        ImGui::TextUnformatted(recent.displayName.empty() ? "Engine Project" : recent.displayName.c_str());
        ImGui::TextWrapped("%s", PathText(recent.manifestPath).c_str());
        const bool available = m_recentAvailable[index];

        if(!available)
        {
            ImGui::TextDisabled("Unavailable / missing, invalid, or a different project");
        }

        ImGui::BeginDisabled(!available);

        if(ImGui::Button("Open"))
        {
            request = Request{Action::Open, recent.manifestPath, {}, {}, recent.projectID};
        }

        ImGui::EndDisabled();
        ImGui::SameLine();

        if(ImGui::Button("Locate..."))
        {
            if(const auto path = Browse(false, window, message))
            {
                request = Request{Action::Locate, *path, recent.manifestPath, {}, recent.projectID};
            }
        }

        ImGui::SameLine();

        if(ImGui::Button("Remove"))
        {
            request = Request{Action::Remove, recent.manifestPath, {}, {}, {}};
        }

        ImGui::PopID();
    }

    ImGui::EndChild();
    ImGui::End();
    return request;
}
