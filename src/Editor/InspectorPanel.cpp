#include "InspectorPanel.h"
#include "InspectorNumericDrag.h"
#include "EditorWorkspace.h"
#include "Math/Math.h"
#include "assets/ProjectAssetResolver.h"
#include "assets/MaterialAssetSerializer.h"
#include "Renderer/Material.h"
#include "Renderer/Renderer.h"

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

std::optional<std::string> InspectorPanel::TakeMaterialLocateRequest()
{ return std::exchange(m_materialLocateRequest,std::nullopt); }
void InspectorPanel::HandleMaterialLocateItem(std::string_view reference)
{
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        m_materialLocateRequest=std::string(reference);
}
void InspectorPanel::DrawSceneObject(const EditorWorkspaceRect& rectangle, const SceneObjectInspectorView& object,
    const std::function<void()>& drawTransformControls,
    const ProjectPaths& paths, const ProjectAssetRegistry& registry,
    bool assignmentPending, const std::string& assignmentStatus)
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
        DrawSceneAssetTiles(object, paths, registry, assignmentPending);
        DrawSceneMaterialPicker(object, paths, registry, assignmentPending);
        if(!m_materialLocateStatus.empty()) ImGui::TextWrapped("%s",m_materialLocateStatus.c_str());

        if(!assignmentStatus.empty())
        {
            ImGui::TextWrapped("%s", assignmentStatus.c_str());
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

namespace
{
    bool DrawScenePreviewTile(float extent, const char* placeholder,
        const std::function<std::optional<D3D12_GPU_DESCRIPTOR_HANDLE>()>& requestImage,
        bool interactive = false, bool selected = false)
    {
        const auto start = ImGui::GetCursorScreenPos();
        bool pressed = false;

        if(interactive)
        {
            pressed = ImGui::InvisibleButton("##PreviewTile", ImVec2(extent, extent));
        }
        else
        {
            ImGui::Dummy(ImVec2(extent, extent));
        }

        if(!ImGui::IsItemVisible())
        {
            return false;
        }

        const ImVec2 end(start.x + extent, start.y + extent);
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(start, end, IM_COL32(55, 55, 55, 255));
        const auto image = requestImage();

        if(image)
        {
            draw->AddImage(static_cast<ImTextureID>(image->ptr), start, end);
        }
        else
        {
            draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(start.x + 6.0f, start.y + 6.0f),
                IM_COL32(190, 190, 190, 255), placeholder, nullptr, extent - 12.0f);
        }

        const ImVec2 cornerStart(end.x - 22.0f, end.y - 18.0f);
        draw->AddRectFilled(cornerStart, end, IM_COL32(35, 35, 35, 230));
        draw->AddText(ImVec2(cornerStart.x + 4.0f, cornerStart.y), IM_COL32(200, 200, 200, 255), "...");

        if(selected)
        {
            draw->AddRect(start, end, IM_COL32(110, 175, 255, 255), 0.0f, 0, 2.0f);
        }

        if(ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", interactive ? "Choose material for this slot." : "Selection controls will be available later.");
        }

        return pressed;
    }
}

void InspectorPanel::ReleaseSceneObjectPreview()
{
    if(m_thumbnailRenderer && m_meshThumbnailTicket)
    {
        m_thumbnailRenderer->DiscardMeshThumbnail(m_meshThumbnailTicket,
            MaterialPreviewRenderer::MeshThumbnailConsumer::SceneInspector);
    }

    m_meshThumbnailTicket = 0;
    m_meshThumbnailReference.clear();
    m_meshThumbnailProject.clear();
    m_meshThumbnailValidation = {};
}

std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> InspectorPanel::GetSceneMeshThumbnail(
    const ProjectPaths& paths, const ProjectAssetRegistry& registry, const std::string& reference)
{
    const auto id = AssetID::ParseReference(reference);
    const auto* record = id ? registry.Find(*id) : nullptr;

    if(!m_thumbnailRenderer || !record || record->type != AssetType::Mesh ||
        ProjectAssetResolver::Resolve(paths, registry, *id).status != AssetAvailability::Available)
    {
        ReleaseSceneObjectPreview();
        return std::nullopt;
    }

    const auto now = std::chrono::steady_clock::now();

    if(now >= m_meshThumbnailValidation)
    {
        if(m_meshThumbnailTicket && !m_thumbnailRenderer->IsMeshThumbnailRequestCurrent(m_meshThumbnailTicket))
        {
            ReleaseSceneObjectPreview();
        }

        std::string error;
        m_thumbnailRenderer->RequestMeshThumbnail(paths, registry, *id, m_meshThumbnailTicket, error,
            MaterialPreviewRenderer::MeshThumbnailConsumer::SceneInspector);
        m_meshThumbnailReference = reference;
        m_meshThumbnailProject = paths.GetRoot();
        m_meshThumbnailValidation = now + std::chrono::milliseconds(m_meshThumbnailTicket ? 2000 : 250);
    }

    const auto image = m_thumbnailRenderer->GetMeshThumbnailImage(m_meshThumbnailTicket);
    return image ? std::optional<D3D12_GPU_DESCRIPTOR_HANDLE>(image->srv) : std::nullopt;
}

void InspectorPanel::DrawSceneAssetTiles(const SceneObjectInspectorView& object,
    const ProjectPaths& paths, const ProjectAssetRegistry& registry, bool assignmentPending)
{
    if(m_meshThumbnailReference != object.meshReference || m_meshThumbnailProject != paths.GetRoot())
    {
        ReleaseSceneObjectPreview();
    }

    const float extent = (std::max)(16.0f, (std::min)(128.0f, ImGui::GetContentRegionAvail().x));
    ImGui::TextUnformatted("Mesh");
    const auto meshID = AssetID::ParseReference(object.meshReference);
    const char* meshPlaceholder = object.meshResidency == SceneMeshResidency::Pending ? "Pending / unavailable" :
        object.mesh == "No mesh" ? "No mesh" :
        meshID ? "Pending / unavailable" : "Built-in / runtime mesh";
    DrawScenePreviewTile(extent, meshPlaceholder, [&]()
    {
        return GetSceneMeshThumbnail(paths, registry, object.meshReference);
    });
    ImGui::TextWrapped("%s", object.mesh.c_str());
    ImGui::Spacing();
    ImGui::Separator();

    if(object.meshResidency == SceneMeshResidency::Pending && !object.materialSlotMetadataAvailable)
    {
        ImGui::TextUnformatted("Materials");
        ImGui::TextDisabled("Mesh metadata unavailable. Authored overrides are retained.");
        ImGui::TextWrapped("Fallback: %s", object.material.c_str());
        if(object.metadata) HandleMaterialLocateItem(object.metadata->materialReference);

        if(object.metadata)
        {
            for(const auto& [slotID, reference] : object.metadata->materialSlotBindings)
            {
                const auto id = AssetID::ParseReference(reference);
                const auto* record = id ? registry.Find(*id) : nullptr;
                const auto materialName = record && record->type == AssetType::Material ?
                    record->relativePath.stem().generic_string() : "Unavailable: " + reference;
                ImGui::TextWrapped("Override %s: %s", slotID.c_str(), materialName.c_str());
                HandleMaterialLocateItem(reference);
            }
        }

        return;
    }

    if (object.meshResidency == SceneMeshResidency::Pending)
    {
        ImGui::TextDisabled("Geometry pending; using retained mesh-slot metadata.");

        if (object.metadata)
        {
            for (const auto& [slotID, reference] : object.metadata->materialSlotBindings)
            {
                const bool known = std::any_of(object.materialSlots.begin(), object.materialSlots.end(),
                    [&](const auto& slot) { return slot.id == slotID; });

                if (!known)
                {
                    ImGui::TextWrapped("Unrecognized authored override %s: %s (retained; requires compatible mesh metadata)",
                        slotID.c_str(), reference.c_str());
                    HandleMaterialLocateItem(reference);
                }
            }
        }
    }

    if(object.materialSlots.size() > 1)
    {
        ImGui::Text("Materials (%zu)", object.materialSlots.size());
        const bool interactive = object.metadata.has_value() && !assignmentPending && !m_slotAssignmentPending;
        const auto selectedSlot = DrawCompactMaterialSlots(object, interactive);

        if(selectedSlot)
        {
            OpenSceneMaterialPicker(object, paths, *selectedSlot);
        }

        return;
    }

    ImGui::TextUnformatted("Materials");

    if(object.materialSlots.empty())
    {
        ImGui::TextDisabled("No material slots available.");
        return;
    }

    std::optional<std::string> selectedSlot;

    for(const auto& slot : object.materialSlots)
    {
        ImGui::PushID(slot.id.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("%s", slot.name.c_str());
        const auto materialID = AssetID::ParseReference(slot.materialReference);
        const char* materialPlaceholder = !slot.available ? "Unavailable" :
            materialID ? "Pending / unavailable" : object.metadata ? "Default material" : "Runtime material";
        const bool pressed = DrawScenePreviewTile(extent, materialPlaceholder,
            [&]() -> std::optional<D3D12_GPU_DESCRIPTOR_HANDLE>
        {
            if(!m_thumbnailRenderer || !slot.available || !materialID)
            {
                return std::nullopt;
            }

            std::string error;
            m_thumbnailRenderer->RequestSavedMaterialThumbnail(paths, registry, *materialID, error);
            const auto image = m_thumbnailRenderer->GetSavedMaterialThumbnailImage(paths, *materialID);
            return image ? std::optional<D3D12_GPU_DESCRIPTOR_HANDLE>(image->srv) : std::nullopt;
        }, object.metadata.has_value() && !assignmentPending && !m_slotAssignmentPending);

        HandleMaterialLocateItem(slot.materialReference);
        if(pressed)
        {
            selectedSlot = slot.id;
        }

        ImGui::TextWrapped("%s", slot.materialName.c_str());
        HandleMaterialLocateItem(slot.materialReference);
        ImGui::TextDisabled("%s", slot.explicitBinding ? "Explicit slot override" : "Inherited fallback");
        ImGui::PopID();
    }

    if(selectedSlot)
    {
        OpenSceneMaterialPicker(object, paths, *selectedSlot);
    }
}

std::optional<std::string> InspectorPanel::DrawCompactMaterialSlots(
    const SceneObjectInspectorView& object, bool interactive)
{
    if(!ImGui::BeginTable("ObjectMaterialSlots", 3, ImGuiTableFlags_SizingStretchProp))
    {
        return std::nullopt;
    }

    ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthStretch, 0.35f);
    ImGui::TableSetupColumn("Material", ImGuiTableColumnFlags_WidthStretch, 0.65f);
    ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Override").x);
    std::optional<std::string> selectedSlot;
    size_t slotIndex = 0;

    for(const auto& slot : object.materialSlots)
    {
        ImGui::PushID(slot.id.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        const auto label = slot.name.empty() ? "Slot " + std::to_string(slotIndex) : slot.name;
        ImGui::TextUnformatted(label.c_str());

        if(ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", label.c_str());
        }

        ImGui::TableNextColumn();

        if(!interactive)
        {
            ImGui::BeginDisabled();
        }

        const auto materialName = slot.materialName.empty() ? std::string("Unavailable material") : slot.materialName;

        if(ImGui::Button(materialName.c_str(), ImVec2(-1.0f, 0.0f)))
        {
            selectedSlot = slot.id;
        }

        HandleMaterialLocateItem(slot.materialReference);
        if(ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s\nClick to choose a material for this slot. Right-click to locate it in Asset Browser.", materialName.c_str());
        }

        if(!interactive)
        {
            ImGui::EndDisabled();
        }

        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", slot.explicitBinding ? "Override" : "Fallback");
        ImGui::PopID();
        ++slotIndex;
    }

    ImGui::EndTable();
    return selectedSlot;
}

void InspectorPanel::OpenSceneMaterialPicker(const SceneObjectInspectorView& object, const ProjectPaths& paths,
    const std::string& slotID)
{
    DismissSceneMaterialPicker();
    m_materialPickerObject = object.metadata;
    m_materialPickerProject = paths.GetRoot();
    m_materialPickerSlot = slotID;
    m_materialSearch.fill(0);

    ImGui::OpenPopup("SceneMaterialSlotPicker");
}

void InspectorPanel::DismissSceneMaterialPicker()
{
    m_materialPickerObject.reset();
    m_materialPickerSlot.clear();
    m_materialPickerProject.clear();
    m_materialPickerStatus.clear();

    m_slotAssignmentRequest.reset();
    m_slotAssignmentPending = false;
    m_closeSlotPicker = false;
}

std::optional<InspectorPanel::SlotAssignmentRequest> InspectorPanel::TakeSlotAssignmentRequest()
{
    return std::exchange(m_slotAssignmentRequest, std::nullopt);
}

void InspectorPanel::OnSlotAssignmentCompleted(const SceneObjectMetadata& selection, const std::string& slotID,
    bool succeeded, const std::string& message)
{
    if(!m_materialPickerObject || !m_slotAssignmentPending || slotID != m_materialPickerSlot ||
        selection.runtimeIdentity != m_materialPickerObject->runtimeIdentity ||
        selection.sceneID != m_materialPickerObject->sceneID || selection.objectID != m_materialPickerObject->objectID ||
        selection.structureRevision != m_materialPickerObject->structureRevision)
    {
        return;
    }

    m_slotAssignmentPending = false;
    m_materialPickerStatus = message;
    m_closeSlotPicker = succeeded;
}

bool InspectorPanel::MaterialPickerMatches(const SceneObjectInspectorView& object, const ProjectPaths& paths) const
{
    if(!object.metadata || !m_materialPickerObject || m_materialPickerProject != paths.GetRoot())
    {
        return false;
    }

    const auto& current = *object.metadata;
    const auto& opened = *m_materialPickerObject;
    const bool sameObject = current.runtimeIdentity == opened.runtimeIdentity && current.sceneID == opened.sceneID &&
        current.objectID == opened.objectID && current.runtimeIndex == opened.runtimeIndex &&
        current.structureRevision == opened.structureRevision;
    const bool sameBindings = current.meshReference == opened.meshReference &&
        current.materialReference == opened.materialReference && current.materialSlotBindings == opened.materialSlotBindings;

    return sameObject && sameBindings && std::any_of(object.materialSlots.begin(), object.materialSlots.end(),
        [&](const auto& slot) { return slot.id == m_materialPickerSlot; });
}

void InspectorPanel::RequestSlotMaterial(const SceneObjectInspectorView& object, const ProjectPaths& paths,
    const ProjectAssetRegistry& registry, const std::optional<AssetID>& materialID)
{
    if(m_slotAssignmentPending || !MaterialPickerMatches(object, paths))
    {
        m_materialPickerStatus = "Object, mesh, or slot changed. Reopen the material picker.";
        return;
    }

    const auto meshID = AssetID::ParseReference(object.meshReference);
    const auto* meshRecord = meshID ? registry.Find(*meshID) : nullptr;

    if(meshID && (!meshRecord || meshRecord->type != AssetType::Mesh ||
        ProjectAssetResolver::Resolve(paths, registry, *meshID).status != AssetAvailability::Available))
    {
        m_materialPickerStatus = "The selected mesh asset is no longer available.";
        return;
    }

    if(materialID)
    {
        const auto* record = registry.Find(*materialID);

        if(!record || record->type != AssetType::Material ||
            ProjectAssetResolver::Resolve(paths, registry, *materialID).status != AssetAvailability::Available)
        {
            m_materialPickerStatus = "The selected material asset is no longer available.";
            return;
        }
    }

    m_slotAssignmentRequest = SlotAssignmentRequest{*m_materialPickerObject, m_materialPickerSlot, materialID};
    m_slotAssignmentPending = true;
    m_materialPickerStatus = "Applying material slot...";
}

void InspectorPanel::DrawSlotMaterialGrid(const SceneObjectInspectorView& object, const SceneMaterialSlotInspectorView& slot,
    const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    const auto lowercase = [](unsigned char character) { return static_cast<char>(std::tolower(character)); };
    std::string search(m_materialSearch.data());
    std::transform(search.begin(), search.end(), search.begin(), lowercase);
    m_materialChoices = registry.GetAssets(AssetType::Material);
    std::sort(m_materialChoices.begin(), m_materialChoices.end(), [](const auto& left, const auto& right)
    {
        return left.relativePath < right.relativePath;
    });

    if(!ImGui::BeginTable("SlotMaterialGrid", 2, ImGuiTableFlags_SizingStretchSame))
    {
        return;
    }

    size_t matching = 0;
    size_t thumbnailRequests = 0;

    for(const auto& record : m_materialChoices)
    {
        std::string filtered = record.relativePath.generic_string();
        std::transform(filtered.begin(), filtered.end(), filtered.begin(), lowercase);

        if(filtered.find(search) == std::string::npos)
        {
            continue;
        }

        ++matching;
        ImGui::TableNextColumn();
        const auto reference = record.id.ToReference();
        const bool selected = slot.materialReference == reference;
        ImGui::PushID(reference.c_str());
        const float extent = (std::max)(32.0f, (std::min)(120.0f, ImGui::GetContentRegionAvail().x));
        const bool pressed = DrawScenePreviewTile(extent, "Pending / unavailable",
            [&]() -> std::optional<D3D12_GPU_DESCRIPTOR_HANDLE>
        {
            if(!m_thumbnailRenderer || thumbnailRequests++ >= MaterialPreviewRenderer::kThumbnailCapacity)
            {
                return std::nullopt;
            }

            std::string error;
            m_thumbnailRenderer->RequestSavedMaterialThumbnail(paths, registry, record.id, error);
            const auto image = m_thumbnailRenderer->GetSavedMaterialThumbnailImage(paths, record.id);
            return image ? std::optional<D3D12_GPU_DESCRIPTOR_HANDLE>(image->srv) : std::nullopt;
        }, true, selected);
        ImGui::TextWrapped("%s", record.relativePath.stem().generic_string().c_str());

        if(selected)
        {
            ImGui::TextDisabled("%s", slot.explicitBinding ? "Current override" : "Current fallback");
        }

        if(pressed)
        {
            RequestSlotMaterial(object, paths, registry, record.id);
        }

        ImGui::PopID();
    }

    ImGui::EndTable();

    if(matching == 0)
    {
        ImGui::TextDisabled("No matching registered materials.");
    }
}

void InspectorPanel::DrawSceneMaterialPicker(const SceneObjectInspectorView& object,
    const ProjectPaths& paths, const ProjectAssetRegistry& registry, bool assignmentPending)
{
    ImGui::SetNextWindowSize(ImVec2(360.0f, 480.0f), ImGuiCond_Appearing);

    if(!ImGui::BeginPopup("SceneMaterialSlotPicker"))
    {
        if(!m_slotAssignmentPending)
        {
            DismissSceneMaterialPicker();
        }

        return;
    }

    if(m_closeSlotPicker || !MaterialPickerMatches(object, paths) || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        DismissSceneMaterialPicker();
        return;
    }

    const auto selectedSlot = std::find_if(object.materialSlots.begin(), object.materialSlots.end(),
        [&](const auto& slot) { return slot.id == m_materialPickerSlot; });
    ImGui::TextWrapped("Material: %s", selectedSlot->name.c_str());
    ImGui::TextWrapped("Current: %s", selectedSlot->materialName.c_str());
    ImGui::TextDisabled("%s", selectedSlot->explicitBinding ? "Explicit override" : "Inherited fallback");

    if(!m_materialPickerStatus.empty())
    {
        ImGui::TextWrapped("%s", m_materialPickerStatus.c_str());
    }

    const bool blocked = assignmentPending || m_slotAssignmentPending;

    if(blocked)
    {
        ImGui::BeginDisabled();
    }

    if(ImGui::Button("Use fallback material"))
    {
        RequestSlotMaterial(object, paths, registry, std::nullopt);
    }

    ImGui::SameLine();

    if(ImGui::Button("Close"))
    {
        ImGui::CloseCurrentPopup();
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##SlotMaterialSearch", "Search materials...", m_materialSearch.data(), m_materialSearch.size());
    ImGui::Separator();

    if(ImGui::BeginChild("SlotMaterialChoices", ImVec2(0.0f, 0.0f)))
    {
        DrawSlotMaterialGrid(object, *selectedSlot, paths, registry);
    }

    ImGui::EndChild();

    if(blocked)
    {
        ImGui::EndDisabled();
    }

    ImGui::EndPopup();
}

void InspectorPanel::Draw(
    const EditorWorkspaceRect& rectangle,
    const ProjectPaths& paths,
    const ProjectAssetRegistry& registry,
    const std::optional<MaterialPreviewImage>& preview,
    bool protectedAction)
{
    m_texturePickerDrawn = false;

    if(protectedAction)
    {
        DismissTexturePicker();
    }

    if(rectangle.size.x <= 0.0f || rectangle.size.y <= 0.0f)
    {
        DismissTexturePicker();
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

    if(!m_texturePickerDrawn)
    {
        DismissTexturePicker();
    }

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
    DismissTexturePicker();
    EndWorkingMaterial();
    m_lastWorkingDraft.reset();
    m_lastWorkingBaseline.reset();
    ++m_texturePickerGeneration;
    ++m_previewTextureRefresh;
    m_textureSearch.fill(0);
    m_texturePickerRegistry.clear();
    m_textureChoices.clear();
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

    // Transform-style drag controls edit the unsaved material draft directly; Save and
    // Revert keep their existing meaning. Ranges and clamps match the previous controls:
    // roughness and metallic were always clamped to [0, 1], including typed values.
    constexpr InspectorDragSpec unitIntervalDrag{0.005f, 0.0f, 1.0f, true, "%.4f"};
    InspectorDragFloats("Roughness", &material.roughness, 1, unitIntervalDrag);
    InspectorDragFloats("Metallic", &material.metallic, 1, unitIntervalDrag);

    if(material.model == MaterialModel::Dielectric || material.model == MaterialModel::Conductor)
    {
        ImGui::SeparatorText("Optical properties");
    }

    if(material.model == MaterialModel::Dielectric)
    {
        constexpr InspectorDragSpec iorDrag{0.005f};

        if(InspectorDragFloats("IOR (> 0)", &material.ior, 1, iorDrag) != InspectorDragResult::Idle)
        {
            material.ior = (std::max)(material.ior, (std::numeric_limits<float>::min)());
        }
    }
    else if(material.model == MaterialModel::Conductor)
    {
        constexpr InspectorDragSpec conductorDrag{0.01f};

        if(InspectorDragFloats("Conductor N (>= 0)", material.conductorN.data(), 3, conductorDrag) != InspectorDragResult::Idle)
        {
            for(auto& value : material.conductorN)
            {
                value = (std::max)(value, 0.0f);
            }
        }

        if(InspectorDragFloats("Conductor K (>= 0)", material.conductorK.data(), 3, conductorDrag) != InspectorDragResult::Idle)
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

    constexpr InspectorDragSpec heightDrag{0.001f};
    InspectorDragFloats("Height scale", &material.heightScale, 1, heightDrag);
    InspectorDragFloats("Height bias", &material.heightBias, 1, heightDrag);
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

    DrawTextureReference("Base color", TextureSlot::BaseColor, material.baseColorTexture, paths, registry, selectionValid);
    DrawTextureReference("Normal", TextureSlot::Normal, material.normalTexture, paths, registry, selectionValid);
    DrawTextureReference("Roughness", TextureSlot::Roughness, material.roughnessTexture, paths, registry, selectionValid);
    DrawTextureReference("Metallic", TextureSlot::Metallic, material.metallicTexture, paths, registry, selectionValid);
    DrawTextureReference("ARM", TextureSlot::ARM, material.armTexture, paths, registry, selectionValid);
    DrawTextureReference("AO", TextureSlot::AO, material.aoTexture, paths, registry, selectionValid);
    DrawTextureReference("Height", TextureSlot::Height, material.heightTexture, paths, registry, selectionValid);

    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
}

void InspectorPanel::DrawTextureReference(
    const char* label,
    TextureSlot slot,
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
        DismissTexturePicker();
        m_texturePicker = TexturePickerIdentity{*m_record, m_projectRoot, slot, m_texturePickerGeneration};
        m_textureSearch.fill(0);
        m_refreshTextureChoices = true;
        ImGui::OpenPopup("TexturePicker");
    }

    if(!selectionValid)
    {
        ImGui::EndDisabled();
    }

    ImGui::SetNextWindowSize(ImVec2(360.0f, 480.0f), ImGuiCond_Appearing);

    if(ImGui::BeginPopup("TexturePicker"))
    {
        m_texturePickerDrawn = true;

        if(!selectionValid || !TexturePickerMatches(slot, paths, registry) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ImGui::CloseCurrentPopup();
            DismissTexturePicker();
        }
        else
        {
            DrawTexturePickerContents(slot, paths, registry);
        }

        ImGui::EndPopup();
    }

    if(m_texturePicker && m_texturePicker->slot == slot && !ImGui::IsPopupOpen("TexturePicker"))
    {
        DismissTexturePicker();
    }

    ImGui::PopID();
}

void InspectorPanel::DismissTexturePicker()
{
    PruneTexturePickerRequests(true);
    m_texturePicker.reset();
    m_texturePickerError.clear();
}

void InspectorPanel::PruneTexturePickerRequests(bool all)
{
    for(auto request = m_texturePickerRequests.begin(); request != m_texturePickerRequests.end();)
    {
        if(!all && request->second.visible)
        {
            ++request;
            continue;
        }

        if(m_thumbnailRenderer && request->second.ticket)
        {
            m_thumbnailRenderer->GetTextureThumbnailCache().Release(request->second.ticket,
                TextureThumbnailCache::Consumer::MaterialInspector);
        }

        request = m_texturePickerRequests.erase(request);
    }
}

bool InspectorPanel::TexturePickerMatches(TextureSlot slot, const ProjectPaths& paths,
    const ProjectAssetRegistry& registry) const
{
    if(!m_texturePicker || !m_record || !m_material || m_texturePicker->slot != slot ||
        m_texturePicker->generation != m_texturePickerGeneration ||
        m_texturePicker->project != paths.GetRoot() || m_projectRoot != paths.GetRoot())
    {
        return false;
    }

    const auto& opened = m_texturePicker->material;
    const auto* current = registry.Find(opened.id);
    return m_record->id == opened.id && m_record->relativePath == opened.relativePath &&
        current && current->type == AssetType::Material && current->relativePath == opened.relativePath;
}

std::optional<std::string>* InspectorPanel::GetTextureSlot(TextureSlot slot)
{
    if(!m_material)
    {
        return nullptr;
    }

    switch(slot)
    {
    case TextureSlot::BaseColor:
        return &m_material->baseColorTexture;
    case TextureSlot::Normal:
        return &m_material->normalTexture;
    case TextureSlot::Roughness:
        return &m_material->roughnessTexture;
    case TextureSlot::Metallic:
        return &m_material->metallicTexture;
    case TextureSlot::ARM:
        return &m_material->armTexture;
    case TextureSlot::AO:
        return &m_material->aoTexture;
    case TextureSlot::Height:
        return &m_material->heightTexture;
    }

    return nullptr;
}

bool InspectorPanel::ApplyTextureSelection(const AssetRecord* texture, const ProjectPaths& paths,
    const ProjectAssetRegistry& registry)
{
    if(!m_texturePicker || !TexturePickerMatches(m_texturePicker->slot, paths, registry) ||
        ProjectAssetResolver::Resolve(paths, registry, m_texturePicker->material.id).status !=
            AssetAvailability::Available)
    {
        m_texturePickerError = "Material changed or is unavailable. Reopen the texture picker.";
        m_status = m_texturePickerError;
        return false;
    }

    auto* reference = GetTextureSlot(m_texturePicker->slot);

    if(!reference)
    {
        m_texturePickerError = "The active texture slot is no longer available.";
        m_status = m_texturePickerError;
        return false;
    }

    if(texture)
    {
        const auto* current = registry.Find(texture->id);

        if(!current || current->type != AssetType::Texture || current->relativePath != texture->relativePath ||
            ProjectAssetResolver::Resolve(paths, registry, texture->id).status != AssetAvailability::Available)
        {
            m_texturePickerError = "The selected texture is no longer available. Refresh textures and retry.";
            m_status = m_texturePickerError;
            m_refreshTextureChoices = true;
            return false;
        }

        *reference = current->id.ToReference();
    }
    else
    {
        reference->reset();
    }

    // Draw's existing change detection schedules validation and the 400 ms autosave.
    ImGui::CloseCurrentPopup();
    DismissTexturePicker();
    return true;
}

void InspectorPanel::RefreshTextureChoices(const ProjectAssetRegistry& registry)
{
    const auto textures = registry.GetAssets(AssetType::Texture);
    const bool registryMatches = textures.size() == m_texturePickerRegistry.size() &&
        std::equal(textures.begin(), textures.end(), m_texturePickerRegistry.begin(),
            [](const AssetRecord& left, const AssetRecord& right)
            {
                return left.id == right.id && left.type == right.type && left.relativePath == right.relativePath;
            });

    if(!m_refreshTextureChoices && registryMatches)
    {
        return;
    }

    PruneTexturePickerRequests(true);
    m_textureChoices = textures;
    std::sort(m_textureChoices.begin(), m_textureChoices.end(),
        [](const AssetRecord& left, const AssetRecord& right)
        {
            return left.relativePath < right.relativePath;
        });
    m_texturePickerRegistry = textures;
    m_refreshTextureChoices = false;
}

namespace
{
    std::string TextureFilenameLabel(const std::string& filename, float width)
    {
        if(ImGui::CalcTextSize(filename.c_str()).x <= width)
        {
            return filename;
        }

        std::string label = filename;

        while(!label.empty() && ImGui::CalcTextSize((label + "...").c_str()).x > width)
        {
            // Remove a complete UTF-8 code point before adding the ellipsis.
            size_t last = label.size() - 1;

            while(last > 0 && (static_cast<unsigned char>(label[last]) & 0xc0) == 0x80)
            {
                --last;
            }

            label.resize(last);
        }

        return label + "...";
    }
}

bool InspectorPanel::DrawTextureTile(const AssetRecord& texture, float extent, bool selected,
    const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    const auto identity = texture.id.ToReference();
    const auto filename = texture.relativePath.filename().string();
    ImGui::PushID(identity.c_str());
    const auto start = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##TextureTile", ImVec2(extent, extent));
    const bool hovered = ImGui::IsItemHovered();
    std::string error;

    if(ImGui::IsItemVisible())
    {
        const ImVec2 end(start.x + extent, start.y + extent);
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(start, end, IM_COL32(55, 55, 55, 255));
        const TextureThumbnailCache::Preview* image = nullptr;

        if(m_thumbnailRenderer)
        {
            auto& cache = m_thumbnailRenderer->GetTextureThumbnailCache();
            const auto ticket = cache.Request(paths, registry, texture,
                TextureThumbnailCache::Consumer::MaterialInspector);
            auto& request = m_texturePickerRequests[identity];

            if(request.ticket && request.ticket != ticket)
            {
                cache.Release(request.ticket, TextureThumbnailCache::Consumer::MaterialInspector);
            }

            request.ticket = ticket;
            request.visible = true;
            image = cache.Query(ticket);

            if(image && !image->invalidated && image->error.empty() && image->texture.GetResource())
            {
                const float scale = (extent - 6.0f) / static_cast<float>((std::max)(image->width, image->height));
                const ImVec2 size(image->width * scale, image->height * scale);
                const ImVec2 position(start.x + (extent - size.x) * 0.5f, start.y + (extent - size.y) * 0.5f);
                draw->AddImage(static_cast<ImTextureID>(cache.GetHandle(*image).ptr), position,
                    ImVec2(position.x + size.x, position.y + size.y));
            }
            else
            {
                error = image ? image->error : std::string{};
                image = nullptr;
            }
        }

        if(!image)
        {
            draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(start.x + 6.0f, start.y + 6.0f),
                IM_COL32(190, 190, 190, 255), "Pending / unavailable", nullptr, extent - 12.0f);
        }

        if(selected || hovered)
        {
            draw->AddRect(start, end, selected ? IM_COL32(110, 175, 255, 255) : IM_COL32(150, 150, 150, 255),
                0.0f, 0, selected ? 2.0f : 1.0f);
        }
    }

    const auto label = TextureFilenameLabel(filename, extent);
    ImGui::TextUnformatted(label.c_str());

    if(hovered || ImGui::IsItemHovered())
    {
        const auto path = texture.relativePath.generic_string();
        ImGui::SetTooltip("%s%s%s", path.c_str(), error.empty() ? "" : "\n", error.c_str());
    }

    ImGui::PopID();
    return pressed;
}

void InspectorPanel::DrawTextureGrid(TextureSlot slot, const ProjectPaths& paths,
    const ProjectAssetRegistry& registry)
{
    const auto* reference = GetTextureSlot(slot);

    if(!reference || !ImGui::BeginTable("TextureGrid", 2, ImGuiTableFlags_SizingStretchSame))
    {
        return;
    }

    std::string search(m_textureSearch.data());
    const auto lowercase = [](unsigned char character) { return static_cast<char>(std::tolower(character)); };
    std::transform(search.begin(), search.end(), search.begin(), lowercase);
    size_t matches = 0;

    for(const auto& texture : m_textureChoices)
    {
        auto filtered = texture.relativePath.filename().string();
        std::transform(filtered.begin(), filtered.end(), filtered.begin(), lowercase);

        if(filtered.find(search) == std::string::npos)
        {
            continue;
        }

        ++matches;
        ImGui::TableNextColumn();
        const float extent = (std::max)(32.0f, (std::min)(120.0f, ImGui::GetContentRegionAvail().x));
        const bool selected = *reference && **reference == texture.id.ToReference();

        if(DrawTextureTile(texture, extent, selected, paths, registry) &&
            ApplyTextureSelection(&texture, paths, registry))
        {
            break;
        }
    }

    ImGui::EndTable();

    if(matches == 0)
    {
        ImGui::TextDisabled("No matching registered textures.");
    }
}

void InspectorPanel::DrawTexturePickerContents(TextureSlot slot,
    const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    ImGui::TextUnformatted("Select texture");
    ImGui::SameLine();

    if(ImGui::Button("Close"))
    {
        ImGui::CloseCurrentPopup();
        DismissTexturePicker();
        return;
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##Search", "Search filenames...", m_textureSearch.data(), m_textureSearch.size());

    if(ImGui::SmallButton("Refresh textures"))
    {
        if(m_thumbnailRenderer)
        {
            auto& cache = m_thumbnailRenderer->GetTextureThumbnailCache();

            for(const auto& texture : registry.GetAssets(AssetType::Texture))
            {
                cache.Invalidate((paths.GetRoot() / texture.relativePath).lexically_normal());
            }
        }

        ++m_previewTextureRefresh;
        m_refreshTextureChoices = true;
        m_texturePickerError.clear();
    }

    ImGui::SameLine();
    const auto* reference = GetTextureSlot(slot);

    if(ImGui::Selectable("None", reference && !*reference, ImGuiSelectableFlags_DontClosePopups, ImVec2(80.0f, 0.0f)) &&
        ApplyTextureSelection(nullptr, paths, registry))
    {
        return;
    }

    if(!m_texturePickerError.empty())
    {
        ImGui::TextWrapped("%s", m_texturePickerError.c_str());
    }

    RefreshTextureChoices(registry);

    for(auto& [identity, request] : m_texturePickerRequests)
    {
        request.visible = false;
    }

    ImGui::Separator();

    if(ImGui::BeginChild("TextureList", ImVec2(0.0f, 0.0f)))
    {
        DrawTextureGrid(slot, paths, registry);
    }

    ImGui::EndChild();
    PruneTexturePickerRequests();
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
    DismissTexturePicker();
    ++m_texturePickerGeneration;

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
