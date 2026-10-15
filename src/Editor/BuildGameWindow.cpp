#include "Editor/Editor.h"
#include "imgui.h"
#include <Windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <cstring>
namespace
{
std::optional<std::filesystem::path> BrowseBuildFolder(std::string& error)
{
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(initialized)) { error="Cannot initialize the native folder dialog (COM apartment unavailable)."; return {}; }
    struct Cleanup { ~Cleanup(){CoUninitialize();} } cleanup;
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    HRESULT result=CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog));
    if(SUCCEEDED(result)) result=dialog->SetOptions(FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);
    if(SUCCEEDED(result)) result=dialog->SetTitle(L"Build Game Output Directory");
    if(SUCCEEDED(result)) result=dialog->Show(GetActiveWindow());
    if(result==HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    Microsoft::WRL::ComPtr<IShellItem> folder;
    if(SUCCEEDED(result)) result=dialog->GetResult(&folder);
    PWSTR path=nullptr;
    if(SUCCEEDED(result)) result=folder->GetDisplayName(SIGDN_FILESYSPATH,&path);
    if(FAILED(result)) { error="Native folder selection failed (HRESULT "+std::to_string(result)+")."; return {}; }
    std::filesystem::path selected(path); CoTaskMemFree(path); return selected;
}
}
void Editor::DrawBuildGame()
{
    if(!m_showBuildGame) return;
    ImGui::SetNextWindowSize(ImVec2(540,280),ImGuiCond_FirstUseEver);
    if(ImGui::Begin("Build Game",&m_showBuildGame))
    {
        ImGui::BeginDisabled(m_buildGameStatus.busy || m_buildGameRequest.has_value());
        ImGui::TextUnformatted("Build Type");
        if(ImGui::RadioButton("Simple",!m_buildAdvanced)) m_buildAdvanced=false;
        ImGui::SameLine(); if(ImGui::RadioButton("Advanced",m_buildAdvanced)) m_buildAdvanced=true;
        ImGui::InputText("Title",m_buildTitle,sizeof(m_buildTitle));
        ImGui::InputText("Output Dir",m_buildOutput,sizeof(m_buildOutput)); ImGui::SameLine();
        if(ImGui::Button("Browse"))
        {
            std::string error; const auto folder=BrowseBuildFolder(error);
            if(folder)
            {
                const auto utf8=folder->u8string();
                const std::string text(reinterpret_cast<const char*>(utf8.data()),utf8.size());
                if(text.size()<sizeof(m_buildOutput)) std::memcpy(m_buildOutput,text.c_str(),text.size()+1);
                else error="Selected directory path exceeds the field limit.";
            }
            if(!error.empty()) m_buildGameStatus={false,false,0,error};
        }
        if(ImGui::Button("Build Game"))
        {
            std::string error;
            if(!BuildGame::ValidateTitle(m_buildTitle,error) || m_buildOutput[0]=='\0')
                m_buildGameStatus={false,false,0,error.empty()?"Select an output directory.":error};
            else m_buildGameRequest=BuildGame::Settings{m_buildTitle,std::filesystem::u8path(m_buildOutput),m_buildAdvanced};
        }
        ImGui::EndDisabled();
        if(m_buildGameStatus.busy) { ImGui::SameLine(); if(ImGui::Button("Cancel")) m_buildGameCancel=true; }
        ImGui::ProgressBar(m_buildGameStatus.progress);
        ImGui::TextWrapped("%s",m_buildGameStatus.message.c_str());
    }
    ImGui::End();
}
