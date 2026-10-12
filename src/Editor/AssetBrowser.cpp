#include "AssetBrowser.h"
#include "EditorWorkspace.h"

#include "Project/ProjectLocator.h"
#include "Project/ProjectPaths.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "assets/ProjectTextureImportService.h"
#include "Core/Log.h"

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
        m_thumbnailResetRequested = true;
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
    CaptureDropTarget(path);

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
            const auto registered = m_textureRecords.find(entry.path.lexically_normal());
            const bool isTexture = !entry.directory && registered != m_textureRecords.end();
            const auto tilePosition = ImGui::GetCursorScreenPos();
            constexpr float thumbnailSize = 96.0f;
            const bool pressed = ImGui::Selectable(isTexture ? "##Texture" : entry.directory ? "[Folder]" : "[File]",
                m_selection == entry.path, ImGuiSelectableFlags_AllowDoubleClick,
                isTexture ? ImVec2(thumbnailSize, thumbnailSize) : ImVec2(0.0f, 48.0f));
            const TextureThumbnailCache::Preview* preview = nullptr;

            if(isTexture && ImGui::IsItemVisible())
            {
                if(!m_thumbnailResetRequested && m_thumbnailPaths && m_thumbnailRegistry)
                {
                    preview = m_thumbnails.FindOrCreate(*m_thumbnailPaths, *m_thumbnailRegistry, registered->second);
                }

                auto* draw = ImGui::GetWindowDrawList();
                const ImVec2 start(tilePosition.x + 3.0f, tilePosition.y + 3.0f);
                const ImVec2 end(tilePosition.x + thumbnailSize - 3.0f, tilePosition.y + thumbnailSize - 3.0f);
                draw->AddRectFilled(start, end, IM_COL32(55, 55, 55, 255));

                if(preview && preview->texture.GetResource())
                {
                    const float scale = (thumbnailSize - 6.0f) / static_cast<float>((std::max)(preview->width, preview->height));
                    const ImVec2 size(preview->width * scale, preview->height * scale);
                    const ImVec2 position(tilePosition.x + (thumbnailSize - size.x) * 0.5f,
                        tilePosition.y + (thumbnailSize - size.y) * 0.5f);
                    draw->AddImage(static_cast<ImTextureID>(m_thumbnails.GetHandle(*preview).ptr), position,
                        ImVec2(position.x + size.x, position.y + size.y));
                }
                else
                {
                    draw->AddText(ImVec2(start.x + 5.0f, start.y + 5.0f), IM_COL32(190, 190, 190, 255), "Texture");
                }
            }
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(entry.name.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            CaptureDropTarget(entry.navigable ? entry.path : std::filesystem::path{});
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
                ImGui::SetTooltip("%s%s%s", entry.name.c_str(),
                    entry.directory && !entry.navigable ? " (directory link; browsing disabled)" : "",
                    preview && !preview->error.empty() ? ("\n" + preview->error).c_str() : "");
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

void AssetBrowser::Draw(const EditorWorkspaceRect& rectangle)
{
    m_thumbnails.BeginFrame();
    m_dropTargets.clear();
    m_dropFrame = ImGui::GetFrameCount();
    if(rectangle.size.x <= 0.0f || rectangle.size.y <= 0.0f)
    {
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
            m_listings.clear();
            m_thumbnailResetRequested = true;
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
            DrawContents();
            ImGui::EndChild();
            ImGui::EndTable();
        }

        DrawMaterialCreationModal();
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
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
        m_importFeedback = "Drop rejected: use a visible texture folder or empty Asset Browser content area.";
        Log::Info(m_importFeedback);
        return;
    }

    size_t successes = 0;
    std::string failures;
    std::string renamed;

    for(const auto& source : files)
    {
        std::filesystem::path imported;
        std::string error;

        if(ProjectTextureImportService::Import(paths, registry, source, target->directory, imported, error))
        {
            ++successes;

            if(imported.filename() != source.filename())
            {
                renamed += "\nCollision resolved: " + source.filename().string() + " -> " + imported.filename().string();
            }

            Log::Info("Imported texture: " + imported.string());
        }
        else
        {
            failures += "\n" + source.filename().string() + ": " + error;
            Log::Error("Texture import failed: " + source.string() + ": " + error);
        }
    }

    m_importFeedback = "Imported " + std::to_string(successes) + " of " + std::to_string(files.size()) + " textures." + renamed + failures;

    if(successes != 0)
    {
        // Refresh without clearing folder navigation, selection, or Inspector edits.
        m_listings.clear();
    }
}

void AssetBrowser::SetThumbnailProject(const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    if(!m_thumbnailProjectRoot.empty() && m_thumbnailProjectRoot != paths.GetRoot())
    {
        m_thumbnailResetRequested = true;
    }

    m_thumbnailProjectRoot = paths.GetRoot();
    m_thumbnailPaths = paths;
    m_thumbnailRegistry = &registry;
    m_textureRecords.clear();

    for(const auto& record : registry.GetAssets(AssetType::Texture))
    {
        m_textureRecords.emplace((paths.GetRoot() / record.relativePath).lexically_normal(), record);
    }
}

void AssetBrowser::ClearThumbnailsAfterGPUDrain()
{
    m_thumbnails.ClearAfterGPUDrain();
    m_thumbnailResetRequested = false;
}
