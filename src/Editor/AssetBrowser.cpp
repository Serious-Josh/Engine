#include "AssetBrowser.h"
#include "EditorIcons.h"
#include "Renderer/Renderer.h"
#include "EditorWorkspace.h"

#include "Project/ProjectPaths.h"
#include "Project/ProjectContext.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "assets/ProjectTextureImportService.h"
#include "assets/ProjectMeshImportService.h"
#include "assets/SupportedAssetImportFormats.h"
#include "assets/ModelImporter.h"
#include "Editor/InspectorNumericDrag.h"
#include "assets/ProjectAssetResolver.h"
#include "Core/Log.h"
#include "Debug/MaterialThumbnailDiagnostics.h"
#include "Debug/MeshThumbnailDiagnostics.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <memory>
#include <utility>
#include <format>

namespace
{
    // Placeholder for a tile without a ready thumbnail: the asset kind and why it is waiting.
    void DrawThumbnailStatus(ImDrawList* draw, const ImVec2& start, const char* kind, const char* status)
    {
        draw->AddText(ImVec2(start.x + 5.0f, start.y + 5.0f), IM_COL32(190, 190, 190, 255), kind);
        draw->AddText(ImVec2(start.x + 5.0f, start.y + 22.0f), IM_COL32(140, 140, 140, 255), status);
    }
}

namespace
{
    std::string PathID(const std::filesystem::path& path)
    {
        const auto utf8 = path.u8string();
        return std::string(
            reinterpret_cast<const char*>(utf8.data()),
            utf8.size());
    }

    std::string DisplayName(const std::filesystem::path& path)
    {
        const auto utf8 = path.filename().u8string();
        return std::string(
            reinterpret_cast<const char*>(utf8.data()),
            utf8.size());
    }

    bool CreationFilename(
        const char* input,
        std::filesystem::path& filename,
        std::string& error,
        bool material)
    {
        const std::string name(input);

        if (name.empty() || name == "." || name == ".." ||
            (material && name.find("..") != std::string::npos) ||
            name.find_first_of("<>:\"/\\|?*") != std::string::npos)
        {
            error = "Enter a filename without traversal, trailing dots/spaces, or invalid Windows characters.";
            return false;
        }

        if (name.back() == ' ' || name.back() == '.' ||
            std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32; }))
        {
            error = "Enter a filename without traversal, trailing dots/spaces, or invalid Windows characters.";
            return false;
        }

        const std::u8string utf8(
            reinterpret_cast<const char8_t*>(name.data()),
            name.size());
        filename = std::filesystem::path(utf8);

        if (material && filename.has_extension() && filename.extension() != L".material")
        {
            error = "The filename extension must be .material.";
            return false;
        }

        if (material && !filename.has_extension())
        {
            filename += L".material";
        }

        auto base = filename.native().substr(0, filename.native().find(L'.'));

        while (!base.empty() && base.back() == L' ')
        {
            base.pop_back();
        }

        std::transform(base.begin(), base.end(), base.begin(), [](wchar_t c) { return std::towupper(c); });
        const bool numberedDevice = base.size() == 4 &&
            (base.starts_with(L"COM") || base.starts_with(L"LPT")) &&
            ((base[3] >= L'1' && base[3] <= L'9') || base[3] == L'\u00b9' ||
                base[3] == L'\u00b2' || base[3] == L'\u00b3');

        if (base.empty() || base == L"CON" || base == L"PRN" || base == L"AUX")
        {
            error = "The filename is reserved, empty, or too long.";
            return false;
        }

        if (base == L"NUL" || base == L"CONIN$" || base == L"CONOUT$" || numberedDevice)
        {
            error = "The filename is reserved, empty, or too long.";
            return false;
        }

        if (filename.native().size() > 255)
        {
            error = "The filename is reserved, empty, or too long.";
            return false;
        }

        return true;
    }

    bool CreateBrowserFolder(const std::filesystem::path& directory,
        const std::filesystem::path& filename, std::string& error)
    {
        // Pin every ancestor against replacement while validating and creating.
        // OPEN_REPARSE_POINT inspects the directory itself rather than its target.
        std::vector<std::shared_ptr<void>> locks;
        auto current = directory.root_path();
        for (const auto& component : directory.relative_path())
        {
            current /= component;
            HANDLE handle = CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                error = "Cannot access destination directory: " +
                    std::system_category().message(GetLastError());
                return false;
            }
            locks.emplace_back(handle, [](void* value) { CloseHandle(value); });
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(handle, &info))
            {
                error = "Cannot inspect destination directory: " +
                    std::system_category().message(GetLastError());
                return false;
            }
            if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                error = "Destination must contain only ordinary directories, without links or junctions.";
                return false;
            }
        }

        std::error_code ec;
        if (!std::filesystem::create_directory(directory / filename, ec))
        {
            error = ec ? "Cannot create folder: " + ec.message() :
                "A file or folder with this name already exists.";
            return false;
        }
        return true;
    }
}

AssetBrowser::AssetBrowser() = default;

void AssetBrowser::InvalidateFileListings()
{
    m_listings.clear();
    m_activeSceneIdentity.reset();
}

std::optional<std::filesystem::path> AssetBrowser::TakeSceneOpenRequest()
{
    return std::exchange(m_sceneOpenRequest, std::nullopt);
}

bool AssetBrowser::IsSceneEntry(const std::filesystem::path& path, const ProjectContext& project) const
{
    if(!ProjectAssetRegistry::PathsEqual(path.extension(), ".scene"))
    {
        return false;
    }

    std::string error;
    std::error_code filesystemError;
    const auto root = project.ResolveCanonicalRoot(filesystemError);

    if(filesystemError || !CanBrowse(path.parent_path(), error))
    {
        return false;
    }

    const auto resolved = std::filesystem::canonical(path, filesystemError);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return !filesystemError && IsPathWithin(resolved, root) &&
        attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

bool AssetBrowser::IsActiveSceneEntry(const std::filesystem::path& path, const ProjectContext& project,
    const FileBackedDemoScene* activeScene) const
{
    if(!activeScene || activeScene->IsUnsaved() || activeScene->IsArchiveActivation())
    {
        return false;
    }

    std::error_code error;
    const auto projectRoot = project.ResolveCanonicalRoot(error);

    if(error)
    {
        return false;
    }

    const auto ownerRoot = std::filesystem::canonical(activeScene->GetProjectRoot(), error);

    if(error || !ProjectAssetRegistry::PathsEqual(ownerRoot, projectRoot))
    {
        return false;
    }

    const auto activePath = std::filesystem::canonical(activeScene->GetScenePath(), error);

    if(error || !IsPathWithin(activePath, projectRoot))
    {
        return false;
    }

    const auto entryPath = std::filesystem::canonical(path, error);
    return !error && ProjectAssetRegistry::PathsEqual(activePath, entryPath);
}

bool AssetBrowser::RequestSceneOpen(const std::filesystem::path& path, const ProjectContext& project)
{
    if(m_sceneOpenRequest || !IsSceneEntry(path, project))
    {
        return false;
    }

    // No scene document or renderer ownership crosses this request boundary.
    m_sceneOpenRequest = path;
    return true;
}

std::optional<AssetBrowser::MaterialCreationRequest> AssetBrowser::TakeMaterialCreationRequest()
{
    return std::exchange(
        m_materialRequest,
        std::nullopt);
}

void AssetBrowser::OnMaterialCreationCompleted(
    const std::filesystem::path& relativePath,
    const std::string& error)
{
    m_materialCreationPending = false;
    m_error = error;

    if (error.empty())
    {
        const auto path = m_root / relativePath;
        m_listings.erase(path.parent_path());

        if (m_directory == path.parent_path())
        {
            m_selection = path;
        }
    }
}

bool AssetBrowser::CanCreateMaterial(std::string& error) const
{
    const auto materials = ProjectPaths(m_root).GetMaterialsDirectory();
    const auto relative = m_directory.lexically_relative(materials);

    if (relative.empty() || relative.is_absolute())
    {
        return false;
    }

    for (const auto& component : relative)
    {
        if (component == "..")
        {
            return false;
        }
    }

    return CanBrowse(
        m_directory,
        error);
}

std::optional<AssetBrowser::AcousticMaterialCreationRequest> AssetBrowser::TakeAcousticMaterialCreationRequest()
{
    return std::exchange(m_acousticRequest, std::nullopt);
}

// Built-in presets are read-only, so this is how one becomes an editable project asset: the new file starts with its values.
void AssetBrowser::DrawAcousticMaterialCreationModal()
{
    if(m_openAcousticModal)
    {
        ImGui::OpenPopup("Create Acoustic Material");
        m_openAcousticModal = false;
    }

    bool creationOpen = true;

    if(!ImGui::BeginPopupModal("Create Acoustic Material", &creationOpen, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }

    if(ImGui::IsWindowAppearing())
    {
        ImGui::SetKeyboardFocusHere();
    }

    ImGui::InputText("Name", m_acousticName.data(), m_acousticName.size());
    const std::vector<std::string> presets = AcousticMaterials::BuiltinReferences();
    m_acousticPreset = std::clamp(m_acousticPreset, 0, static_cast<int>(presets.size()) - 1);

    if(ImGui::BeginCombo("Start from", AcousticMaterials::BuiltinDisplayName(presets[m_acousticPreset]).c_str()))
    {
        for(int index = 0; index < static_cast<int>(presets.size()); ++index)
        {
            if(ImGui::Selectable(AcousticMaterials::BuiltinDisplayName(presets[index]).c_str(), index == m_acousticPreset))
            {
                m_acousticPreset = index;
            }
        }

        ImGui::EndCombo();
    }

    if(!m_acousticError.empty())
    {
        ImGui::TextWrapped("%s", m_acousticError.c_str());
    }

    ImGui::BeginDisabled(m_materialCreationPending);

    if(ImGui::Button("Create"))
    {
        m_acousticError.clear();

        try
        {
            std::filesystem::path filename;

            if(CreationFilename(m_acousticName.data(), filename, m_acousticError, false) && CanBrowse(m_acousticDirectory, m_acousticError))
            {
                if(!filename.has_extension())
                {
                    filename += std::wstring(AcousticMaterials::FileExtension);
                }

                const auto destination = m_acousticDirectory / filename;
                std::error_code ec;

                if(_wcsicmp(filename.extension().c_str(), std::wstring(AcousticMaterials::FileExtension).c_str()) != 0)
                {
                    m_acousticError = "The filename extension must be .acousticmaterial.";
                }
                else if(std::filesystem::exists(destination, ec) || ec)
                {
                    m_acousticError = ec ? "Cannot inspect destination: " + ec.message() : "A file with this name already exists.";
                }
                else
                {
                    m_acousticRequest = AcousticMaterialCreationRequest{destination.lexically_relative(m_root),
                        *AcousticMaterials::FindBuiltin(presets[m_acousticPreset])};
                    m_materialCreationPending = true;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        catch(const std::exception& exception)
        {
            m_acousticError = exception.what();
        }
    }

    ImGui::EndDisabled();
    ImGui::SameLine();

    if(ImGui::Button("Cancel"))
    {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

// Copies the saved values under the first free "<name> Copy" name, in the same folder. The copy is a new asset with its own
// identity: references to the original stay on the original.
void AssetBrowser::RequestAcousticMaterialDuplicate(const std::filesystem::path& source)
{
    AcousticMaterialData material;

    if(!AcousticMaterials::LoadFromFile(source, material, &m_error))
    {
        return;
    }

    std::error_code ec;

    for(int attempt = 1; attempt < 1000; ++attempt)
    {
        std::wstring stem = source.stem().native() + L" Copy";

        if(attempt > 1)
        {
            stem += L" " + std::to_wstring(attempt);
        }

        const auto destination = source.parent_path() / (stem + source.extension().native());

        if(!std::filesystem::exists(destination, ec) && !ec)
        {
            m_acousticRequest = AcousticMaterialCreationRequest{destination.lexically_relative(m_root), material};
            m_materialCreationPending = true;
            return;
        }
    }

    m_error = "No free name for a copy of this acoustic material.";
}

void AssetBrowser::DrawMaterialCreationModal()
{
    if (m_openMaterialModal)
    {
        ImGui::OpenPopup("Create Material");
        m_openMaterialModal = false;
    }

    bool creationOpen = true;

    // Closing is equivalent to Cancel: no creation request is issued.
    if (ImGui::BeginPopupModal("Create Material", &creationOpen, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }

        ImGui::InputText("Name", m_materialName.data(), m_materialName.size());

        if (!m_materialError.empty())
        {
            ImGui::TextWrapped("%s", m_materialError.c_str());
        }

        ImGui::BeginDisabled(m_materialCreationPending);

        if (ImGui::Button("Create"))
        {
            m_materialError.clear();

            try
            {
                std::filesystem::path filename;

                if (CreationFilename(m_materialName.data(), filename, m_materialError, true) &&
                    CanBrowse(m_materialDirectory, m_materialError))
                {
                    const auto destination = m_materialDirectory / filename;
                    std::error_code ec;
                    const bool exists = std::filesystem::exists(destination, ec);

                    if (ec || exists)
                    {
                        m_materialError = ec ? "Cannot inspect destination: " + ec.message() :
                            "A file with this name already exists.";
                    }
                    else
                    {
                        m_materialRequest = MaterialCreationRequest{destination.lexically_relative(m_root)};
                        m_materialCreationPending = true;
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
            catch (const std::exception& exception)
            {
                m_materialError = exception.what();
            }
        }

        ImGui::EndDisabled();
        ImGui::SameLine();

        if (ImGui::Button("Cancel"))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void AssetBrowser::DrawFolderCreationModal()
{
    if (m_openFolderModal)
    {
        ImGui::OpenPopup("Create Folder");
        m_openFolderModal = false;
    }

    bool creationOpen = true;
    if (ImGui::BeginPopupModal("Create Folder", &creationOpen, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }
        const bool submitted = ImGui::InputText("Name", m_folderName.data(), m_folderName.size(),
            ImGuiInputTextFlags_EnterReturnsTrue);
        if (!m_folderError.empty())
        {
            ImGui::TextWrapped("%s", m_folderError.c_str());
        }
        const bool cancelled = ImGui::IsKeyPressed(ImGuiKey_Escape);
        if ((ImGui::Button("Create") || submitted) && !cancelled)
        {
            m_folderError.clear();
            try
            {
                std::filesystem::path filename;
                if (CreationFilename(m_folderName.data(), filename, m_folderError, false) &&
                    CanBrowse(m_folderDirectory, m_folderError) &&
                    CreateBrowserFolder(m_folderDirectory, filename, m_folderError))
                {
                    m_listings.erase(m_folderDirectory);
                    m_error.clear();
                    ImGui::CloseCurrentPopup();
                }
            }
            catch (const std::exception& exception)
            {
                m_folderError = exception.what();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || cancelled)
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

bool AssetBrowser::CanBrowse(
    const std::filesystem::path& path,
    std::string& error) const
{
    const auto relative = path.lexically_relative(m_root);

    if (relative.empty() || relative.is_absolute() ||
        std::any_of(relative.begin(), relative.end(), [](const auto& component) { return component == ".."; }))
    {
        error = "Directory is outside the project root.";
        return false;
    }

    // Refuse directory reparse points, including junctions, even when their target
    // is inside the project. This also prevents cycles in the directory tree.
    auto current = m_root.root_path();

    for (const auto& component : path.relative_path())
    {
        current /= component;
        const DWORD attributes = GetFileAttributesW(current.c_str());

        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            error = "Cannot access directory: " + DisplayName(current) + " (" +
                std::system_category().message(GetLastError()) + ")";
            return false;
        }

        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            error = "Directory links and junctions are not browsed: " + DisplayName(current);
            return false;
        }
    }

    std::error_code ec;

    if (!std::filesystem::is_directory(path, ec) || ec)
    {
        error = ec ? ec.message() : "Directory no longer exists.";
        return false;
    }

    return true;
}

AssetBrowser::Listing& AssetBrowser::GetListing(const std::filesystem::path& path)
{
    auto [position, inserted] = m_listings.try_emplace(path);
    auto& listing = position->second;

    if (!inserted)
    {
        return listing;
    }

    if (!CanBrowse(path, listing.error))
    {
        return listing;
    }

    listing.browsable = true;

    std::error_code ec;
    std::filesystem::directory_iterator iterator(
        path,
        ec);
    const std::filesystem::directory_iterator end;

    while (!ec && iterator != end)
    {
        const auto entryPath = iterator->path();
        const DWORD attributes = GetFileAttributesW(entryPath.c_str());

        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            listing.error = "Cannot inspect " + DisplayName(entryPath) + ": " +
                std::system_category().message(GetLastError());
        }
        else
        {
            const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool reparsePoint = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            Entry entry;
            entry.path = entryPath;
            entry.name = DisplayName(entryPath);
            entry.directory = directory;
            entry.navigable = directory && !reparsePoint;
            entry.ordinaryFile = !directory && !reparsePoint;
            listing.entries.push_back(std::move(entry));
        }

        iterator.increment(ec);
    }

    if (ec)
    {
        listing.error = "Cannot read directory: " + ec.message();
    }

    std::sort(listing.entries.begin(), listing.entries.end(), [](const Entry& left, const Entry& right)
    {
        if (left.directory != right.directory)
        {
            return left.directory;
        }

        return left.name < right.name;
    });
    return listing;
}

void AssetBrowser::ClassifySceneEntries(Listing& listing, const ProjectContext& project) const
{
    // A listing built for another project (or before a project switch) is re-resolved once.
    if(!ProjectAssetRegistry::PathsEqual(listing.sceneProjectRoot, project.GetRoot()))
    {
        listing.sceneProjectRoot = project.GetRoot();
        std::error_code rootError;
        listing.sceneRoot = project.ResolveCanonicalRoot(rootError);

        if(rootError)
        {
            listing.sceneRoot.clear();
        }

        for(auto& entry : listing.entries)
        {
            entry.sceneCanonicalPath.reset();
        }
    }

    for(auto& entry : listing.entries)
    {
        if(entry.sceneCanonicalPath)
        {
            continue;
        }

        entry.sceneCanonicalPath.emplace();
        const bool sceneCandidate = entry.ordinaryFile && listing.browsable && !listing.sceneRoot.empty() &&
            ProjectAssetRegistry::PathsEqual(entry.path.extension(), ".scene");

        if(!sceneCandidate)
        {
            continue;
        }

        // Same containment rule as IsSceneEntry; the directory walk is already covered by listing.browsable.
        std::error_code canonicalError;
        auto resolved = std::filesystem::canonical(entry.path, canonicalError);

        if(!canonicalError && IsPathWithin(resolved, listing.sceneRoot))
        {
            *entry.sceneCanonicalPath = std::move(resolved);
        }
    }
}

const std::filesystem::path& AssetBrowser::GetActiveScenePath(const ProjectContext& project,
    const FileBackedDemoScene* activeScene)
{
    static const std::filesystem::path none;
    const bool eligible = activeScene && !activeScene->IsUnsaved() && !activeScene->IsArchiveActivation();
    const auto& scenePath = eligible ? activeScene->GetScenePath() : none;
    const auto& ownerRoot = eligible ? activeScene->GetProjectRoot() : none;

    const bool current = m_activeSceneIdentity &&
        m_activeSceneIdentity->eligible == eligible &&
        m_activeSceneIdentity->projectRoot == project.GetRoot() &&
        m_activeSceneIdentity->scenePath == scenePath &&
        m_activeSceneIdentity->sceneOwnerRoot == ownerRoot;

    if(current)
    {
        return m_activeSceneIdentity->canonicalScenePath;
    }

    // Resolution mirrors IsActiveSceneEntry: same owning project, and the scene inside that project.
    ActiveSceneIdentity identity;
    identity.projectRoot = project.GetRoot();
    identity.scenePath = scenePath;
    identity.sceneOwnerRoot = ownerRoot;
    identity.eligible = eligible;

    if(eligible)
    {
        identity.canonicalScenePath = ResolveActiveScenePath(project, ownerRoot, scenePath);
    }

    m_activeSceneIdentity = std::move(identity);
    return m_activeSceneIdentity->canonicalScenePath;
}

std::filesystem::path AssetBrowser::ResolveActiveScenePath(const ProjectContext& project,
    const std::filesystem::path& ownerRoot, const std::filesystem::path& scenePath)
{
    std::error_code error;
    const auto projectRoot = project.ResolveCanonicalRoot(error);

    if(error)
    {
        return {};
    }

    const auto canonicalOwner = std::filesystem::canonical(ownerRoot, error);

    if(error || !ProjectAssetRegistry::PathsEqual(canonicalOwner, projectRoot))
    {
        return {};
    }

    auto activePath = std::filesystem::canonical(scenePath, error);

    if(error || !IsPathWithin(activePath, projectRoot))
    {
        return {};
    }

    return activePath;
}

bool AssetBrowser::CanRequestFileOperation(const std::filesystem::path& path, std::string& error) const
{
    // Live re-check before a destructive request: cached listing facts never authorize an operation.
    // ProjectAssetDeletion/rename still perform their own pinned containment checks when executing.
    const DWORD attributes = GetFileAttributesW(path.c_str());

    if(attributes == INVALID_FILE_ATTRIBUTES)
    {
        error = "Cannot access " + DisplayName(path) + ": " + std::system_category().message(GetLastError());
        return false;
    }

    if((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
    {
        error = DisplayName(path) + " is no longer an ordinary file.";
        return false;
    }

    if(!m_thumbnailPaths || path == m_thumbnailPaths->GetAssetRegistryPath())
    {
        error = "The project asset registry cannot be modified here.";
        return false;
    }

    return CanBrowse(path.parent_path(), error);
}

bool AssetBrowser::RevealMaterial(const AssetID& id,const ProjectPaths& paths,const ProjectAssetRegistry& registry,std::string& error)
{
    error.clear();
    const auto* record=registry.Find(id);
    if(!record || (record->type!=AssetType::Material && record->type!=AssetType::AcousticMaterial)) { error="Assigned material is not registered as a project material."; return false; }
    const auto resolved=ProjectAssetResolver::Resolve(paths,registry,id);
    if(resolved.status!=AssetAvailability::Available) { error="Assigned material is missing or unavailable."; return false; }
    auto directory=resolved.resolvedPath.parent_path();
    if(!CanBrowse(directory,error)) return false;
    // No selection/selection-intent writes, no renderer work, no focus transfer.
    m_listings.erase(directory);
    m_directory.swap(directory);
    m_thumbnailResetRequested=true; m_materialRequestsReset=true;
    m_navigationHighlight=id; m_revealPending=true;
    return true;
}
void AssetBrowser::Navigate(const std::filesystem::path& path)
{
    m_error.clear();

    if (CanBrowse(path, m_error))
    {
        m_thumbnailResetRequested = true;
        m_materialRequestsReset = true;
        m_directory = path;
        m_listings.erase(path);
        m_activeSceneIdentity.reset();
        m_navigationHighlight.reset(); m_revealPending=false;
        m_selection.clear();
    }
}

void AssetBrowser::DrawTree(
    const std::filesystem::path& path,
    const std::string& label)
{
    ImGui::PushID(PathID(path).c_str());
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;

    if (path == m_root)
    {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }

    if (path == m_directory)
    {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    const float iconSize = ImGui::GetFontSize();
    // Reserve label space inside the original tree item; decoration adds no input target.
    const float spaceWidth = (std::max)(1.0f, ImGui::CalcTextSize(" ").x);
    const size_t paddingSpaces = m_icons ?
        static_cast<size_t>((iconSize + ImGui::GetStyle().ItemInnerSpacing.x) / spaceWidth) + 1 : 0;
    const std::string padding(paddingSpaces, ' ');
    const bool open = ImGui::TreeNodeEx("##Directory", flags, "%s%s", padding.c_str(), label.c_str());

    if(m_icons)
    {
        const auto start = ImGui::GetItemRectMin();
        m_icons->Draw(*ImGui::GetWindowDrawList(), EditorIcon::Folder,
            ImVec2(start.x + ImGui::GetTreeNodeToLabelSpacing(), start.y), ImVec2(iconSize, iconSize),
            ImGui::GetColorU32(ImGuiCol_Text));
    }

    CaptureDropTarget(path);
    DrawAssetMoveTarget(path);

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
    {
        Navigate(path);
    }

    if (open)
    {
        const auto& listing = GetListing(path);

        if (!listing.error.empty())
        {
            ImGui::TextWrapped("%s", listing.error.c_str());
        }

        for (const auto& entry : listing.entries)
        {
            if (entry.navigable)
            {
                DrawTree(entry.path, entry.name);
            }
            else if (entry.directory)
            {
                ImGui::TextDisabled("[link] %s", entry.name.c_str());
            }
        }

        ImGui::TreePop();
    }

    ImGui::PopID();
}

void AssetBrowser::DrawContents(const ProjectContext* project, const FileBackedDemoScene* activeScene)
{
    auto& listing = GetListing(m_directory);

    if (!listing.error.empty())
    {
        ImGui::TextWrapped("%s", listing.error.c_str());
    }

    // Filesystem facts are resolved once per listing (and per active-scene change), not per tile per frame.
    EditorPhaseTiming::Scope classificationPhase(m_phaseTiming,
        EditorPhaseTiming::Phase::AssetBrowserContentsFilesystem);
    static const std::filesystem::path noActiveScene;

    if(project)
    {
        ClassifySceneEntries(listing, *project);
    }

    const auto& activeScenePath = project ? GetActiveScenePath(*project, activeScene) : noActiveScene;
    classificationPhase.End();

    const auto& style = ImGui::GetStyle();
    const float compactIconSize = ImGui::GetFontSize() * 2.0f;
    const float compactTileWidth = ImGui::GetFontSize() * 6.0f + style.FramePadding.x * 2.0f;
    const float compactTileHeight = compactIconSize + style.ItemInnerSpacing.y +
        ImGui::GetTextLineHeight() + style.FramePadding.y * 2.0f;
    EditorPhaseTiming::Scope previewLookupPhase(m_phaseTiming, EditorPhaseTiming::Phase::AssetBrowserContentsLookup);
    const bool hasPreviewTiles = std::any_of(listing.entries.begin(), listing.entries.end(), [&](const Entry& entry)
    {
        const auto path = entry.path.lexically_normal();
        return !entry.directory && m_filter.PassFilter(entry.name.c_str()) &&
            (m_textureRecords.contains(path) || m_materialRecords.contains(path) || m_meshRecords.contains(path));
    });
    previewLookupPhase.End();
    // Fixed columns prevent sparse listings from stretching small icons across the panel.
    // Keep enough room for the existing 96px previews in mixed listings.
    const float columnWidth = hasPreviewTiles ? (std::max)(96.0f, compactTileWidth) : compactTileWidth;
    const float columnPitch = columnWidth + style.CellPadding.x * 2.0f + style.ItemSpacing.x;
    const int columns = (std::max)(1, static_cast<int>(ImGui::GetContentRegionAvail().x / columnPitch));
    std::filesystem::path destination;
    bool tileHovered = false;

    // Set when a live re-check contradicts the cached listing; the listing is rebuilt after the loop
    // because erasing it here would invalidate the entries being iterated.
    bool staleListing = false;
    const auto now = ThumbnailAdmission::Clock::now();

    if (ImGui::BeginTable("Files", columns, ImGuiTableFlags_SizingFixedSame))
    {
        for(int column = 0; column < columns; ++column)
        {
            ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, columnWidth + style.ItemSpacing.x);
        }

        for (const auto& entry : listing.entries)
        {
            if(!m_filter.PassFilter(entry.name.c_str()))
            {
                continue;
            }

            ImGui::TableNextColumn();
            ImGui::PushID(PathID(entry.path).c_str());
            // Compact tiles submit one item containing both icon and filename.
            // Thumbnail tiles keep their existing preview and wrapped-label layout.
            ImGui::BeginGroup();
            // Per-entry scopes add to one per-frame sample for their phase.
            EditorPhaseTiming::Scope lookupPhase(m_phaseTiming, EditorPhaseTiming::Phase::AssetBrowserContentsLookup);
            const bool sceneDocument = !entry.directory && ProjectAssetRegistry::PathsEqual(entry.path.extension(), ".scene");
            const bool scriptDocument = !entry.directory && ProjectAssetRegistry::PathsEqual(entry.path.extension(), ".as");
            // Only a .as entry can be a script, so other entries skip the registry scan.
            const auto* scriptRecord = scriptDocument && project && m_thumbnailRegistry ?
                m_thumbnailRegistry->FindByPath(entry.path.lexically_relative(project->GetRoot())) : nullptr;
            const bool isScript = scriptDocument && scriptRecord && scriptRecord->type == AssetType::Script;
            const auto registered = m_textureRecords.find(entry.path.lexically_normal());
            const bool isTexture = !entry.directory && !sceneDocument && registered != m_textureRecords.end();
            const auto material = m_materialRecords.find(entry.path.lexically_normal());
            const bool isMaterial = !entry.directory && material != m_materialRecords.end() &&
                _wcsicmp(entry.path.extension().c_str(), L".material") == 0;
            const bool isMesh = !entry.directory && !sceneDocument && m_meshRecords.contains(entry.path.lexically_normal());
            lookupPhase.End();
            const bool isScene = sceneDocument && project &&
                entry.sceneCanonicalPath && !entry.sceneCanonicalPath->empty();
            const bool active = isScene && !activeScenePath.empty() &&
                ProjectAssetRegistry::PathsEqual(*entry.sceneCanonicalPath, activeScenePath);
            // M4: an icon tile; acoustic materials have no visual preview, so no thumbnail is ever requested.
            const auto* acousticRecord = !entry.directory && project && m_thumbnailRegistry &&
                _wcsicmp(entry.path.extension().c_str(), L".acousticmaterial") == 0 ?
                m_thumbnailRegistry->FindByPath(entry.path.lexically_relative(project->GetRoot())) : nullptr;
            const bool isAcoustic = acousticRecord && acousticRecord->type == AssetType::AcousticMaterial;
            const bool hasThumbnail = isTexture || isMaterial || isMesh;
            const bool compactTile = !hasThumbnail && !isScript;
            const auto tilePosition = ImGui::GetCursorScreenPos();
            constexpr float thumbnailSize = 96.0f;
            const char* tileLabel = isTexture ? "##Texture" : isMaterial ? "##Material" :
                isMesh ? "##Mesh" : isScene ? "##Scene" : isScript ? "[Script]" :
                compactTile ? "##Entry" : entry.directory ? "[Folder]" : "[File]";
            const ImVec2 tileSize = compactTile ? ImVec2(compactTileWidth, compactTileHeight) :
                hasThumbnail ? ImVec2(thumbnailSize, thumbnailSize) : ImVec2(0.0f, 48.0f);
            const ImGuiSelectableFlags tileFlags = ImGuiSelectableFlags_AllowDoubleClick |
                (compactTile ? ImGuiSelectableFlags_NoPadWithHalfSpacing : ImGuiSelectableFlags_None);
            const bool pressed = ImGui::Selectable(tileLabel,
                m_selection == entry.path, tileFlags, tileSize);
            const bool tileVisible = ImGui::IsItemVisible();

            if(compactTile && tileVisible)
            {
                auto* draw = ImGui::GetWindowDrawList();
                const ImVec2 iconPosition(tilePosition.x + (tileSize.x - compactIconSize) * 0.5f,
                    tilePosition.y + style.FramePadding.y);

                if(m_icons)
                {
                    m_icons->Draw(*draw, entry.directory ? EditorIcon::Folder : isAcoustic ? EditorIcon::AudioWaveform :
                        sceneDocument ? EditorIcon::Box : EditorIcon::File,
                        iconPosition, ImVec2(compactIconSize, compactIconSize), ImGui::GetColorU32(ImGuiCol_Text));
                }
                else
                {
                    draw->AddRect(iconPosition, ImVec2(iconPosition.x + compactIconSize,
                        iconPosition.y + compactIconSize), ImGui::GetColorU32(ImGuiCol_Text));
                }

                const float labelWidth = tileSize.x - style.FramePadding.x * 2.0f;
                const auto textSize = ImGui::CalcTextSize(entry.name.c_str());
                const ImVec2 labelPosition(tilePosition.x + (tileSize.x - (std::min)(textSize.x, labelWidth)) * 0.5f,
                    iconPosition.y + compactIconSize + style.ItemInnerSpacing.y);
                const ImVec2 labelEnd(tilePosition.x + tileSize.x - style.FramePadding.x,
                    labelPosition.y + ImGui::GetTextLineHeight());
                ImGui::RenderTextEllipsis(draw, labelPosition, labelEnd, labelEnd.x,
                    entry.name.c_str(), nullptr, &textSize);
            }

            if(isMesh)
            {
                DrawMeshDragSource(entry.path, entry.name);
            }
            else if(isTexture || isMaterial || isAcoustic)
            {
                DrawAssetMoveSource(entry.path, entry.name);
            }

            const TextureThumbnailCache::Preview* preview = nullptr;

            std::string materialError;
            std::string tileFailure;

            if(hasThumbnail && tileVisible)
            {
                if(isTexture && !m_thumbnailResetRequested && m_thumbnailPaths && m_thumbnailRegistry)
                {
                    preview = m_thumbnails->FindOrCreate(*m_thumbnailPaths, *m_thumbnailRegistry, registered->second);
                }

                auto* draw = ImGui::GetWindowDrawList();
                const ImVec2 start(tilePosition.x + 3.0f, tilePosition.y + 3.0f);
                const ImVec2 end(tilePosition.x + thumbnailSize - 3.0f, tilePosition.y + thumbnailSize - 3.0f);
                draw->AddRectFilled(start, end, IM_COL32(55, 55, 55, 255));

                if(preview && !preview->invalidated && preview->error.empty() && preview->texture.GetResource())
                {
                    const float scale = (thumbnailSize - 6.0f) / static_cast<float>((std::max)(preview->width, preview->height));
                    const ImVec2 size(preview->width * scale, preview->height * scale);
                    const ImVec2 position(tilePosition.x + (thumbnailSize - size.x) * 0.5f,
                        tilePosition.y + (thumbnailSize - size.y) * 0.5f);
                    draw->AddImage(static_cast<ImTextureID>(m_thumbnails->GetHandle(*preview).ptr), position,
                        ImVec2(position.x + size.x, position.y + size.y));
                }
                else if(isMaterial && m_materialRenderer && m_thumbnailPaths && m_thumbnailRegistry)
                {
                    // Resident tiles display their finished image and join the renderer's working set
                    // directly; only the remaining tiles go through admission (B.2 rotation).
                    const auto key = material->second.id.ToReference();
                    std::optional<MaterialPreviewImage> image = GetResidentMaterialThumbnail(key, material->second.id);
                    const auto admission = image ? ThumbnailAdmission::TileState::Admitted :
                        m_materialAdmission.MarkVisible(key, now);
                    const char* status = admission == ThumbnailAdmission::TileState::CoolingDown ? "Unavailable" : "Queued";

                    if(!image && admission == ThumbnailAdmission::TileState::Admitted)
                    {
                        auto& diagnostics = MaterialThumbnailDiagnostics::Get();

                        {
                            MaterialThumbnailDiagnostics::Timer requestTimer(diagnostics.browserRequestMicroseconds);
                            MaterialThumbnailDiagnostics::Add(diagnostics.browserRequests);
                            m_materialRenderer->RequestSavedMaterialThumbnail(
                                *m_thumbnailPaths, *m_thumbnailRegistry, material->second.id, materialError);
                        }

                        // Lookup registers this exact image's current ImGui reference with Renderer.
                        MaterialThumbnailDiagnostics::Timer lookupTimer(diagnostics.imageLookupMicroseconds);
                        MaterialThumbnailDiagnostics::Add(diagnostics.imageLookups);
                        image = m_materialRenderer->GetSavedMaterialThumbnailImage(*m_thumbnailPaths, material->second.id);
                        status = "Generating";

                        if(image)
                        {
                            m_materialAdmission.MarkShown(key, now);
                            m_thumbnailFailures.erase(key);
                            m_materialResidents[key] = ResidentMaterialThumbnail{now, true, image->cacheSlot, image->cacheSerial};
                        }
                        else if(!materialError.empty())
                        {
                            m_materialAdmission.MarkFailed(key, now);
                            m_thumbnailFailures[key] = {materialError, false};
                            status = "Unavailable";
                        }
                    }

                    RecordMaterialDisplayForDiagnostics(key, image.has_value());

                    if(image)
                    {
                        draw->AddImage(static_cast<ImTextureID>(image->srv.ptr), start, end);
                    }
                    else
                    {
                        DrawThumbnailStatus(draw, start, "Material", status);
                        tileFailure = FindThumbnailFailure(key);
                    }
                }
                else if(isMesh && m_materialRenderer && m_thumbnailPaths && m_thumbnailRegistry)
                {
                    const auto& record = m_meshRecords.at(entry.path.lexically_normal());
                    const auto key = record.id.ToReference();

                    // A resident tile displays its retained image without occupying a generation slot.
                    std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> image = GetResidentMeshThumbnail(record);
                    const auto admission = image ? ThumbnailAdmission::TileState::Admitted :
                        m_meshAdmission.MarkVisible(key, now);
                    const auto failure = m_thumbnailFailures.find(key);
                    const bool terminalFailure = failure != m_thumbnailFailures.end() && failure->second.terminal;
                    const char* status = admission != ThumbnailAdmission::TileState::CoolingDown ? "Queued" :
                        terminalFailure ? "No preview" : "Retrying soon";

                    if(!image && admission == ThumbnailAdmission::TileState::Admitted)
                    {
                        bool terminal = false;
                        image = GetVisibleMeshThumbnail(record, materialError, terminal);
                        status = "Generating";

                        if(image)
                        {
                            m_meshAdmission.MarkShown(key, now);
                            m_thumbnailFailures.erase(key);
                            auto& residentRequest = m_meshThumbnailRequests[key];
                            residentRequest.resident = true;
                            residentRequest.residentSince = now;
                        }
                        else if(!materialError.empty())
                        {
                            m_meshAdmission.MarkFailed(key, now);
                            m_thumbnailFailures[key] = {materialError, terminal};
                            status = terminal ? "No preview" : "Retrying soon";
                        }
                    }

                    if(image)
                    {
                        draw->AddImage(static_cast<ImTextureID>(image->ptr), start, end);
                    }
                    else
                    {
                        DrawThumbnailStatus(draw, start, "Mesh", status);
                        tileFailure = FindThumbnailFailure(key);
                    }
                }
                else
                {
                    draw->AddText(ImVec2(start.x + 5.0f, start.y + 5.0f), IM_COL32(190, 190, 190, 255),
                        isMaterial ? "Material" : isMesh ? "Mesh" : "Texture");
                }
            }

            if(!compactTile)
            {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(entry.name.c_str());

                ImGui::PopTextWrapPos();
            }
            if(active)
            {
                ImGui::TextDisabled("Active");
            }
            ImGui::EndGroup();
            CaptureDropTarget(entry.navigable ? entry.path : std::filesystem::path{});
            const bool located=m_navigationHighlight && ((isMaterial && material->second.id==*m_navigationHighlight) ||
                (isAcoustic && acousticRecord->id==*m_navigationHighlight));
            if(located)
            {
                if(m_revealPending) { ImGui::SetScrollHereY(0.5f); m_revealPending=false; }
                const auto minimum=ImGui::GetItemRectMin(), maximum=ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRect(ImVec2(minimum.x-2,minimum.y-2),ImVec2(maximum.x+2,maximum.y+2),
                    IM_COL32(255,190,65,255),4.0f,0,3.0f);
            }
            const bool hovered = ImGui::IsItemHovered();
            tileHovered = tileHovered || hovered;

            // Failure detail on demand only; scheduling never logs per tile.
            if(hovered && !tileFailure.empty())
            {
                ImGui::SetTooltip("%s", tileFailure.c_str());
            }
            if(entry.navigable)
            {
                DrawAssetMoveTarget(entry.path);
            }

            if (pressed || (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))
            {
                m_navigationHighlight.reset(); m_revealPending=false;
                m_selection = entry.path;
                m_selectionIntent = true;

                if (entry.directory && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    destination = entry.path;
                }
                else if(isScene && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    RequestSceneOpen(entry.path, *project);
                }
            }

            if (hovered)
            {
                ImGui::SetTooltip("%s%s%s", entry.name.c_str(),
                    entry.directory && !entry.navigable ? " (directory link; browsing disabled)" : "",
                    preview && !preview->error.empty() ? ("\n" + preview->error).c_str() :
                    !materialError.empty() ? ("\n" + materialError).c_str() : "");
            }

            // Only decides whether the menu is offered; Rename/Delete re-check the filesystem when chosen.
            const bool eligible = entry.ordinaryFile && listing.browsable && m_thumbnailPaths &&
                entry.path != m_thumbnailPaths->GetAssetRegistryPath();

            if(eligible && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                ImGui::OpenPopup("FileContext");
            }

            if(eligible && ImGui::BeginPopup("FileContext"))
            {
                const bool canRequest = m_deleteTarget.empty() && !m_deleteRequest && m_renameTarget.empty();

                if(isScene && ImGui::MenuItem("Open Scene", nullptr, false, !m_sceneOpenRequest))
                {
                    RequestSceneOpen(entry.path, *project);
                }

                // Scene file operations need lifecycle/workspace coordination in a later milestone.
                const bool canModify = canRequest && !sceneDocument && !scriptDocument;

                if(isMesh && ImGui::MenuItem("Import Units...", nullptr, false, canRequest && !m_unitsRequest))
                {
                    m_unitsAsset = m_meshRecords.at(entry.path.lexically_normal());
                    m_unitsDraft = m_unitsAsset->meshImportUnits;
                    m_unitsManualScale = static_cast<float>(m_unitsDraft.scale);
                    m_openUnits = true;

                    try
                    {
                        const double metres = ModelImporter::InspectFBXUnitScale(entry.path);
                        m_unitsSourceInfo = std::format("Effective FBX metres per source unit: {:.9g}", metres);
                    }
                    catch(const std::exception& error)
                    {
                        m_unitsSourceInfo = error.what();
                    }
                }

                std::string operationError;

                if(isAcoustic && ImGui::MenuItem("Duplicate", nullptr, false, canModify && !m_materialCreationPending))
                {
                    RequestAcousticMaterialDuplicate(entry.path);
                }

                if(ImGui::MenuItem("Rename", nullptr, false, canModify))
                {
                    if(CanRequestFileOperation(entry.path, operationError))
                    {
                        m_renameTarget = entry.path;
                        const auto stem = entry.path.stem().u8string();
                        m_renameStem.fill(0);
                        memcpy(m_renameStem.data(), stem.data(), (std::min)(stem.size(), m_renameStem.size() - 1));
                        m_renameError.clear();
                        m_openRenameModal = true;
                        m_focusRename = true;
                    }
                    else
                    {
                        m_error = operationError;
                        staleListing = true;
                    }
                }

                if(ImGui::MenuItem("Delete", nullptr, false, canModify))
                {
                    if(CanRequestFileOperation(entry.path, operationError))
                    {
                        m_deleteTarget = entry.path;
                        m_openDeleteModal = true;
                    }
                    else
                    {
                        m_error = operationError;
                        staleListing = true;
                    }
                }

                ImGui::EndPopup();
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    if (listing.entries.empty() && listing.error.empty())
    {
        ImGui::TextDisabled("This directory is empty.");
    }

    if(staleListing)
    {
        m_listings.erase(m_directory);
        m_activeSceneIdentity.reset();
    }

    if (!destination.empty())
    {
        Navigate(destination);
    }

    if (ImGui::IsWindowHovered() && !tileHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        ImGui::OpenPopup("ContentContext");
    }

    if (ImGui::BeginPopup("ContentContext"))
    {
        std::string error;

        if (CanBrowse(m_directory, error) && ImGui::BeginMenu("Create"))
        {
            if (ImGui::MenuItem("Folder"))
            {
                m_folderDirectory = m_directory;
                std::strcpy(m_folderName.data(), "NewFolder");
                m_folderError.clear();
                m_openFolderModal = true;
            }

            if (ImGui::BeginMenu("Material"))
            {
                // Visual materials keep their rule: only inside the project's Materials folder.
                std::string visualError;
                const bool canCreateVisual = CanCreateMaterial(visualError);

                if (ImGui::MenuItem("Visual Material", nullptr, false, canCreateVisual && !m_materialCreationPending))
                {
                    m_materialDirectory = m_directory;
                    std::strcpy(m_materialName.data(), "NewMaterial");
                    m_materialError.clear();
                    m_openMaterialModal = true;
                }

                if (!canCreateVisual && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip("%s", visualError.c_str());
                }

                if (ImGui::MenuItem("Acoustic Material", nullptr, false, !m_materialCreationPending))
                {
                    m_acousticDirectory = m_directory;
                    std::strcpy(m_acousticName.data(), "NewAcousticMaterial");
                    m_acousticError.clear();
                    m_openAcousticModal = true;
                }

                ImGui::EndMenu();
            }

            ImGui::EndMenu();
        }

        if (!error.empty())
        {
            ImGui::TextWrapped("%s", error.c_str());
        }

        ImGui::EndPopup();
    }
}

void AssetBrowser::Draw(const EditorWorkspaceRect& rectangle, const ProjectContext* project,
    const FileBackedDemoScene* activeScene)
{
    EditorPhaseTiming::Scope setupPhase(m_phaseTiming, EditorPhaseTiming::Phase::AssetBrowserSetup);
    m_thumbnails->BeginFrame();

    for(auto& [identity, request] : m_meshThumbnailRequests)
    {
        request.visible = false;
    }

    for(auto& [identity, resident] : m_materialResidents)
    {
        resident.visible = false;
    }

    m_meshAdmission.BeginFrame();
    m_materialAdmission.BeginFrame();

    if(m_materialRenderer)
    {
        m_materialRenderer->BeginSavedMaterialThumbnailWorkingSet();

        if(m_materialRequestsReset)
        {
            PruneMeshThumbnailRequests(true);
            m_materialRenderer->DiscardSavedMaterialThumbnailRequests();
            m_materialRequestsReset = false;
        }

        if(m_materialImagesInvalidated)
        {
            PruneMeshThumbnailRequests(true);
            m_materialRenderer->InvalidateSavedMaterialThumbnailImages();
            m_materialImagesInvalidated = false;
        }
    }

    m_dropTargets.clear();
    m_dropFrame = ImGui::GetFrameCount();

    if(rectangle.size.x <= 0.0f || rectangle.size.y <= 0.0f)
    {
        PruneMeshThumbnailRequests(true);
        return;
    }

    ImGui::SetNextWindowPos(rectangle.position, ImGuiCond_Always);
    ImGui::SetNextWindowSize(rectangle.size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGuiWindowFlags fixedFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing;

    // ImGui otherwise enforces title-bar height even when the assigned panel is smaller.
    if(rectangle.size.y < ImGui::GetFrameHeight())
    {
        fixedFlags |= ImGuiWindowFlags_NoTitleBar;
    }

    if(ImGui::Begin("Asset Browser", nullptr, fixedFlags))
    {
        EditorWorkspace::KeepCurrentWindowBehindFloatingUI();

        if (ImGui::Button("Refresh"))
        {
            m_scriptRefreshRequested = true;
            m_listings.clear();
            m_activeSceneIdentity.reset();
            m_thumbnailResetRequested = true;
            m_materialRequestsReset = true;
            m_materialImagesInvalidated = true;
            m_selection.clear();
            m_navigationHighlight.reset(); m_revealPending=false;
            m_error.clear();
        }

        ImGui::SameLine();

        if (ImGui::SmallButton("Assets"))
        {
            Navigate(m_root);
        }

        if(project)
        {
            ImGui::SameLine();

            if(ImGui::SmallButton("Scenes"))
            {
                Navigate(project->GetPaths().GetScenesDirectory());
            }

            ImGui::SameLine();

            if(ImGui::SmallButton("Scripts"))
            {
                Navigate(project->GetPaths().GetScriptsDirectory());
            }
        }

        ImGui::SameLine();
        m_filter.Draw("Filter", 150.0f);

        auto breadcrumb = m_root;
        const auto relative = m_directory.lexically_relative(m_root);

        for (const auto& component : relative)
        {
            if (component == ".")
            {
                continue;
            }

            breadcrumb /= component;
            ImGui::SameLine();
            ImGui::TextUnformatted("/");
            ImGui::SameLine();
            ImGui::PushID(PathID(breadcrumb).c_str());

            if (ImGui::SmallButton(DisplayName(component).c_str()))
            {
                Navigate(breadcrumb);
            }

            ImGui::PopID();
        }

        if (!m_error.empty())
        {
            ImGui::TextWrapped("%s", m_error.c_str());
        }

        if(!m_importFeedback.empty())
        {
            ImGui::TextWrapped("%s", m_importFeedback.c_str());
        }

        if (ImGui::BeginTable("BrowserPanes", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Directories", ImGuiTableColumnFlags_WidthFixed, 180.0f);
            ImGui::TableSetupColumn("Contents", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextColumn();
            ImGui::BeginChild("DirectoryTree");
            DrawTree(m_root, "Assets");
            ImGui::EndChild();
            ImGui::TableNextColumn();
            ImGui::BeginChild("ContentGrid");
            CaptureDropTarget(m_directory, true);
            setupPhase.End();
            {
                EditorPhaseTiming::Scope contentsPhase(m_phaseTiming, EditorPhaseTiming::Phase::AssetBrowserContents);
                DrawContents(project, activeScene);
            }
            ImGui::EndChild();
            ImGui::EndTable();
        }

        DrawMaterialCreationModal();
    DrawAcousticMaterialCreationModal();
        DrawFolderCreationModal();
        DrawMeshImportUnits();
    }

    setupPhase.End();
    ImGui::End();
    ImGui::PopStyleVar(2);
    EditorPhaseTiming::Scope thumbnailPhase(m_phaseTiming, EditorPhaseTiming::Phase::AssetBrowserThumbnails);
    PruneMeshThumbnailRequests();
    ScheduleThumbnails();
}

void AssetBrowser::ScheduleThumbnails()
{
    // Browser budgets leave the remaining renderer capacity to the Scene Object Inspector.
    const auto now = ThumbnailAdmission::Clock::now();
    std::vector<std::string> released;
    m_meshAdmission.Schedule(MaterialPreviewRenderer::kMeshThumbnailCapacity, now, released);
    MeshThumbnailDiagnostics::Add(MeshThumbnailDiagnostics::Get().admissionReleases, released.size());

    // A released mesh tile stops requesting and displaying. The renderer retires its image only
    // after every fence and display pin clears, so an in-flight draw is never invalidated.
    for(const auto& key : released)
    {
        const auto request = m_meshThumbnailRequests.find(key);

        // A resident tile leaving admission keeps its finished image; only leaving the view
        // (PruneMeshThumbnailRequests) or invalidation discards it.
        if(request == m_meshThumbnailRequests.end() || request->second.resident)
        {
            continue;
        }

        if(m_materialRenderer && request->second.ticket)
        {
            m_materialRenderer->DiscardMeshThumbnail(request->second.ticket);
        }

        m_meshThumbnailRequests.erase(request);
    }

    YieldResidentMeshThumbnail(now);

    ScheduleMaterialThumbnails(now);
}

std::optional<MaterialPreviewImage> AssetBrowser::GetResidentMaterialThumbnail(const std::string& key, const AssetID& id)
{
    const auto found = m_materialResidents.find(key);

    if(found == m_materialResidents.end() || !m_materialRenderer || !m_thumbnailPaths || !m_thumbnailRegistry)
    {
        return std::nullopt;
    }

    found->second.visible = true;
    auto& diagnostics = MaterialThumbnailDiagnostics::Get();

    // Fast path: an unchanged resident image is pinned and displayed by its cache handle in O(1).
    D3D12_GPU_DESCRIPTOR_HANDLE srv{};
    MaterialPreviewRenderer::ResidentThumbnail resident;

    {
        MaterialThumbnailDiagnostics::Timer fastTimer(diagnostics.residentFastMicroseconds);
        MaterialThumbnailDiagnostics::Add(diagnostics.residentFastLookups);
        resident = m_materialRenderer->PinResidentMaterialThumbnail(found->second.cacheSlot, found->second.cacheSerial, srv);
    }

    if(resident == MaterialPreviewRenderer::ResidentThumbnail::Display)
    {
        MaterialPreviewImage image;
        image.srv = srv;
        image.cacheSlot = found->second.cacheSlot;
        image.cacheSerial = found->second.cacheSerial;
        return image;
    }

    if(resident == MaterialPreviewRenderer::ResidentThumbnail::Invalid)
    {
        // Reassigned, stale (changed material or dependency) or unusable: the tile rejoins admission.
        m_materialResidents.erase(found);
        return std::nullopt;
    }

    // Validation due: the full request path queues the periodic source and dependency validation and
    // pins the image through the working set until the validation has run.
    std::string error;

    {
        MaterialThumbnailDiagnostics::Timer requestTimer(diagnostics.browserRequestMicroseconds);
        MaterialThumbnailDiagnostics::Add(diagnostics.browserRequests);
        m_materialRenderer->RequestSavedMaterialThumbnail(*m_thumbnailPaths, *m_thumbnailRegistry, id, error);
    }

    std::optional<MaterialPreviewImage> image;

    if(error.empty())
    {
        MaterialThumbnailDiagnostics::Timer lookupTimer(diagnostics.imageLookupMicroseconds);
        MaterialThumbnailDiagnostics::Add(diagnostics.imageLookups);
        image = m_materialRenderer->GetSavedMaterialThumbnailImage(*m_thumbnailPaths, id);
    }

    if(!image)
    {
        // Stale (changed material or dependency), evicted or failing: the tile rejoins admission.
        m_materialResidents.erase(found);
        return std::nullopt;
    }

    // Validation never reassigns the displayed entry while it stays current, but keep the handle exact.
    found->second.cacheSlot = image->cacheSlot;
    found->second.cacheSerial = image->cacheSerial;
    return image;
}

void AssetBrowser::ScheduleMaterialThumbnails(std::chrono::steady_clock::time_point now)
{
    std::erase_if(m_materialResidents, [](const auto& resident)
    {
        return !resident.second.visible;
    });

    // Admitted and resident tiles together stay within the residency budget, so every pinned image fits.
    const size_t residentCount = m_materialResidents.size();
    const size_t freeResidency = residentCount >= MaterialPreviewRenderer::kThumbnailResidentCapacity ? 0 :
        MaterialPreviewRenderer::kThumbnailResidentCapacity - residentCount;
    const size_t budget = (std::min)(MaterialPreviewRenderer::kThumbnailCapacity, freeResidency);

    // Released material tiles simply leave the next frame's renderer working set.
    std::vector<std::string> released;
    m_materialAdmission.Schedule(budget, now, released);
    MaterialThumbnailDiagnostics::Add(MaterialThumbnailDiagnostics::Get().admissionReleases, released.size());

    // Over budget with tiles waiting: the longest resident yields after the minimum residency, at most once
    // per minimum-residency interval, so every visible material eventually receives a thumbnail.
    const bool waiting = m_materialAdmission.GetWaitingCount(now) > 0;
    const bool full = residentCount + m_materialAdmission.GetAdmittedCount() >= MaterialPreviewRenderer::kThumbnailResidentCapacity;
    const bool yieldDue = now - m_lastMaterialResidentYield >= ThumbnailAdmission::kMinimumResidency;

    if(!waiting || !full || !yieldDue || m_materialResidents.empty())
    {
        return;
    }

    const auto longest = std::min_element(m_materialResidents.begin(), m_materialResidents.end(),
        [](const auto& left, const auto& right)
        {
            return left.second.since < right.second.since;
        });

    if(now - longest->second.since >= ThumbnailAdmission::kMinimumResidency)
    {
        m_materialResidents.erase(longest);
        m_lastMaterialResidentYield = now;
    }
}

void AssetBrowser::RecordMaterialDisplayForDiagnostics(const std::string& key, bool displayed)
{
#if defined(ENGINE_HAS_VERIFICATION)
    // A tile that has displayed its thumbnail and later draws a status label instead is a visible dropout.
    if(displayed)
    {
        m_materialDisplayedForDiagnostics.insert(key);
    }
    else if(m_materialDisplayedForDiagnostics.contains(key))
    {
        MaterialThumbnailDiagnostics::Add(MaterialThumbnailDiagnostics::Get().displayDropouts);
    }
#else
    (void)key;
    (void)displayed;
#endif
}

void AssetBrowser::YieldResidentMeshThumbnail(std::chrono::steady_clock::time_point now)
{
    // Displayed images are never evictable, so a visible set larger than the residency budget would
    // starve its waiting tiles. Only then does the longest-resident tile yield, after the same minimum
    // residency and at the same one-per-frame rate as admission rotation. Within budget nothing yields.
    size_t residentCount = 0;
    size_t generatingCount = 0;
    auto longestResident = m_meshThumbnailRequests.end();

    for(auto request = m_meshThumbnailRequests.begin(); request != m_meshThumbnailRequests.end(); ++request)
    {
        if(!request->second.resident)
        {
            ++generatingCount;
            continue;
        }

        ++residentCount;

        if(longestResident == m_meshThumbnailRequests.end() ||
            request->second.residentSince < longestResident->second.residentSince)
        {
            longestResident = request;
        }
    }

    const bool overBudget = residentCount + generatingCount > MaterialPreviewRenderer::kMeshResidentImageCapacity;

    // At most one yield per minimum-residency interval: yielded tiles requeue and keep the set over
    // budget, so a per-frame rate would cycle every resident image within a few frames.
    const bool yieldDue = now - m_lastResidentYield >= ThumbnailAdmission::kMinimumResidency;

    if(!overBudget || !yieldDue || generatingCount == 0 || longestResident == m_meshThumbnailRequests.end() ||
        now - longestResident->second.residentSince < ThumbnailAdmission::kMinimumResidency)
    {
        return;
    }

    m_lastResidentYield = now;

    if(m_materialRenderer && longestResident->second.ticket)
    {
        m_materialRenderer->DiscardMeshThumbnail(longestResident->second.ticket);
    }

    m_meshThumbnailRequests.erase(longestResident);
}

std::string AssetBrowser::FindThumbnailFailure(const std::string& key) const
{
    const auto failure = m_thumbnailFailures.find(key);
    return failure == m_thumbnailFailures.end() ? std::string{} : failure->second.message;
}

void AssetBrowser::CaptureDropTarget(const std::filesystem::path& directory, bool contentArea)
{
    const ImGuiWindow* window = ImGui::GetCurrentWindow();
    ImRect bounds = contentArea ? window->InnerRect : ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    bounds.ClipWith(window->ClipRect);
    m_dropTargets.push_back({bounds.Min.x, bounds.Min.y, bounds.Max.x, bounds.Max.y, window->ID, directory});
}

void AssetBrowser::ImportFileDrop(
    const std::vector<std::filesystem::path>& files,
    float screenX,
    float screenY,
    const ProjectPaths& paths,
    ProjectAssetRegistry& registry)
{
    m_importFeedback.clear();
    const ImVec2 point(screenX, screenY);
    const DropTarget* target = nullptr;
    ImGuiID topWindow = 0;

    // Use this frame's final window order, including floating windows and splitters.
    const ImGuiContext& context = *ImGui::GetCurrentContext();

    for(int index = context.Windows.Size - 1; index >= 0; --index)
    {
        const ImGuiWindow* window = context.Windows[index];

        if(window->LastFrameActive != ImGui::GetFrameCount() || window->Hidden ||
           (window->Flags & ImGuiWindowFlags_NoMouseInputs))
        {
            continue;
        }

        if(window->OuterRectClipped.Contains(point))
        {
            topWindow = window->ID;
            break;
        }
    }

    if(m_dropFrame == ImGui::GetFrameCount() &&
       !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    {
        for(auto iterator = m_dropTargets.rbegin(); iterator != m_dropTargets.rend(); ++iterator)
        {
            if(screenX >= iterator->left && screenX < iterator->right &&
               screenY >= iterator->top && screenY < iterator->bottom && iterator->windowID == topWindow)
            {
                target = &*iterator;
                break;
            }
        }
    }

    if(!target || target->directory.empty())
    {
        m_importFeedback = "Drop rejected: use a visible folder or empty Asset Browser content area.";
        Log::Info(m_importFeedback);
        return;
    }

    size_t successes = 0;
    std::string failures;
    std::string renamed;

    for(const auto& source : files)
    {
        const auto category = SupportedAssetImportFormats::Lookup(source);

        if(!category)
        {
            failures += "\n" + source.filename().string() + ": " + SupportedAssetImportFormats::UnsupportedMessage;
            Log::Error("Asset import failed: " + source.string() + ": " + SupportedAssetImportFormats::UnsupportedMessage);
            continue;
        }

        std::filesystem::path imported;
        std::string error;
        const bool isMesh = *category == AssetType::Mesh;
        const bool succeeded = isMesh ?
            ProjectMeshImportService::Import(paths, registry, source, imported, error) :
            ProjectTextureImportService::Import(paths, registry, source, target->directory, imported, error);

        if(succeeded)
        {
            ++successes;

            if(imported.filename() != source.filename())
            {
                renamed += "\nCollision resolved: " + source.filename().string() + " -> " + imported.filename().string();
            }

            Log::Info("Imported asset: " + imported.string());
        }
        else
        {
            failures += "\n" + source.filename().string() + ": " + error;
            Log::Error("Asset import failed: " + source.string() + ": " + error);
        }
    }

    m_importFeedback = "Imported " + std::to_string(successes) + " of " + std::to_string(files.size()) +
        " assets. FBX meshes are stored in Meshes." + renamed + failures;

    if(successes != 0)
    {
        // Refresh without clearing folder navigation, selection, or Inspector edits.
        m_listings.clear();
        m_materialImagesInvalidated = true;
        SetThumbnailProject(paths, registry);
    }
}

void AssetBrowser::SetThumbnailProject(const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    std::error_code projectError;
    const auto canonicalProject = std::filesystem::canonical(paths.GetRoot(), projectError);

    if(projectError || canonicalProject != m_meshDragProject)
    {
        m_unitsAsset.reset();
        m_unitsRequest.reset();
        m_openUnits = false;
        PruneMeshThumbnailRequests(true);
        InvalidateMeshDragIdentity();
        m_meshDragProject = projectError ? std::filesystem::path{} : canonicalProject;
    }

    if(!m_thumbnailProjectRoot.empty() && m_thumbnailProjectRoot != paths.GetRoot())
    {
        m_thumbnailResetRequested = true;
        m_materialRequestsReset = true;
        m_materialImagesInvalidated = true;
    }

    if(m_navigationHighlight)
    {
        const auto* located=registry.Find(*m_navigationHighlight);
        if(m_thumbnailProjectRoot!=paths.GetRoot() || !located || located->type!=AssetType::Material ||
            (paths.GetRoot()/located->relativePath).lexically_normal().parent_path()!=m_directory)
        { m_navigationHighlight.reset(); m_revealPending=false; }
    }
    m_thumbnailProjectRoot = paths.GetRoot();
    m_thumbnailPaths = paths;
    m_thumbnailRegistry = &registry;
    m_textureRecords.clear();
    m_materialRecords.clear();
    m_meshRecords.clear();

    for(const auto& record : registry.GetAssets(AssetType::Texture))
    {
        m_textureRecords.emplace((paths.GetRoot() / record.relativePath).lexically_normal(), record);
    }

    for(const auto& record : registry.GetAssets(AssetType::Material))
    {
        m_materialRecords.emplace((paths.GetRoot() / record.relativePath).lexically_normal(), record);
    }

    for(const auto& record : registry.GetAssets(AssetType::Mesh))
    {
        m_meshRecords.emplace((paths.GetRoot() / record.relativePath).lexically_normal(), record);
    }
}

void AssetBrowser::DrawMeshImportUnits()
{
    if(m_openUnits)
    {
        ImGui::OpenPopup("Mesh Import Units");
        m_openUnits = false;
    }

    if(!ImGui::BeginPopupModal("Mesh Import Units", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }

    if(!m_unitsAsset || !m_thumbnailPaths)
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::TextUnformatted(m_unitsAsset->relativePath.generic_string().c_str());
    const auto savedMode = m_unitsAsset->meshImportUnits.mode;
    const char* savedName = savedMode == MeshImportUnitMode::Legacy ? "Legacy" :
        savedMode == MeshImportUnitMode::FileUnits ? "File Units" : "Manual Scale";
    ImGui::Text("Current saved policy: %s", savedName);
    ImGui::TextWrapped("%s", m_unitsSourceInfo.c_str());
    ImGui::TextWrapped("FBX defaults to centimetres when unit metadata is omitted.");
    int mode = static_cast<int>(m_unitsDraft.mode);

    if(ImGui::Combo("Import policy", &mode, "Legacy\0File Units\0Manual Scale\0"))
    {
        m_unitsDraft.mode = static_cast<MeshImportUnitMode>(mode);
    }

    if(m_unitsDraft.mode == MeshImportUnitMode::Manual)
    {
        InspectorDragFloats("Metres multiplier", &m_unitsManualScale, 1, InspectorDragSpec{});
    }

    ImGui::TextWrapped("Changing this asset changes imported geometry, bounds and previews. "
        "Scene-object transforms stay unchanged. Open a scene that does not reference this mesh first; "
        "reload the scene before using the updated asset.");

    if(ImGui::Button("Validate and Apply"))
    {
        m_unitsDraft.scale = m_unitsDraft.mode == MeshImportUnitMode::Manual ? double(m_unitsManualScale) : 1.0;
        m_unitsRequest = MeshImportUnitsRequest{m_thumbnailPaths->GetRoot(), *m_unitsAsset, m_unitsDraft};
        ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();

    if(ImGui::Button("Cancel"))
    {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

std::optional<AssetBrowser::MeshImportUnitsRequest> AssetBrowser::TakeMeshImportUnitsRequest()
{
    return std::exchange(m_unitsRequest, std::nullopt);
}

void AssetBrowser::OnMeshImportUnitsCompleted(const std::string& error)
{
    m_importFeedback = error.empty() ? "Mesh import units saved. Reload the scene before using this asset." : error;

    if(error.empty())
    {
        PruneMeshThumbnailRequests(true);
    }
}

void AssetBrowser::InvalidateMaterialSource(const std::filesystem::path& path)
{
    if(m_materialRenderer)
    {
        m_materialRenderer->InvalidateSavedMaterialThumbnailSource(path);
    }
}

void AssetBrowser::PauseMaterialThumbnails()
{
    if(m_thumbnails)
    {
        m_thumbnails->ReleaseConsumer(TextureThumbnailCache::Consumer::AssetBrowser);
    }

    PruneMeshThumbnailRequests(true);
    if(m_materialRenderer)
    {
        // The Scene Object Inspector may still admit visible slots with the browser hidden.
        m_materialRenderer->BeginSavedMaterialThumbnailWorkingSet();
        m_materialRenderer->DiscardSavedMaterialThumbnailRequests();
    }
}

void AssetBrowser::PruneMeshThumbnailRequests(bool all)
{
    // A full prune (project, folder-root or thumbnail reset) also forgets admission state.
    if(all)
    {
        m_meshAdmission.Clear();
        m_materialAdmission.Clear();
        m_materialResidents.clear();
        m_thumbnailFailures.clear();
    }

    for(auto entry = m_meshThumbnailRequests.begin(); entry != m_meshThumbnailRequests.end();)
    {
        if(!all && entry->second.visible)
        {
            ++entry;
            continue;
        }

        if(m_materialRenderer && entry->second.ticket)
        {
            m_materialRenderer->DiscardMeshThumbnail(entry->second.ticket);
        }

        entry = m_meshThumbnailRequests.erase(entry);
    }
}

std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> AssetBrowser::GetVisibleMeshThumbnail(
    const AssetRecord& record, std::string& error, bool& terminal)
{
    terminal = false;

    if(!m_materialRenderer || !m_thumbnailPaths || !m_thumbnailRegistry)
    {
        return std::nullopt;
    }

    auto& diagnostics = MeshThumbnailDiagnostics::Get();
    MeshThumbnailDiagnostics::Add(diagnostics.visibleLookups);
    const auto key = record.id.ToReference();
    const auto now = std::chrono::steady_clock::now();
    const auto existing = m_meshThumbnailRequests.find(key);
    const bool validationDue = existing == m_meshThumbnailRequests.end() || now >= existing->second.nextValidation;

    // Source resolution touches the file system, so it runs with the periodic validation rather than
    // every frame. Between validations the renderer's own staleness poll cancels a changed source.
    if(validationDue && !IsMeshSourceAvailable(record.id))
    {
        error = "Mesh thumbnail source is missing or no longer registered.";
        terminal = true;

        if(existing != m_meshThumbnailRequests.end())
        {
            m_materialRenderer->DiscardMeshThumbnail(existing->second.ticket);
            m_meshThumbnailRequests.erase(existing);
        }

        return std::nullopt;
    }

    auto& request = m_meshThumbnailRequests[key];
    request.visible = true;

    if(validationDue)
    {
        if(request.ticket && !m_materialRenderer->IsMeshThumbnailRequestCurrent(request.ticket))
        {
            m_materialRenderer->DiscardMeshThumbnail(request.ticket);
            request.ticket = 0;
        }

        {
            MeshThumbnailDiagnostics::Timer requestTimer(diagnostics.browserRequestMicroseconds);
            MeshThumbnailDiagnostics::Add(diagnostics.browserRequests);
            m_materialRenderer->RequestMeshThumbnail(*m_thumbnailPaths, *m_thumbnailRegistry,
                record.id, request.ticket, request.error);
        }

        request.nextValidation = now + std::chrono::milliseconds(request.ticket ? 2000 : 250);
    }

    error = request.error.empty() && request.ticket ?
        m_materialRenderer->GetMeshThumbnailError(request.ticket) : request.error;

    // Preparation failure means the source itself is unreadable or unsupported.
    if(request.ticket && m_materialRenderer->HasMeshThumbnailPreparationFailed(request.ticket))
    {
        terminal = true;

        if(error.empty())
        {
            error = "Mesh preview could not be prepared from this source.";
        }
    }
    const auto image = m_materialRenderer->GetMeshThumbnailImage(request.ticket);
    return image ? std::optional<D3D12_GPU_DESCRIPTOR_HANDLE>(image->srv) : std::nullopt;
}

bool AssetBrowser::IsMeshSourceAvailable(const AssetID& id) const
{
    auto& diagnostics = MeshThumbnailDiagnostics::Get();
    MeshThumbnailDiagnostics::Timer resolveTimer(diagnostics.browserResolveMicroseconds);
    MeshThumbnailDiagnostics::Add(diagnostics.browserResolves);
    const auto* registered = m_thumbnailRegistry->Find(id);
    const auto resolved = ProjectAssetResolver::Resolve(*m_thumbnailPaths, *m_thumbnailRegistry, id);
    return registered && registered->type == AssetType::Mesh && resolved.status == AssetAvailability::Available;
}

std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> AssetBrowser::GetResidentMeshThumbnail(const AssetRecord& record)
{
    const auto key = record.id.ToReference();
    const auto found = m_meshThumbnailRequests.find(key);

    if(found == m_meshThumbnailRequests.end() || !found->second.resident || !m_materialRenderer ||
        !m_thumbnailPaths || !m_thumbnailRegistry)
    {
        return std::nullopt;
    }

    auto& request = found->second;
    request.visible = true;
    const auto now = std::chrono::steady_clock::now();
    bool keep = true;

    // Resident tiles keep the admitted tiles' validation cadence. Revalidation re-hashes the same
    // retained entry; anything that would need a new preparation leaves residency instead, so new
    // generation only ever starts from an admitted tile.
    if(now >= request.nextValidation)
    {
        request.nextValidation = now + std::chrono::milliseconds(2000);
        keep = m_materialRenderer->IsMeshThumbnailRequestCurrent(request.ticket) && IsMeshSourceAvailable(record.id);

        if(keep)
        {
            auto& diagnostics = MeshThumbnailDiagnostics::Get();
            MeshThumbnailDiagnostics::Timer requestTimer(diagnostics.browserRequestMicroseconds);
            MeshThumbnailDiagnostics::Add(diagnostics.browserRequests);
            std::uint64_t ticket = 0;
            std::string error;
            keep = m_materialRenderer->RequestMeshThumbnail(*m_thumbnailPaths, *m_thumbnailRegistry,
                record.id, ticket, error) && ticket == request.ticket;

            if(!keep && ticket != 0 && ticket != request.ticket)
            {
                m_materialRenderer->DiscardMeshThumbnail(ticket);
            }
        }
    }

    const auto image = keep ? m_materialRenderer->GetMeshThumbnailImage(request.ticket) : std::nullopt;

    if(!image)
    {
        // Evicted, invalidated or changed: the tile rejoins admission to regenerate.
        m_materialRenderer->DiscardMeshThumbnail(request.ticket);
        m_meshThumbnailRequests.erase(found);
        return std::nullopt;
    }

    return image->srv;
}

void AssetBrowser::ResetThumbnailRequests()
{
    if(m_thumbnails)
    {
        m_thumbnails->ReleaseConsumer(TextureThumbnailCache::Consumer::AssetBrowser, true);
    }

    m_thumbnailResetRequested = false;
}

void AssetBrowser::DrawDeletionConfirmation(bool protectedEdits)
{
    if(m_openDeleteModal)
    {
        ImGui::OpenPopup("Delete project file");
        m_openDeleteModal = false;
    }

    if(ImGui::BeginPopupModal("Delete project file", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("Delete '%s' permanently?", DisplayName(m_deleteTarget).c_str());
        ImGui::TextUnformatted("This will remove the file from disk. This action cannot be undone.");

        if(protectedEdits)
        {
            ImGui::TextWrapped("Save or discard pending Inspector edits before deleting files.");
        }

        ImGui::BeginDisabled(protectedEdits);

        if(ImGui::Button("Delete"))
        {
            // The file may have changed while the confirmation was open.
            std::string error;

            if(CanRequestFileOperation(m_deleteTarget, error))
            {
                m_deleteRequest = m_deleteTarget;
            }
            else
            {
                m_error = error;
                m_listings.erase(m_deleteTarget.parent_path());
                m_activeSceneIdentity.reset();
            }

            m_deleteTarget.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndDisabled();
        ImGui::SameLine();

        if(ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            m_deleteTarget.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

std::optional<std::filesystem::path> AssetBrowser::TakeDeletionRequest()
{
    return std::exchange(m_deleteRequest, std::nullopt);
}

void AssetBrowser::OnDeletionCompleted(const std::filesystem::path& path, const std::string& error)
{
    m_error = error;
    // Also refresh on failure: rollback errors must not leave a stale usable tile.
    m_listings.erase(path.parent_path());

    if(error.empty())
    {
        m_thumbnails->Invalidate(path);
        m_textureRecords.erase(path.lexically_normal());
        m_materialRecords.erase(path.lexically_normal());
        m_materialImagesInvalidated = true;
        m_materialRequestsReset = true;

        if(m_selection == path)
        {
            m_selection.clear();
        }
    }
}

void AssetBrowser::DrawRenameModal(bool protectedEdits)
{
    if(m_openRenameModal)
    {
        ImGui::OpenPopup("Rename project file");
        m_openRenameModal = false;
    }

    if(ImGui::BeginPopupModal("Rename project file", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if(m_closeRenameModal)
        {
            m_closeRenameModal = false;
            m_renameTarget.clear();
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        ImGui::TextWrapped("Rename '%s'", DisplayName(m_renameTarget).c_str());

        if(m_focusRename)
        {
            ImGui::SetKeyboardFocusHere();
            m_focusRename = false;
        }

        const bool enter = ImGui::InputText("Filename stem", m_renameStem.data(), m_renameStem.size(),
            ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Text("Extension (preserved): %s", DisplayName(m_renameTarget.extension()).c_str());

        if(!m_renameError.empty())
        {
            ImGui::TextWrapped("%s", m_renameError.c_str());
        }

        if(protectedEdits)
        {
            ImGui::TextWrapped("Save or discard pending Inspector edits before renaming files.");
        }

        const bool blocked = protectedEdits || m_renamePending;
        ImGui::BeginDisabled(blocked);
        const bool confirm = ImGui::Button("Rename");
        ImGui::EndDisabled();

        if(!blocked && (confirm || enter))
        {
            m_renameRequest = RenameRequest{m_renameTarget, m_renameStem.data()};
            m_renamePending = true;
        }

        ImGui::SameLine();

        if(!m_renamePending && (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        {
            m_renameTarget.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

std::optional<AssetBrowser::RenameRequest> AssetBrowser::TakeRenameRequest()
{
    return std::exchange(m_renameRequest, std::nullopt);
}

void AssetBrowser::OnRenameCompleted(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const std::string& error)
{
    m_renamePending = false;
    m_renameError = error;
    m_listings.erase(source.parent_path());

    if(error.empty())
    {
        m_closeRenameModal = true;
        m_materialImagesInvalidated = true;
        m_materialRequestsReset = true;

        if(source != destination)
        {
            m_thumbnails->Invalidate(source);
            m_textureRecords.erase(source.lexically_normal());
        }

        if(m_selection == source)
        {
            m_selection = destination;
        }
    }
}

void AssetBrowser::InvalidateMeshDragIdentity() noexcept
{
    static std::atomic<std::uint64_t> nextIdentity{1};
    m_meshDragIdentity = nextIdentity.fetch_add(1, std::memory_order_relaxed);
}

void AssetBrowser::DrawAssetMoveSource(const std::filesystem::path& path, const std::string& name)
{
    if(!m_thumbnailRegistry || !m_thumbnailPaths || !m_meshDragIdentity)
    {
        return;
    }
    const auto* record = m_thumbnailRegistry->FindByPath(path.lexically_relative(m_thumbnailPaths->GetRoot()));
    if(!record || record->type == AssetType::Script || record->type == AssetType::AudioBank ||
        !ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers))
    {
        return;
    }
    const auto* current = ImGui::GetDragDropPayload();
    if(!current || current->DataSize == 0)
    {
        m_moveSource = path;
    }
    MovePayload payload;
    payload.projectIdentity = m_meshDragIdentity;
    const auto reference = record->id.ToReference();
    std::copy(reference.begin(), reference.end(), payload.reference.begin());
    if(record->type == AssetType::Material)
    {
        const auto material = MakeMaterialDragPayload(record->id);
        if(material)
            ImGui::SetDragDropPayload(MaterialDragPayloadType, &*material, sizeof(*material), ImGuiCond_Once);
        ImGui::Text("Material: %s", name.c_str());
        ImGui::TextUnformatted("Drop onto a viewport object to replace all material slots");
    }
    else
    {
        ImGui::SetDragDropPayload("ENGINE_ASSET_MOVE", &payload, sizeof(payload), ImGuiCond_Once);
        ImGui::Text("Move: %s", name.c_str());
    }
    ImGui::TextUnformatted("Drop onto an Asset Browser folder");
    ImGui::EndDragDropSource();
}

void AssetBrowser::DrawAssetMoveTarget(const std::filesystem::path& directory)
{
    const auto* payload = ImGui::GetDragDropPayload();
    if(!payload || !m_thumbnailPaths || !m_thumbnailRegistry ||
        (!payload->IsDataType("ENGINE_ASSET_MOVE") && !payload->IsDataType(MeshDragPayloadType) &&
            !payload->IsDataType(MaterialDragPayloadType)))
    {
        return;
    }
    if(!ImGui::BeginDragDropTarget())
    {
        return;
    }

    std::string error;
    std::optional<AssetRecord> asset;
    if(payload->IsDataType(MeshDragPayloadType) && payload->DataSize == sizeof(MeshDragPayload))
    {
        MeshDragPayload mesh;
        memcpy(&mesh, payload->Data, sizeof(mesh));
        asset = ValidateMeshDrag(mesh, *m_thumbnailPaths, *m_thumbnailRegistry, error);
    }
    else if(payload->IsDataType(MaterialDragPayloadType) && payload->DataSize == sizeof(MaterialDragPayload))
    {
        MaterialDragPayload material;
        memcpy(&material, payload->Data, sizeof(material));
        asset = ValidateMaterialDrag(material, *m_thumbnailPaths, *m_thumbnailRegistry, error);
    }
    else if(payload->IsDataType("ENGINE_ASSET_MOVE") && payload->DataSize == sizeof(MovePayload))
    {
        MovePayload move;
        memcpy(&move, payload->Data, sizeof(move));
        const auto id = move.reference.back() == '\0' ?
            AssetID::ParseReference(std::string_view(move.reference.data(), move.reference.size() - 1)) : std::nullopt;
        const auto* record = id ? m_thumbnailRegistry->Find(*id) : nullptr;
        if(record && move.projectIdentity != 0 && move.projectIdentity == m_meshDragIdentity)
        {
            asset = *record;
        }
    }

    const auto& paths = *m_thumbnailPaths;
    if(!asset || m_moveSource.empty() || !ProjectAssetRegistry::PathsEqual(
        paths.GetRoot() / asset->relativePath, m_moveSource))
    {
        error = "Asset drag is stale. Start a new drag.";
    }
    else if(!CanBrowse(directory, error) || !CanBrowse(m_moveSource.parent_path(), error))
    {
        // CanBrowse supplies the containment or directory-link error.
    }
    else if(ProjectAssetRegistry::PathsEqual(m_moveSource.parent_path(), directory))
    {
        error = "Asset is already in this directory.";
    }
    else
    {
        const auto destination = directory / m_moveSource.filename();
        const auto relative = destination.lexically_relative(paths.GetRoot());
        const DWORD attributes = GetFileAttributesW(m_moveSource.c_str());
        std::error_code ec;
        if(!ValidateAssetRecord(AssetRecord{asset->id, asset->type, relative}))
        {
            error = "Materials must remain in Materials; meshes must remain in Meshes.";
        }
        else if(destination.native().size() >= MAX_PATH)
        {
            error = "Destination exceeds the supported Windows path length.";
        }
        else if(attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        {
            error = "Source is missing, read-only, or linked.";
        }
        else if(m_thumbnailRegistry->FindByPath(relative) || std::filesystem::exists(destination, ec) || ec)
        {
            error = "Destination already exists, is registered, or cannot be inspected.";
        }
    }

    if(error.empty())
    {
        HANDLE writable = CreateFileW(directory.c_str(), FILE_ADD_FILE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if(writable == INVALID_HANDLE_VALUE)
        {
            error = "Destination is not writable: " + std::system_category().message(GetLastError());
        }
        else
        {
            CloseHandle(writable);
        }
    }
    if(error.empty() && asset)
    {
        if(const auto* accepted = ImGui::AcceptDragDropPayload(payload->DataType))
        {
            if(accepted->IsDelivery())
            {
                m_moveRequest = MoveRequest{*asset, m_moveSource, directory};
            }
        }
        ImGui::SetTooltip("Move to %s", PathID(directory).c_str());
    }
    else
    {
        ImGui::SetTooltip("Cannot move: %s", error.c_str());
        if(ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            m_error = error;
        }
    }
    ImGui::EndDragDropTarget();
}

std::optional<AssetBrowser::MoveRequest> AssetBrowser::TakeMoveRequest()
{
    return std::exchange(m_moveRequest, std::nullopt);
}

void AssetBrowser::OnMoveCompleted(const MoveRequest& request, const std::filesystem::path& destination,
    const std::string& error)
{
    m_error = error;
    if(!error.empty())
    {
        return;
    }
    m_listings.erase(request.source.parent_path());
    m_listings.erase(request.directory);
    m_thumbnails->Invalidate(request.source);
    InvalidateMaterialSource(request.source);
    m_materialRequestsReset = true;
    if(m_selection == request.source)
    {
        m_selection = destination;
    }
    SetThumbnailProject(*m_thumbnailPaths, *m_thumbnailRegistry);
}

std::optional<MaterialDragPayload> AssetBrowser::MakeMaterialDragPayload(const AssetID& id) const
{
    const auto* record = m_thumbnailRegistry ? m_thumbnailRegistry->Find(id) : nullptr;
    if(!record || record->type != AssetType::Material || !m_meshDragIdentity || m_meshDragProject.empty())
        return std::nullopt;
    MaterialDragPayload payload;
    payload.projectIdentity = m_meshDragIdentity;
    const auto reference = id.ToReference();
    std::copy(reference.begin(), reference.end(), payload.materialReference.begin());
    return payload;
}

std::optional<AssetRecord> AssetBrowser::ValidateMaterialDrag(const MaterialDragPayload& payload,
    const ProjectPaths& paths, const ProjectAssetRegistry& registry, std::string& error) const
{
    error.clear();
    std::error_code pathError;
    const auto project = std::filesystem::canonical(paths.GetRoot(), pathError);
    if(payload.version != 1 || payload.reserved || !payload.projectIdentity ||
        payload.projectIdentity != m_meshDragIdentity || pathError || project != m_meshDragProject)
    {
        error = "Material drag belongs to a stale project or scene. Start a new drag.";
        return std::nullopt;
    }
    const auto id = payload.materialReference.back() == '\0' ? AssetID::ParseReference(
        std::string_view(payload.materialReference.data(), payload.materialReference.size() - 1)) : std::nullopt;
    const auto* record = id ? registry.Find(*id) : nullptr;
    if(!record || record->type != AssetType::Material ||
        ProjectAssetResolver::Resolve(paths, registry, *id).status != AssetAvailability::Available)
    {
        error = "Dragged material is no longer an available registered material.";
        return std::nullopt;
    }
    return *record;
}

void AssetBrowser::DrawMeshDragSource(const std::filesystem::path& path, const std::string& name)
{
    const auto found = m_meshRecords.find(path.lexically_normal());

    if(found == m_meshRecords.end() || m_meshDragProject.empty() || m_meshDragIdentity == 0)
    {
        return;
    }

    if(!ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers))
    {
        return;
    }

    MeshDragPayload payload;
    if(!ImGui::GetDragDropPayload() || ImGui::GetDragDropPayload()->DataSize == 0)
    {
        m_moveSource = path;
    }
    payload.projectIdentity = m_meshDragIdentity;
    const auto reference = found->second.id.ToReference();
    std::copy(reference.begin(), reference.end(), payload.meshReference.begin());
    ImGui::SetDragDropPayload(MeshDragPayloadType, &payload, sizeof(payload), ImGuiCond_Once);
    ImGui::Text("Mesh: %s", name.c_str());
    ImGui::TextUnformatted("Drop into Scene Viewport to place at cursor");
    ImGui::TextUnformatted("Or drop onto an Asset Browser folder to move");
    ImGui::EndDragDropSource();
}

std::optional<AssetRecord> AssetBrowser::ValidateMeshDrag(const MeshDragPayload& payload,
    const ProjectPaths& paths, const ProjectAssetRegistry& registry, std::string& error) const
{
    error.clear();
    std::error_code pathError;
    const auto project = std::filesystem::canonical(paths.GetRoot(), pathError);

    if(payload.version != 1 || payload.reserved != 0 || payload.projectIdentity == 0 ||
        payload.projectIdentity != m_meshDragIdentity || pathError || project != m_meshDragProject)
    {
        error = "Mesh drag is stale or belongs to another project. Start a new drag.";
        return std::nullopt;
    }

    if(payload.meshReference.back() != '\0')
    {
        error = "Mesh drag has an invalid reference format.";
        return std::nullopt;
    }

    const std::string_view reference(payload.meshReference.data(), payload.meshReference.size() - 1);
    const auto id = AssetID::ParseReference(reference);
    const auto* record = id ? registry.Find(*id) : nullptr;

    if(!record || record->type != AssetType::Mesh)
    {
        error = "Dragged asset is no longer registered as a mesh.";
        return std::nullopt;
    }

    const auto resolved = ProjectAssetResolver::Resolve(paths, registry, *id);

    if(resolved.status != AssetAvailability::Available ||
        _wcsicmp(resolved.resolvedPath.extension().c_str(), L".fbx") != 0)
    {
        error = "Dragged mesh source is missing, inaccessible, or is not a project FBX. Restore it before dragging.";
        return std::nullopt;
    }

    return *record;
}

void AssetBrowser::ConfigureMaterialThumbnails(Renderer& renderer, const ProjectPaths& projectPaths)
{
    if(m_root.empty())
    {
        m_root=projectPaths.GetRoot().lexically_normal();
        m_directory=m_root;
    }
    m_materialRenderer = &renderer;
    m_thumbnails = &renderer.GetTextureThumbnailCache();
}
