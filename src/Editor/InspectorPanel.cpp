#include "InspectorPanel.h"
#include "EditorWorkspace.h"
#include "Math/Math.h"
#include "assets/ProjectAssetResolver.h"
#include "assets/MaterialAssetSerializer.h"
#include "Renderer/Material.h"

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

const std::optional<MaterialPreviewRequest>& InspectorPanel::GetPreviewRequest()
{
    if(!m_material || !m_record)
    {
        m_previewRequest.reset();
        m_previewDraft.reset();
        m_previewYaw = 0.0f;
        m_previewPitch = 0.0f;
        m_previewOrientationDirty = false;
        return m_previewRequest;
    }

    const std::string identity = m_record->id.ToReference();

    if(m_previewIdentity != identity || m_previewProject != m_projectRoot)
    {
        m_previewYaw = 0.0f;
        m_previewPitch = 0.0f;
        m_previewOrientationDirty = false;
    }

    if(m_previewDraft && m_previewIdentity == identity && m_previewProject == m_projectRoot &&
       EqualMaterials(*m_previewDraft, *m_material))
    {
        if(m_previewRequest)
        {
            m_previewRequest->dependencyRevision = m_previewTextureRefresh;
        }

        if(m_previewRequest && m_previewOrientationDirty)
        {
            m_previewRequest->yaw = m_previewYaw;
            m_previewRequest->pitch = m_previewPitch;
            m_previewRequest->revision = ++m_previewRevision;
            m_previewOrientationDirty = false;
        }

        return m_previewRequest;
    }

    m_previewDraft = m_material;
    m_previewIdentity = identity;
    m_previewProject = m_projectRoot;
    m_previewRequest.reset();

    // Validate the draft through the established serializer without writing it.
    // Invalid edits remain editable/autosave-controlled, but are not sent to the GPU.
    try
    {
        MaterialAssetSerializer::Serialize(*m_material);
        Material material;
        material.model = m_material->model;
        material.roughness = m_material->roughness;
        material.metallic = m_material->metallic;
        material.ior = m_material->ior;

        material.surfaceDetailMode = m_material->surfaceDetailMode;
        material.heightScale = m_material->heightScale;
        material.heightBias = m_material->heightBias;
        material.preferARM = m_material->preferARM;

        for(size_t index = 0; index < 3; ++index)
        {
            material.baseColor[index] = m_material->baseColor[index];
            material.conductorN[index] = m_material->conductorN[index];
            material.conductorK[index] = m_material->conductorK[index];
        }

        m_previewRequest = MaterialPreviewRequest{
            m_projectRoot, identity, ++m_previewRevision, material.GetGPUData(), m_previewYaw, m_previewPitch};
        m_previewRequest->textures = {m_material->baseColorTexture, m_material->normalTexture,
            m_material->roughnessTexture, m_material->metallicTexture, m_material->armTexture,
            m_material->aoTexture, m_material->heightTexture};
        m_previewRequest->dependencyRevision = m_previewTextureRefresh;
        m_previewOrientationDirty = false;
    }
    catch(const std::exception&)
    {
        // Safe idle state until a valid working copy is available again.
    }

    return m_previewRequest;
}

void InspectorPanel::DrawSceneObject(const EditorWorkspaceRect& rectangle, const SceneObjectInspectorView& object,
    const std::function<void()>& drawTransformControls,
    const ProjectPaths& paths, const ProjectAssetRegistry& registry, bool assignmentPending,
    const std::string& assignmentStatus, const std::function<void(std::string)>& requestAssignment)
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

    if(rectangle.size.y < ImGui::GetFrameHeight())
    {
        flags |= ImGuiWindowFlags_NoTitleBar;
    }

    if(ImGui::Begin("Inspector", nullptr, flags))
    {
        EditorWorkspace::KeepCurrentWindowBehindFloatingUI();
        ImGui::TextWrapped("Object: %s", object.name.c_str());
        ImGui::Separator();
        drawTransformControls();
        ImGui::Separator();
        ImGui::TextWrapped("Mesh: %s", object.mesh.c_str());
        ImGui::TextWrapped("Material: %s", object.material.c_str());

        if(!object.metadata)
        {
            ImGui::TextDisabled("Material assignment requires a file-backed scene.");
        }
        else
        {
            if(assignmentPending)
            {
                ImGui::BeginDisabled();
            }

            if(ImGui::SmallButton("Select Material..."))
            {
                m_materialSearch.fill(0);
                m_materialPickerObject = object.metadata;
                m_materialPickerProject = paths.GetRoot();
                m_materialChoices = registry.GetAssets(AssetType::Material);
                std::sort(m_materialChoices.begin(), m_materialChoices.end(),
                    [](const AssetRecord& left, const AssetRecord& right)
                    {
                        return left.relativePath < right.relativePath;
                    });
                ImGui::OpenPopup("SceneMaterialPicker");
            }

            if(assignmentPending)
            {
                ImGui::EndDisabled();
            }

            DrawSceneMaterialPicker(object, paths, registry, assignmentPending, requestAssignment);
        }

        if(!assignmentStatus.empty())
        {
            ImGui::TextWrapped("%s", assignmentStatus.c_str());
        }

    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void InspectorPanel::DrawSceneMaterialPicker(
    const SceneObjectInspectorView& object,
    const ProjectPaths& paths,
    const ProjectAssetRegistry& registry,
    bool assignmentPending,
    const std::function<void(std::string)>& requestAssignment)
{
    ImGui::SetNextWindowSize(ImVec2(360.0f, 320.0f), ImGuiCond_Appearing);

    if(ImGui::BeginPopup("SceneMaterialPicker"))
    {
        const auto& selection = *object.metadata;
        const bool matches = m_materialPickerObject && m_materialPickerProject == paths.GetRoot() &&
            m_materialPickerObject->runtimeIdentity == selection.runtimeIdentity &&
            m_materialPickerObject->sceneID == selection.sceneID &&
            m_materialPickerObject->objectID == selection.objectID &&
            m_materialPickerObject->runtimeIndex == selection.runtimeIndex &&
            m_materialPickerObject->structureRevision == selection.structureRevision;

        if(!matches || assignmentPending || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            ImGui::CloseCurrentPopup();
        }
        else
        {
            const float closeSize = ImGui::GetFrameHeight();
            const float closeX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - closeSize;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Select material");
            ImGui::SameLine();
            ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), closeX));

            if(ImGui::Button("X##CloseMaterialPicker", ImVec2(closeSize, closeSize)))
            {
                ImGui::CloseCurrentPopup();
            }

            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##MaterialSearch", "Search materials...",
                m_materialSearch.data(), m_materialSearch.size());
            const auto lowercase = [](unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            };
            std::string search(m_materialSearch.data());
            std::transform(search.begin(), search.end(), search.begin(), lowercase);

            if(ImGui::BeginChild("MaterialChoices", ImVec2(0.0f, 0.0f)))
            {
                const bool defaultSelected = selection.materialReference.empty() ||
                    selection.materialReference == "builtin:default";

                if(ImGui::Selectable("Default Material", defaultSelected))
                {
                    if(!defaultSelected)
                    {
                        requestAssignment("builtin:default");
                    }

                    ImGui::CloseCurrentPopup();
                }

                size_t count = 0;

                for(const auto& record : m_materialChoices)
                {
                    const auto* current = registry.Find(record.id);

                    if(!current || current->type != AssetType::Material || current->relativePath != record.relativePath ||
                       ProjectAssetResolver::Resolve(paths, registry, record.id).status != AssetAvailability::Available)
                    {
                        continue;
                    }

                    std::string label = record.relativePath.generic_string();
                    std::string filtered = label;
                    std::transform(filtered.begin(), filtered.end(), filtered.begin(), lowercase);
                    std::string extension = record.relativePath.extension().string();
                    std::transform(extension.begin(), extension.end(), extension.begin(), lowercase);

                    if(extension != ".material" || filtered.find(search) == std::string::npos)
                    {
                        continue;
                    }

                    ++count;
                    const auto reference = record.id.ToReference();
                    const bool selected = selection.materialReference == reference;
                    ImGui::PushID(reference.c_str());

                    if(ImGui::Selectable(label.c_str(), selected))
                    {
                        if(!selected)
                        {
                            requestAssignment(reference);
                        }

                        ImGui::CloseCurrentPopup();
                    }

                    ImGui::PopID();
                }

                if(count == 0)
                {
                    ImGui::TextDisabled("No available matching materials.");
                }
            }

            ImGui::EndChild();
        }

        ImGui::EndPopup();
    }
}

void InspectorPanel::Draw(
    const EditorWorkspaceRect& rectangle,
    const ProjectPaths& paths,
    const ProjectAssetRegistry& registry,
    const std::optional<MaterialPreviewImage>& preview,
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

    if(m_material)
    {
        flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    }

    // ImGui otherwise enforces title-bar height even when the assigned panel is smaller.
    if(rectangle.size.y < ImGui::GetFrameHeight())
    {
        flags |= ImGuiWindowFlags_NoTitleBar;
    }

    if(ImGui::Begin("Inspector", nullptr, flags))
    {
        EditorWorkspace::KeepCurrentWindowBehindFloatingUI();
        const bool materialLayout = m_material.has_value();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float spacing = ImGui::GetStyle().ItemSpacing.y;
        const float labelHeight = ImGui::GetTextLineHeightWithSpacing();
        const float previewSize = (std::max)(0.0f, (std::min)(available.x,
            available.y * 0.45f - labelHeight - spacing * 2.0f));

        if(materialLayout)
        {
            const float propertiesHeight = (std::max)(1.0f,
                available.y - previewSize - labelHeight - spacing * 2.0f - 1.0f);
            ImGui::BeginChild("Material Properties", ImVec2(0.0f, propertiesHeight));
        }

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

        if(materialLayout)
        {
            ImGui::EndChild();
            ImGui::Separator();
            ImGui::TextUnformatted("Material Preview");
            const auto& request = GetPreviewRequest();
            const ImVec2 remaining = ImGui::GetContentRegionAvail();
            const float size = (std::max)(0.0f, (std::min)(previewSize,
                (std::min)(remaining.x, remaining.y)));

            if(size > 0.0f)
            {
                if(preview && request && preview->srv.ptr != 0 &&
                   preview->projectRoot == request->projectRoot &&
                   preview->materialIdentity == request->materialIdentity)
                {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (remaining.x - size) * 0.5f);
                    ImGui::PushID(request->materialIdentity.c_str());
                    ImGui::InvisibleButton("Preview Rotation", ImVec2(size, size), ImGuiButtonFlags_MouseButtonLeft);
                    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(preview->srv.ptr),
                        ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

                    if(ImGui::IsItemHovered() || ImGui::IsItemActive())
                    {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                    }

                    const float previousYaw = m_previewYaw;
                    const float previousPitch = m_previewPitch;

                    if(ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        m_previewYaw = 0.0f;
                        m_previewPitch = 0.0f;
                    }
                    else if(ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
                    {
                        const ImVec2 delta = ImGui::GetIO().MouseDelta;

                        if(std::isfinite(delta.x) && std::isfinite(delta.y))
                        {
                            // Radians per logical mouse pixel, independent of frame time.
                            constexpr float sensitivity = 0.01f;
                            constexpr float twoPi = 6.28318530718f;
                            // With the fixed camera looking toward +Z, subtract mouse
                            // deltas so the visible surface follows the screen-space drag.
                            m_previewYaw = std::remainder(m_previewYaw - delta.x * sensitivity, twoPi);
                            m_previewPitch = std::clamp(m_previewPitch - delta.y * sensitivity, -1.4f, 1.4f);
                        }
                    }

                    if(previousYaw != m_previewYaw || previousPitch != m_previewPitch)
                    {
                        m_previewOrientationDirty = true;
                    }

                    ImGui::PopID();
                }
                else
                {
                    ImGui::TextWrapped("Preview unavailable or pending.");
                }
            }
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
    EndWorkingMaterial();
    m_lastWorkingDraft.reset();
    m_lastWorkingBaseline.reset();
    ++m_texturePickerGeneration;
    ++m_previewTextureRefresh;
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

    if(material.model == MaterialModel::Dielectric || material.model == MaterialModel::Conductor)
    {
        ImGui::SeparatorText("Optical properties");
    }

    if(material.model == MaterialModel::Dielectric)
    {
        if(ImGui::InputFloat("IOR (> 0)", &material.ior))
        {
            material.ior = (std::max)(material.ior, (std::numeric_limits<float>::min)());
        }
    }
    else if(material.model == MaterialModel::Conductor)
    {
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
    }

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

        if(record && record->type == AssetType::Texture)
        {
            display = record->relativePath.filename().string();
            description = record->relativePath.generic_string();
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
            description += !id ? "Invalid texture reference." :
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
            DrawTexturePickerContents(reference, paths, registry);
        }

        ImGui::EndPopup();
    }

    ImGui::PopID();
}

void InspectorPanel::DrawTexturePickerContents(
    std::optional<std::string>& reference,
    const ProjectPaths& paths,
    const ProjectAssetRegistry& registry)
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
        ++m_previewTextureRefresh;
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

std::optional<std::filesystem::path> InspectorPanel::TakeSavedMaterialSource()
{
    return std::exchange(m_savedThumbnailSource, std::nullopt);
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
        auto thumbnailSource = resolved.resolvedPath;
        MaterialSavedRevision revision{std::filesystem::canonical(paths.GetRoot()), record->id,
            record->relativePath, saved, m_saveSequence + 1};
        // Allocate notification storage before publishing the file replacement.
        m_savedMaterialRevisions.reserve(m_savedMaterialRevisions.size() + 1);

        if(!MaterialAssetSerializer::SaveToFile(*m_material, resolved.resolvedPath, &m_status))
        {
            return false;
        }

        ++m_saveSequence;
        CoalesceMaterialSavedRevision(m_savedMaterialRevisions, std::move(revision));
        m_savedThumbnailSource = std::move(thumbnailSource);
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

std::vector<MaterialSavedRevision> InspectorPanel::TakeSavedMaterialRevisions()
{
    return std::exchange(m_savedMaterialRevisions, {});
}

void InspectorPanel::EndWorkingMaterial()
{
    if(m_workingWasActive && m_record)
    {
        std::error_code error;
        const auto root = std::filesystem::canonical(m_projectRoot, error);

        if(!error)
        {
            m_workingMaterialRevisions.push_back({root, m_record->id, m_record->relativePath,
                ++m_workingSequence, m_saveSequence, MaterialWorkingRevision::Action::Leave, std::nullopt});
        }
    }

    m_workingWasActive = false;
}

void InspectorPanel::ResetWorkingMaterialNotifications()
{
    m_workingMaterialRevisions.clear();
    m_lastWorkingDraft.reset();
    m_lastWorkingBaseline.reset();
    m_workingWasActive = false;
}

std::vector<MaterialWorkingRevision> InspectorPanel::TakeWorkingMaterialRevisions(bool active)
{
    if(!active || !m_material || !m_record || !m_savedMaterial)
    {
        EndWorkingMaterial();
        m_lastWorkingDraft.reset();
        m_lastWorkingBaseline.reset();
        return std::exchange(m_workingMaterialRevisions, {});
    }

    if(m_workingWasActive && m_lastWorkingDraft && m_lastWorkingBaseline &&
       EqualMaterials(*m_lastWorkingDraft, *m_material) && EqualMaterials(*m_lastWorkingBaseline, *m_savedMaterial) &&
       m_lastWorkingSaveSequence == m_saveSequence)
    {
        return std::exchange(m_workingMaterialRevisions, {});
    }

    std::error_code error;
    const auto root = std::filesystem::canonical(m_projectRoot, error);

    if(error)
    {
        return std::exchange(m_workingMaterialRevisions, {});
    }

    MaterialWorkingRevision revision{root, m_record->id, m_record->relativePath, ++m_workingSequence, m_saveSequence};

    try
    {
        MaterialAssetSerializer::Serialize(*m_material); // Validate the entire draft before extracting parameters.
        const auto parametersOnly = [](MaterialAssetData data)
        {
            data.baseColorTexture.reset();
            data.normalTexture.reset();
            data.roughnessTexture.reset();
            data.metallicTexture.reset();
            data.armTexture.reset();
            data.aoTexture.reset();
            data.heightTexture.reset();
            return data;
        };
        revision.parameters = parametersOnly(*m_material);

        if(m_lastWorkingDraft && m_lastWorkingSaveSequence != m_saveSequence &&
           EqualMaterials(*m_material, *m_savedMaterial))
        {
            revision.action = MaterialWorkingRevision::Action::Saved;
        }
        else
        {
            revision.action = EqualMaterials(*revision.parameters, parametersOnly(*m_savedMaterial)) ?
                MaterialWorkingRevision::Action::Restore : MaterialWorkingRevision::Action::Draft;
        }
    }
    catch(const std::exception&)
    {
        revision.action = MaterialWorkingRevision::Action::Invalid;
        revision.parameters.reset();
    }

    m_workingMaterialRevisions.push_back(std::move(revision));
    m_lastWorkingDraft = m_material;
    m_lastWorkingBaseline = m_savedMaterial;
    m_lastWorkingSaveSequence = m_saveSequence;
    m_workingWasActive = true;
    return std::exchange(m_workingMaterialRevisions, {});
}
