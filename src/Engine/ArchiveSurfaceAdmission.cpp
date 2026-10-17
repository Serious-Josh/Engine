#include "Engine/ArchiveSurfaceAdmission.h"
#include "Renderer/Renderer.h"
#include "Scene/FileSceneAssetCatalog.h"
#include "Scene/FileSceneRuntime.h"
#include "Scene/SceneMaterialBindingPublication.h"
#include "assets/CookedScene.h"
#include <chrono>
#include <set>
#include <thread>
#include <stdexcept>
#include <cstring>

namespace
{
void RequireArchiveAdmission(bool condition, const std::string& error)
{
    if (!condition)
    {
        throw std::runtime_error(error);
    }
}

std::set<std::string> CollectReferencedMaterialIDs(const CookedScene::ArchiveScene& source)
{
    std::set<std::string> materialIDs;
    for (const auto& object : source.metadata.document.objects)
    {
        if (auto id = AssetID::ParseReference(object.materialReference))
        {
            materialIDs.insert(id->ToString());
        }
        for (const auto& [slot, reference] : object.materialSlotBindings)
        {
            materialIDs.insert(AssetID::ParseReference(reference)->ToString());
        }
    }
    return materialIDs;
}

void ValidateAdmittedMaterial(const Material& material,
                              const ProjectMaterialPreparationService::Candidate& preparedMaterial)
{
    RequireArchiveAdmission(material.model == preparedMaterial.parameters.model &&
                                material.roughness == preparedMaterial.parameters.roughness,
                            "Archived material parameters differ from admitted material.");
    const Texture* admittedTextures[] = {
        material.texture,    material.normalTexture, material.roughnessTexture, material.metallicTexture,
        material.armTexture, material.aoTexture,     material.heightTexture};
    for (size_t slot = 0; slot < 7; ++slot)
    {
        if (preparedMaterial.textures[slot])
        {
            RequireArchiveAdmission(admittedTextures[slot] && admittedTextures[slot]->GetResource(),
                                    "Archive texture resource missing after asynchronous upload.");
            const auto textureDescription = admittedTextures[slot]->GetResource()->GetDesc();
            const auto& preparedTexture = *preparedMaterial.textures[slot]->prepared;
            RequireArchiveAdmission(textureDescription.Width == preparedTexture.width &&
                                        textureDescription.Height == preparedTexture.height &&
                                        textureDescription.MipLevels == preparedTexture.subresources.size() &&
                                        textureDescription.Format == preparedTexture.resourceFormat,
                                    "Archived texture dimensions/mips/format differ after upload.");
        }
    }
}

void ValidateSharedArchiveTextures(
    Renderer& renderer, FileSceneAssetCatalog& catalog, const std::string& materialIDText,
    std::map<std::pair<std::string, ProjectTextureInterpretation>, const Texture*>& sharedTextures)
{
    auto* material = catalog.FindPublishedMaterial("asset:" + materialIDText, renderer);
    RequireArchiveAdmission(material != nullptr, "Admitted archive material missing.");
    const auto archiveProvider = catalog.ArchiveSource()->provider;
    auto materialBytes =
        archiveProvider->ReadEntry(*AssetID::Parse(materialIDText), EngineAssetPak::Type::Material,
                                   CookedSurface::Version, CookedSurface::MaterialLimit);
    const auto decodedMaterial = CookedSurface::DecodeMaterial(materialBytes);
    const auto textureReferences = CookedSurface::References(decodedMaterial.parameters);
    const Texture* materialTextures[] = {
        material->texture,    material->normalTexture, material->roughnessTexture, material->metallicTexture,
        material->armTexture, material->aoTexture,     material->heightTexture};
    for (size_t slot = 0; slot < 7; ++slot)
    {
        if (textureReferences[slot])
        {
            RequireArchiveAdmission(materialTextures[slot] && materialTextures[slot]->GetResource(),
                                    "Archived texture did not reach GPU admission.");
            auto [existingTexture, insertedTexture] = sharedTextures.emplace(
                std::make_pair(*textureReferences[slot], CookedSurface::Interpretation(slot)),
                materialTextures[slot]);
            RequireArchiveAdmission(insertedTexture || existingTexture->second == materialTextures[slot],
                                    "Shared archive texture uploaded more than once.");
        }
    }
}
} // namespace

void ArchiveSurfaceAdmission::Admit(Renderer& renderer, ProjectMaterialPreparationService& preparation,
                                    FileSceneAssetCatalog& catalog, FileSceneRuntime& runtime,
                                    const std::function<void()>& checkCancellation)
{
    const auto source = catalog.ArchiveSource();
    if (!source)
    {
        throw std::runtime_error("Missing archive surface source.");
    }

    const auto materialIDs = CollectReferencedMaterialIDs(*source);

    // Renderer retains recorded/submitted uploads after failure; no local abort or release.
    unsigned progressFrame = 0;
    const auto submitUploadAndTableProgress = [&]
    {
        const auto frameSlot = progressFrame++ % CommandContext::FrameCount;
        renderer.SubmitArchiveMaterialProgress(frameSlot);

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        renderer.PollPreparedTextureUploads();
    };

    const auto originalMaterialRecords = renderer.m_materialPublication.desired;
    const auto sceneResourceLifetime = renderer.m_sceneResourceLifetime;

    for (const auto& materialIDText : materialIDs)
    {
        if (checkCancellation)
        {
            checkCancellation();
        }
        const auto id = *AssetID::Parse(materialIDText);
        std::uint64_t preparationTicket = 0;
        std::string error;
        RequireArchiveAdmission(preparation.RequestArchive(source->provider, id, runtime.GetRuntimeIdentity(),
                                                           preparationTicket, error) ==
                                    ProjectMaterialPreparationService::Status::Pending,
                                error);

        // Cancel CPU preparation on this material scope exit, including exceptions.
        // This guard does not release Renderer-owned GPU upload resources.
        struct CancelPreparationRequest
        {
            ProjectMaterialPreparationService& service;
            std::uint64_t preparationTicket;
            ~CancelPreparationRequest()
            {
                service.Cancel(preparationTicket);
            }
        } cancelPreparationRequest{preparation, preparationTicket};

        std::shared_ptr<const ProjectMaterialPreparationService::Candidate> preparedMaterial;
        const auto materialDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!preparedMaterial && std::chrono::steady_clock::now() < materialDeadline)
        {
            if (checkCancellation)
            {
                checkCancellation();
            }
            auto preparationResult = preparation.PollArchive(preparationTicket, runtime.GetRuntimeIdentity());
            RequireArchiveAdmission(
                preparationResult.status == ProjectMaterialPreparationService::Status::Pending ||
                    preparationResult.status == ProjectMaterialPreparationService::Status::Retry ||
                    preparationResult.status == ProjectMaterialPreparationService::Status::Ready,
                preparationResult.message);
            preparedMaterial = preparationResult.candidate;
            if (!preparedMaterial)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        RequireArchiveAdmission(bool(preparedMaterial), "Archive material preparation timed out.");

        // Individual material admission may queue uploads; progress uses the same deadline.
        MaterialAdmissionResult admissionResult;
        do
        {
            if (checkCancellation)
            {
                checkCancellation();
            }
            admissionResult =
                renderer.AdvanceArchiveMaterialAdmission(preparation, preparedMaterial, catalog, runtime);
            RequireArchiveAdmission(admissionResult.status == MaterialAdmissionStatus::Pending ||
                                        admissionResult.status == MaterialAdmissionStatus::Retry ||
                                        admissionResult.status == MaterialAdmissionStatus::Ready,
                                    admissionResult.message);
            if (admissionResult.status != MaterialAdmissionStatus::Ready)
            {
                submitUploadAndTableProgress();
            }
        } while (admissionResult.status != MaterialAdmissionStatus::Ready &&
                 std::chrono::steady_clock::now() < materialDeadline);
        RequireArchiveAdmission(admissionResult.status == MaterialAdmissionStatus::Ready &&
                                    admissionResult.material &&
                                    renderer.HasPublishedMaterial(*admissionResult.material),
                                "Archive material admission timed out.");
        ValidateAdmittedMaterial(*admissionResult.material, *preparedMaterial);

        // Frame slots adopt appended tables using their existing safe-reuse protocol.
        for (size_t slot = 0; slot < CommandContext::FrameCount; ++slot)
        {
            submitUploadAndTableProgress();
        }
        RequireArchiveAdmission(
            renderer.m_materialPublication.desired.size() >= originalMaterialRecords.size() &&
                std::memcmp(originalMaterialRecords.data(), renderer.m_materialPublication.desired.data(),
                            originalMaterialRecords.size() * sizeof(GPUMaterial)) == 0,
            "Existing GPU material indices/data changed.");
    }

    RequireArchiveAdmission(renderer.m_sceneResourceLifetime == sceneResourceLifetime,
                            "Material admission rebuilt scene resources.");
    // Shared prepared dependencies must resolve to one catalog GPU texture.
    std::map<std::pair<std::string, ProjectTextureInterpretation>, const Texture*> sharedTextures;

    for (const auto& materialIDText : materialIDs)
    {
        if (checkCancellation)
        {
            checkCancellation();
        }
        ValidateSharedArchiveTextures(renderer, catalog, materialIDText, sharedTextures);
    }

    // Bind objects individually after admission; earlier commits are not rolled back.
    SceneMaterialBindingPublication deferredBindings;
    for (size_t index = 0; index < source->metadata.document.objects.size(); ++index)
    {
        if (!runtime.GetScene().GetRenderObjects()[index].materialsDeferred)
        {
            continue;
        }
        auto currentObject = runtime.GetObjectMetadata(index);
        RequireArchiveAdmission(bool(currentObject), "Archive deferred object lost identity.");
        auto bindingPreparation =
            deferredBindings.PrepareDeferred(renderer, runtime, catalog, *currentObject);
        RequireArchiveAdmission(bindingPreparation.status == SceneMaterialBindingPublication::Status::Ready,
                                bindingPreparation.message);
        auto bindingPublication = deferredBindings.PublishAtBoundary(renderer, runtime, catalog);
        RequireArchiveAdmission(
            bindingPublication.published && !runtime.GetScene().GetRenderObjects()[index].materialsDeferred,
            "Authored archive materials were not atomically bound: " + bindingPublication.message);
    }
}
