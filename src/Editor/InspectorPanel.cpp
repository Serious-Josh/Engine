#include "InspectorPanel.h"
#include "EditorWorkspace.h"
#include "assets/ProjectAssetResolver.h"
#include "assets/MaterialAssetSerializer.h"

#include <utility>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <limits>
#include <exception>
#include <cmath>
#include <cctype>

namespace
{
    const char* TextureAvailabilityReason(const AssetRecord& record, AssetAvailability availability)
    {
        if(record.type != AssetType::Texture)
        {
            return "Registry type mismatch: expected Texture.";
        }

        if(!ValidateAssetRecord(record))
        {
            return "Invalid registry record or project-relative path.";
        }

        switch(availability)
        {
        case AssetAvailability::Available: return "Available";
        case AssetAvailability::Missing: return "Missing file at the registered path.";
        case AssetAvailability::InvalidPath: return "Unsafe path, project escape, or non-file destination.";
        case AssetAvailability::Unreadable: return "File or project path is inaccessible.";
        case AssetAvailability::NotRegistered: return "Asset is no longer registered.";
        default: return "Unknown availability.";
        }
    }

    bool ReadMaterialBytes(const std::filesystem::path& path, std::string& bytes)
    {
        std::ifstream file(path, std::ios::binary);

        if(!file)
        {
            return false;
        }

        bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return !file.bad();
    }

    bool EqualMaterials(const MaterialAssetData& a, const MaterialAssetData& b)
    {
        const auto sameFloat = [](float left, float right)
        {
            // A stable invalid draft must not be mistaken for a new edit every frame.
            return left == right || (std::isnan(left) && std::isnan(right));
        };
        const auto sameVector = [&sameFloat](const auto& left, const auto& right)
        {
            return std::equal(left.begin(), left.end(), right.begin(), sameFloat);
        };

        const bool surface = a.model == b.model && sameVector(a.baseColor, b.baseColor) &&
            sameFloat(a.roughness, b.roughness) && sameFloat(a.metallic, b.metallic);
        const bool optical = sameFloat(a.ior, b.ior) && sameVector(a.conductorN, b.conductorN) &&
            sameVector(a.conductorK, b.conductorK);
        const bool detail = a.surfaceDetailMode == b.surfaceDetailMode && sameFloat(a.heightScale, b.heightScale) &&
            sameFloat(a.heightBias, b.heightBias) && a.preferARM == b.preferARM;

        const bool textures = a.baseColorTexture == b.baseColorTexture && a.normalTexture == b.normalTexture &&
            a.roughnessTexture == b.roughnessTexture && a.metallicTexture == b.metallicTexture;
        const bool packedTextures = a.armTexture == b.armTexture && a.aoTexture == b.aoTexture &&
            a.heightTexture == b.heightTexture;

        return surface && optical && detail && textures && packedTextures;
    }
}

void InspectorPanel::Draw(
    const EditorWorkspaceRect& rectangle,
    const ProjectPaths& paths,
    const ProjectAssetRegistry& registry,
    bool protectedAction)
{
    if(rectangle.size.x <= 0.0f || rectangle.size.y <= 0.0f)
    {
        return;
    }

    ImGui::SetNextWindowPos(rectangle.position, ImGuiCond_Always);
    ImGui::SetNextWindowSize(rectangle.size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing;

    // ImGui otherwise enforces title-bar height even when the assigned panel is smaller.
    if(rectangle.size.y < ImGui::GetFrameHeight())
    {
        flags |= ImGuiWindowFlags_NoTitleBar;
    }

    if(ImGui::Begin("Inspector", nullptr, flags))
    {
        EditorWorkspace::KeepCurrentWindowBehindFloatingUI();
        if(ImGui::Button("Refresh"))
        {
            m_refreshRequested = true;
        }

        if(m_record)
        {
            const auto name = m_record->relativePath.stem().string();
            const auto path = m_record->relativePath.generic_string();
            ImGui::TextWrapped("Material: %s", name.c_str());
            ImGui::TextWrapped("Path: %s", path.c_str());
            ImGui::TextWrapped("AssetID: %s", m_record->id.ToString().c_str());
        }

        if(m_material)
        {
            const MaterialAssetData before = *m_material;
            DrawMaterial(paths, registry);

            if(!EqualMaterials(before, *m_material))
            {
                m_lastPropertyChangeTime = ImGui::GetTime();
                m_saveScheduled = HasUnsavedChanges() && !m_externalConflict;
                m_persistenceState = m_externalConflict ? PersistenceState::Failed : PersistenceState::Waiting;
                if(!m_externalConflict)
                {
                    m_status.clear();
                }
            }

            if(!HasUnsavedChanges())
            {
                m_saveScheduled = false;
                m_persistenceState = m_externalConflict ? PersistenceState::Failed : PersistenceState::Saved;
            }

            // Do not write during mouse dragging. Text focus alone must not prevent an idle save.
            const bool settled = ImGui::GetTime() - m_lastPropertyChangeTime >= 0.4;

            if(m_saveScheduled && settled && !protectedAction && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                Save(paths, registry);
            }

            switch(m_persistenceState)
            {
            case PersistenceState::Saved:
                ImGui::TextUnformatted("Saved");
                break;
            case PersistenceState::Waiting:
                ImGui::TextUnformatted("Unsaved changes");
                break;
            case PersistenceState::Saving:
                ImGui::TextUnformatted("Saving...");
                break;
            case PersistenceState::Failed:
                ImGui::TextUnformatted("Save failed - working values retained");

                if(ImGui::Button("Retry"))
                {
                    Save(paths, registry);
                }

                break;
            }

            ImGui::TextWrapped("%s", m_status.c_str());
        }
        else
        {
            ImGui::TextWrapped("%s", m_status.c_str());
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);

    if(m_showConflictConfirmation && !protectedAction && !ImGui::IsPopupOpen("External material changes"))
    {
        ImGui::OpenPopup("External material changes");
    }

    bool conflictOpen = true;

    if(ImGui::BeginPopupModal("External material changes", &conflictOpen, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("%s", m_status.c_str());

        if(ImGui::Button("Overwrite external changes") && Save(paths, registry, true))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if(ImGui::Button("Cancel"))
        {
            m_showConflictConfirmation = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if(!conflictOpen)
    {
        m_showConflictConfirmation = false;
    }
}

void InspectorPanel::LoadSelection(
    const std::filesystem::path& selection,
    const ProjectPaths& paths,
    const ProjectAssetRegistry& registry)
{
    ++m_texturePickerGeneration;
    m_textureSearch.fill(0);
    m_texturePickerRegistry.clear();
    m_availableTextures.clear();
    m_refreshTextureChoices = true;
    m_projectRoot = paths.GetRoot();
    m_savedMaterial.reset();
    m_savedBytes.clear();
    m_externalConflict = false;
    m_showConflictConfirmation = false;
    m_saveScheduled = false;
    m_leaveFlushAttempted = false;
    m_persistenceState = PersistenceState::Saved;
    m_record.reset();
    m_material.reset();
    m_status = "No asset or scene object selected.";

    if(selection.empty())
    {
        return;
    }

    if(selection.extension() != ".material")
    {
        m_status = "Select a registered .material asset to inspect its serialized properties.";
        return;
    }

    std::error_code error;

    if(std::filesystem::is_directory(selection, error))
    {
        m_status = "Directories cannot be inspected as materials.";
        return;
    }

    const auto relative = selection.lexically_relative(paths.GetRoot()).lexically_normal();

    for(const auto& record : registry.GetAssets(AssetType::Material))
    {
        if(record.relativePath.lexically_normal() == relative)
        {
            m_record = record;
            break;
        }
    }

    if(!m_record)
    {
        m_status = "This .material file is not registered in the live project asset registry.";
        return;
    }

    const auto resolution = ProjectAssetResolver::Resolve(paths, registry, m_record->id);

    if(resolution.status != AssetAvailability::Available)
    {
        switch(resolution.status)
        {
        case AssetAvailability::Missing: m_status = "The registered material file is missing."; break;
        case AssetAvailability::InvalidPath: m_status = "The registered material path is invalid or outside its project category."; break;
        case AssetAvailability::Unreadable: m_status = "The registered material path is inaccessible."; break;
        default: m_status = "The material is no longer registered."; break;
        }

        return;
    }

    std::string before;

    if(!ReadMaterialBytes(resolution.resolvedPath, before))
    {
        m_status = "Cannot read the material file.";
        return;
    }

    MaterialAssetData material;

    if(!MaterialAssetSerializer::LoadFromFile(resolution.resolvedPath, material, &m_status))
    {
        return;
    }

    std::string after;

    if(!ReadMaterialBytes(resolution.resolvedPath, after) || before != after)
    {
        m_status = "Material changed during loading. Refresh to retry.";
        return;
    }

    m_savedBytes = std::move(after);
    m_savedMaterial = material;
    m_material = std::move(material);
    m_status.clear();
}

void InspectorPanel::DrawMaterial(
    const ProjectPaths& paths,
    const ProjectAssetRegistry& registry)
{
    auto& material = *m_material;
    int model = static_cast<int>(material.model);
    int detail = static_cast<int>(material.surfaceDetailMode);

    ImGui::SeparatorText("Surface");

    if(ImGui::Combo("Model", &model, "MetallicRoughness\0Dielectric\0Conductor\0"))
    {
        material.model = static_cast<MaterialModel>(model);
    }

    ImGui::ColorEdit3("Base color", material.baseColor.data());
    ImGui::SliderFloat("Roughness", &material.roughness, 0.0f, 1.0f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderFloat("Metallic", &material.metallic, 0.0f, 1.0f, "%.4f", ImGuiSliderFlags_AlwaysClamp);

    ImGui::SeparatorText("Optical properties (all serialized values)");

    if(ImGui::InputFloat("IOR (> 0)", &material.ior))
    {
        material.ior = (std::max)(material.ior, (std::numeric_limits<float>::min)());
    }

    if(ImGui::InputFloat3("Conductor N (>= 0)", material.conductorN.data()))
    {
        for(auto& value : material.conductorN)
        {
            value = (std::max)(value, 0.0f);
        }
    }

    if(ImGui::InputFloat3("Conductor K (>= 0)", material.conductorK.data()))
    {
        for(auto& value : material.conductorK)
        {
            value = (std::max)(value, 0.0f);
        }
    }

    ImGui::TextWrapped("IOR applies to dielectric shading; N/K apply to conductor shading. All values are preserved when changing models.");

    ImGui::SeparatorText("Surface detail");

    if(ImGui::Combo("Mode", &detail, "NormalOnly\0ParallaxOcclusion\0GeometryWithParallaxFallback\0"))
    {
        material.surfaceDetailMode = static_cast<SurfaceDetailMode>(detail);
    }

    ImGui::InputFloat("Height scale", &material.heightScale);
    ImGui::InputFloat("Height bias", &material.heightBias);
    ImGui::Checkbox("Prefer ARM", &material.preferARM);

    ImGui::SeparatorText("Texture references");
    // Do not let a popup edit a draft whose project or material registry identity changed.
    const AssetRecord* current = m_record ? registry.Find(m_record->id) : nullptr;
    const bool selectionValid = current && current->type == AssetType::Material &&
        current->relativePath == m_record->relativePath && paths.GetRoot() == m_projectRoot;
    const auto rootID = m_projectRoot.generic_string();
    ImGui::PushID(rootID.c_str());
    ImGui::PushID(m_record->id.ToString().c_str());
    ImGui::PushID(static_cast<int>(m_texturePickerGeneration));

    DrawTextureReference("Base color", material.baseColorTexture, paths, registry, selectionValid);
    DrawTextureReference("Normal", material.normalTexture, paths, registry, selectionValid);
    DrawTextureReference("Roughness", material.roughnessTexture, paths, registry, selectionValid);
    DrawTextureReference("Metallic", material.metallicTexture, paths, registry, selectionValid);
    DrawTextureReference("ARM", material.armTexture, paths, registry, selectionValid);
    DrawTextureReference("AO", material.aoTexture, paths, registry, selectionValid);
    DrawTextureReference("Height", material.heightTexture, paths, registry, selectionValid);

    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
}

void InspectorPanel::DrawTextureReference(
    const char* label,
    std::optional<std::string>& reference,
    const ProjectPaths& paths,
    const ProjectAssetRegistry& registry,
    bool selectionValid)
{
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    std::string display = "None";
    std::string description;
    bool missing = false;

    if(reference)
    {
        const auto id = AssetID::ParseReference(*reference);
        const AssetRecord* record = id ? registry.Find(*id) : nullptr;
        missing = true;
        description = *reference;

        if(record && record->type == AssetType::Texture)
        {
            display = record->relativePath.filename().string();
            description = record->relativePath.generic_string() + "\n" + *reference;
            const auto availability = ProjectAssetResolver::Resolve(paths, registry, record->id).status;
            missing = availability != AssetAvailability::Available;
            description += "\n";
            description += TextureAvailabilityReason(*record, availability);

            if(missing)
            {
                display = "Missing / unavailable: " + display;
            }
        }
        else
        {
            display = id ? "Missing / invalid texture reference" : "Invalid texture reference";
            description += !id ? "\nInvalid canonical AssetID." :
                record ? "\nRegistry type mismatch: expected Texture." : "\nAsset is not registered.";
        }
    }

    const float buttonWidth = ImGui::GetFrameHeight();
    const float fieldWidth = (std::max)(1.0f, ImGui::GetContentRegionAvail().x -
        buttonWidth - ImGui::GetStyle().ItemSpacing.x);
    ImGui::SetNextItemWidth(fieldWidth);

    if(missing)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.3f, 1.0f));
    }

    ImGui::InputText("##Reference", display.data(), display.size() + 1, ImGuiInputTextFlags_ReadOnly);

    if(missing)
    {
        ImGui::PopStyleColor();
    }

    if(ImGui::IsItemHovered() && !description.empty())
    {
        ImGui::SetTooltip("%s", description.c_str());
    }

    ImGui::SameLine();

    if(!selectionValid)
    {
        ImGui::BeginDisabled();
    }

    if(ImGui::Button("...", ImVec2(buttonWidth, 0.0f)))
    {
        m_textureSearch.fill(0);
        m_refreshTextureChoices = true;
        ImGui::OpenPopup("TexturePicker");
    }

    if(!selectionValid)
    {
        ImGui::EndDisabled();
    }

    ImGui::SetNextWindowSize(ImVec2(360.0f, 320.0f), ImGuiCond_Appearing);

    if(ImGui::BeginPopup("TexturePicker"))
    {
        if(!selectionValid || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ImGui::CloseCurrentPopup();
        }
        else
        {
            const float closeSize = ImGui::GetFrameHeight();
            const float closePosition = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - closeSize;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Select texture");
            ImGui::SameLine();
            ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), closePosition));

            if(ImGui::Button("X##CloseTexturePicker", ImVec2(closeSize, closeSize)))
            {
                ImGui::CloseCurrentPopup();
            }

            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##Search", "Search filenames...", m_textureSearch.data(), m_textureSearch.size());
            std::string search(m_textureSearch.data());
            const auto lowercase = [](unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            };
            std::transform(search.begin(), search.end(), search.begin(), lowercase);

            if(ImGui::SmallButton("Refresh textures"))
            {
                m_refreshTextureChoices = true;
            }

            const auto textures = registry.GetAssets(AssetType::Texture);
            const bool registryMatches = textures.size() == m_texturePickerRegistry.size() &&
                std::equal(textures.begin(), textures.end(), m_texturePickerRegistry.begin(),
                    [](const AssetRecord& left, const AssetRecord& right)
                    {
                        return left.id == right.id && left.type == right.type && left.relativePath == right.relativePath;
                    });

            // File availability is scanned on opening/Refresh or registry changes,
            // rather than polling every registered path on every popup frame.
            if(m_refreshTextureChoices || !registryMatches)
            {
                m_availableTextures.clear();

                for(const auto& texture : textures)
                {
                    if(ProjectAssetResolver::Resolve(paths, registry, texture.id).status == AssetAvailability::Available)
                    {
                        m_availableTextures.push_back(texture);
                    }
                }

                std::sort(m_availableTextures.begin(), m_availableTextures.end(),
                    [](const AssetRecord& left, const AssetRecord& right)
                    {
                        return left.relativePath < right.relativePath;
                    });
                m_texturePickerRegistry = textures;
                m_refreshTextureChoices = false;
            }

            if(ImGui::BeginChild("TextureList", ImVec2(0.0f, 0.0f)))
            {
                if(ImGui::Selectable("None", !reference))
                {
                    reference.reset();
                    ImGui::CloseCurrentPopup();
                }

                size_t matches = 0;

                for(const auto& texture : m_availableTextures)
                {
                    const auto filename = texture.relativePath.filename().string();
                    std::string filtered = filename;
                    std::transform(filtered.begin(), filtered.end(), filtered.begin(), lowercase);

                    if(filtered.find(search) == std::string::npos)
                    {
                        continue;
                    }

                    ++matches;
                    const auto canonicalReference = texture.id.ToReference();
                    const bool selected = reference && *reference == canonicalReference;
                    ImGui::PushID(canonicalReference.c_str());

                    if(ImGui::Selectable(filename.c_str(), selected))
                    {
                        // A file may disappear after the popup snapshot was captured.
                        // Never commit a stale or unsafe reference from that snapshot.
                        const auto* current = registry.Find(texture.id);
                        const bool identityMatches = current && current->type == AssetType::Texture &&
                            current->relativePath == texture.relativePath;

                        if(identityMatches && ProjectAssetResolver::Resolve(paths, registry, texture.id).status ==
                            AssetAvailability::Available)
                        {
                            if(!selected)
                            {
                                reference = canonicalReference;
                            }

                            ImGui::CloseCurrentPopup();
                        }
                        else
                        {
                            m_refreshTextureChoices = true;
                        }
                    }

                    if(ImGui::IsItemHovered())
                    {
                        const auto relativePath = texture.relativePath.generic_string();
                        ImGui::SetTooltip("%s", relativePath.c_str());
                    }

                    ImGui::PopID();
                }

                if(matches == 0)
                {
                    ImGui::TextWrapped("No available registered textures match this search.");
                }
            }

            ImGui::EndChild();
        }

        ImGui::EndPopup();
    }

    ImGui::PopID();
}

bool InspectorPanel::HasUnsavedChanges() const
{
    return m_material && m_savedMaterial && !EqualMaterials(*m_material, *m_savedMaterial);
}

bool InspectorPanel::TakeRefreshRequest()
{
    return std::exchange(m_refreshRequested, false);
}

void InspectorPanel::Revert()
{
    m_material = m_savedMaterial;
    m_status.clear();
    m_externalConflict = false;
    m_showConflictConfirmation = false;
    m_saveScheduled = false;
    m_persistenceState = PersistenceState::Saved;
}

bool InspectorPanel::Save(
    const ProjectPaths& paths,
    const ProjectAssetRegistry& registry,
    bool overwriteConflict)
{
    m_saveScheduled = false;

    if(!HasUnsavedChanges() && !m_externalConflict)
    {
        m_persistenceState = PersistenceState::Saved;
        return true;
    }

    m_persistenceState = PersistenceState::Saving;
    const bool saved = WriteMaterial(paths, registry, overwriteConflict);
    m_persistenceState = saved ? PersistenceState::Saved : PersistenceState::Failed;
    return saved;
}

bool InspectorPanel::WriteMaterial(
    const ProjectPaths& paths,
    const ProjectAssetRegistry& registry,
    bool overwriteConflict)
{
    if(!m_record || !m_material || paths.GetRoot() != m_projectRoot)
    {
        m_status = "No loaded material in the current project.";
        return false;
    }

    const auto* record = registry.Find(m_record->id);

    if(!record || record->type != AssetType::Material || record->relativePath != m_record->relativePath)
    {
        m_status = "Material registry identity or path changed. Save rejected.";
        return false;
    }

    const auto resolved = ProjectAssetResolver::Resolve(paths, registry, record->id);
    std::string current;

    if(resolved.status != AssetAvailability::Available || !ReadMaterialBytes(resolved.resolvedPath, current))
    {
        m_status = "Material is missing, inaccessible, or has an invalid project path. Save rejected.";
        return false;
    }

    const auto& expected = overwriteConflict ? m_conflictBytes : m_savedBytes;

    if(current != expected || (m_externalConflict && !overwriteConflict))
    {
        m_conflictBytes = std::move(current);
        m_externalConflict = true;
        m_showConflictConfirmation = true;
        m_status = "External file changes detected. Choose explicitly whether to overwrite, or cancel and Refresh.";
        return false;
    }

    try
    {
        // Precompute the exact serializer output before replacement; no fallible reread after publishing.
        std::string saved = MaterialAssetSerializer::Serialize(*m_material).dump(4) + '\n';
        MaterialAssetData baseline = *m_material;

        if(!MaterialAssetSerializer::SaveToFile(*m_material, resolved.resolvedPath, &m_status))
        {
            return false;
        }

        m_savedMaterial = std::move(baseline);
        m_savedBytes = std::move(saved);
        m_externalConflict = false;
        m_showConflictConfirmation = false;
        m_status = "Material saved. Runtime materials are unchanged.";
        return true;
    }
    catch(const std::exception& error)
    {
        m_status = error.what();
        return false;
    }
}

InspectorPanel::LeaveDecision InspectorPanel::DrawUnsavedConfirmation(
    const ProjectPaths& paths,
    const ProjectAssetRegistry& registry)
{
    if(!HasUnsavedChanges())
    {
        m_leaveFlushAttempted = false;
        return LeaveDecision::Proceed;
    }

    if(!m_leaveFlushAttempted)
    {
        m_leaveFlushAttempted = true;

        if(!m_externalConflict && Save(paths, registry))
        {
            m_leaveFlushAttempted = false;
            return LeaveDecision::Proceed;
        }
    }

    constexpr const char* title = "Unsaved material changes";

    if(!ImGui::IsPopupOpen(title))
    {
        ImGui::OpenPopup(title);
    }

    LeaveDecision decision = LeaveDecision::Pending;

    bool confirmationOpen = true;

    if(ImGui::BeginPopupModal(title, &confirmationOpen, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Material changes could not be saved. Retry, discard, or cancel?");
        ImGui::TextWrapped("%s", m_status.c_str());

        if(ImGui::Button("Retry") && Save(paths, registry))
        {
            decision = LeaveDecision::Proceed;
        }

        ImGui::SameLine();

        if(ImGui::Button("Discard"))
        {
            Revert();
            decision = LeaveDecision::Proceed;
        }

        ImGui::SameLine();

        if(ImGui::Button("Cancel"))
        {
            m_showConflictConfirmation = false;
            decision = LeaveDecision::Cancel;
        }

        if(m_externalConflict)
        {
            ImGui::TextWrapped("The file changed outside Engine. Overwrite replaces those external changes.");

            if(ImGui::Button("Overwrite external changes") && Save(paths, registry, true))
            {
                decision = LeaveDecision::Proceed;
            }
        }

        if(decision != LeaveDecision::Pending)
        {
            m_leaveFlushAttempted = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if(!confirmationOpen)
    {
        m_showConflictConfirmation = false;
        m_leaveFlushAttempted = false;
        decision = LeaveDecision::Cancel;
    }

    return decision;
}
