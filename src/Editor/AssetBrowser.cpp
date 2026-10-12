#include "AssetBrowser.h"

#include "Project/ProjectLocator.h"
#include "Project/ProjectPaths.h"
#include "imgui.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <utility>

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

    bool MaterialFilename(
        const char* input,
        std::filesystem::path& filename,
        std::string& error)
    {
        const std::string name(input);

        if (name.empty() || name.find("..") != std::string::npos ||
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

        if (filename.has_extension() && filename.extension() != L".material")
        {
            error = "The filename extension must be .material.";
            return false;
        }

        if (!filename.has_extension())
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
}

AssetBrowser::AssetBrowser()
    : m_root(ProjectLocator::GetDefaultProjectRoot().lexically_normal()),
      m_directory(m_root)
{
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

void AssetBrowser::DrawMaterialCreationModal()
{
    if (m_openMaterialModal)
    {
        ImGui::OpenPopup("Create Material");
        m_openMaterialModal = false;
    }

    if (ImGui::BeginPopupModal("Create Material", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
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

                if (MaterialFilename(m_materialName.data(), filename, m_materialError) &&
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

bool AssetBrowser::CanBrowse(
    const std::filesystem::path& path,
    std::string& error) const
{
    const auto relative = path.lexically_relative(m_root);

    if (relative.empty() || *relative.begin() == "..")
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

const AssetBrowser::Listing& AssetBrowser::GetListing(const std::filesystem::path& path)
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
            listing.entries.push_back({entryPath, DisplayName(entryPath), directory,
                directory && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0});
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

void AssetBrowser::Navigate(const std::filesystem::path& path)
{
    m_error.clear();

    if (CanBrowse(path, m_error))
    {
        m_directory = path;
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

    const bool open = ImGui::TreeNodeEx("##Directory", flags, "%s", label.c_str());

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

void AssetBrowser::DrawContents()
{
    const auto& listing = GetListing(m_directory);

    if (!listing.error.empty())
    {
        ImGui::TextWrapped("%s", listing.error.c_str());
    }

    const int columns = (std::max)(1, static_cast<int>(ImGui::GetContentRegionAvail().x / 112.0f));
    std::filesystem::path destination;
    bool tileHovered = false;

    if (ImGui::BeginTable("Files", columns, ImGuiTableFlags_SizingStretchSame))
    {
        for (const auto& entry : listing.entries)
        {
            ImGui::TableNextColumn();
            ImGui::PushID(PathID(entry.path).c_str());
            // Submit the icon and filename in normal layout order. The group
            // includes both items in the tile's hover and click bounds.
            ImGui::BeginGroup();
            const bool pressed = ImGui::Selectable(entry.directory ? "[Folder]" : "[File]",
                m_selection == entry.path, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, 48.0f));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(entry.name.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            const bool hovered = ImGui::IsItemHovered();
            tileHovered = tileHovered || hovered;

            if (pressed || (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))
            {
                m_selection = entry.path;

                if (entry.directory && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    destination = entry.path;
                }
            }

            if (hovered)
            {
                ImGui::SetTooltip("%s%s", entry.name.c_str(),
                    entry.directory && !entry.navigable ? " (directory link; browsing disabled)" : "");
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    if (listing.entries.empty() && listing.error.empty())
    {
        ImGui::TextDisabled("This directory is empty.");
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

        if (CanCreateMaterial(error) && ImGui::BeginMenu("Create", !m_materialCreationPending))
        {
            if (ImGui::MenuItem("Material"))
            {
                m_materialDirectory = m_directory;
                std::strcpy(m_materialName.data(), "NewMaterial");
                m_materialError.clear();
                m_openMaterialModal = true;
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

void AssetBrowser::Draw(bool& visible)
{
    if (!visible)
    {
        return;
    }

    const auto* viewport = ImGui::GetMainViewport();

    if (viewport->WorkSize.x <= 0.0f || viewport->WorkSize.y <= 0.0f)
    {
        return;
    }

    // Reserve at least 40% of the work area (and 180 pixels when possible)
    // for the scene. Reduce the minimum panel height for very small windows.
    const float maxHeight = (std::max)(1.0f,
        (std::min)(viewport->WorkSize.y * 0.6f, viewport->WorkSize.y - 180.0f));
    const float minHeight = (std::min)(140.0f, maxHeight);
    m_panelHeight = (std::clamp)(m_panelHeight, minHeight, maxHeight);
    const float splitterHeight = (std::min)(6.0f, m_panelHeight);

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x,
        viewport->WorkPos.y + viewport->WorkSize.y - m_panelHeight), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, splitterHeight), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    const ImGuiWindowFlags fixedFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("##AssetBrowserSplitter", nullptr, fixedFlags | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav))
    {
        ImGui::InvisibleButton("Height", ImVec2(viewport->WorkSize.x, splitterHeight));
        const bool active = ImGui::IsItemActive();
        const bool hovered = ImGui::IsItemHovered();

        if (active || hovered)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }

        const ImU32 color = ImGui::GetColorU32(active ? ImGuiCol_SeparatorActive :
            hovered ? ImGuiCol_SeparatorHovered : ImGuiCol_Separator);
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), color);

        if (active)
        {
            m_panelHeight = (std::clamp)(m_panelHeight - ImGui::GetIO().MouseDelta.y, minHeight, maxHeight);
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(3);

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x,
        viewport->WorkPos.y + viewport->WorkSize.y - m_panelHeight + splitterHeight), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x,
        (std::max)(1.0f, m_panelHeight - splitterHeight)), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

    if (ImGui::Begin("Asset Browser", &visible, fixedFlags))
    {
        if (ImGui::Button("Refresh"))
        {
            m_listings.clear();
            m_selection.clear();
            m_error.clear();
        }

        ImGui::SameLine();

        if (ImGui::SmallButton("Assets"))
        {
            Navigate(m_root);
        }

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
            DrawContents();
            ImGui::EndChild();
            ImGui::EndTable();
        }

        DrawMaterialCreationModal();
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}
