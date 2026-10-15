#include "assets/CookedScene.h"
#include "Scene/FileSceneAssetCatalog.h"
#include "Scene/MeshResidencyPublication.h"
#include "assets/MeshCookValidation.h"

#include <exception>

namespace
{
    using Status = ProjectMeshPreparationService::Status;
    using Result = FileSceneAssetCatalog::CookedPreparationResult;
    using Failure = Result::Failure;

    Result Fail(Status status, Failure failure, std::string error)
    {
        Result result;
        result.status = status;
        result.failure = failure;
        result.error = std::move(error);
        return result;
    }
}

bool ValidateCookedMeshResidencyEvidence(const CookedMeshResidencyEvidence& evidence,
    std::string_view reference, std::string* error)
{
    const auto reject = [&](std::string message)
    {
        if(error) { *error = std::move(message); }
        return false;
    };

    const auto id = AssetID::ParseReference(reference);

    if(!id || id->ToReference() != reference || !evidence.provider || !evidence.asset || !evidence.metadata ||
        !evidence.current || !evidence.current->load() || *id != evidence.asset->assetID ||
        evidence.asset->assetType != AssetType::Mesh || evidence.provider->Mapping() != evidence.asset->mapping ||
        evidence.metadata->evidence != evidence.asset->payload ||
        evidence.asset->payload.formatVersion != CookedMesh::V1::Version ||
        evidence.asset->payload.cookCompatibility != CookedMesh::V1::CookCompatibility)
    {
        return reject("Cooked residency identity, compatibility, cancellation or metadata evidence is invalid.");
    }

    const auto current = evidence.provider->CheckCurrent();
    if(current.status != CookedMeshSource::Status::Success) { return reject(std::string(current.message)); }
    const auto lookup = evidence.provider->Lookup(*id);

    if(lookup.status != CookedMeshSource::Status::Success || *lookup.evidence != *evidence.asset)
    {
        return reject("Cooked residency AssetID/payload mapping changed.");
    }

    return true;
}

FileSceneAssetCatalog::CookedPreparationResult FileSceneAssetCatalog::RetainPreparedCookedMetadata(
    std::string_view reference, const ProjectMeshPreparationService::Completion& completion,
    const ProjectMeshPreparationService& preparation)
{
    try
    {
        if(!m_initialized || (!m_registry && !m_archiveScene))
        {
            return Fail(Status::Invalid, Failure::Binding, "Cooked metadata requires an initialized project catalog.");
        }

        if(completion.status != Status::Available)
        {
            return Fail(completion.status, Failure::Completion, completion.error);
        }

        std::string error;
        const auto validity = preparation.ValidateCookedCompletion(completion, error);
        if(validity != Status::Available) { return Fail(validity, Failure::Completion, std::move(error)); }
        const auto id = AssetID::ParseReference(reference);
        const auto& identity = completion.identity;
        const auto* record = id && m_registry ? m_registry->Find(*id) : nullptr;

        if(!id || id->ToReference() != reference || *id != identity.cooked->assetID ||
            (m_archiveScene ? (completion.provider != m_archiveScene->provider ||
                !ValidateArchiveMesh(reference, identity.cooked.get(), completion.metadata.get(), &error)) :
                (!record || record->type != AssetType::Mesh || std::filesystem::canonical(m_projectRoot).generic_string() != identity.cooked->mapping.projectNamespace)))
        {
            return Fail(Status::Invalid, Failure::Binding, "Cooked completion belongs to a different project, AssetID or type.");
        }

        if(!completion.metadata || completion.metadata->evidence != identity.cooked->payload ||
            !ValidateMeshMetadata(completion.metadata->metadata, completion.metadata->indexCount, error))
        {
            return Fail(Status::Corrupt, Failure::Metadata, "Cooked slot metadata/evidence is invalid: " + error);
        }

        if(completion.geometry && (completion.geometry->metadata != completion.metadata->metadata ||
            completion.geometry->vertices.size() != completion.metadata->vertexCount ||
            completion.geometry->indices.size() != completion.metadata->indexCount ||
            !MeshCookValidation::Same(completion.geometry->boundsMin, completion.metadata->boundsMin) ||
            !MeshCookValidation::Same(completion.geometry->boundsMax, completion.metadata->boundsMax)))
        {
            return Fail(Status::Corrupt, Failure::Metadata, "Cooked full geometry differs from retained CPU metadata.");
        }

        const auto key = identity.CacheKey();
        const auto existing = m_cookedMetadata.find(key);

        if(existing != m_cookedMetadata.end() && *existing->second->asset != *identity.cooked)
        {
            return Fail(Status::Stale, Failure::Completion, "Cooked mapping changed; reload the catalog rather than replacing evidence.");
        }

        auto evidence = std::make_shared<CookedMeshResidencyEvidence>();
        evidence->provider = completion.provider;
        evidence->asset = identity.cooked;
        evidence->metadata = completion.metadata;
        evidence->current = completion.current;

        if(!ValidateCookedMeshResidencyEvidence(*evidence, reference, &error))
        {
            return Fail(Status::Stale, Failure::Completion, std::move(error));
        }

        m_cookedMetadata.insert_or_assign(key, evidence);
        Result result;
        result.status = Status::Available;
        result.evidence = std::move(evidence);
        return result;
    }
    catch(const std::bad_alloc&) { return Fail(Status::AllocationFailure, Failure::Metadata, "Allocation"); }
    catch(const std::filesystem::filesystem_error& error) { return Fail(Status::IOFailure, Failure::Binding, error.what()); }
    catch(const std::exception& error) { return Fail(Status::Invalid, Failure::Binding, error.what()); }
}

FileSceneAssetCatalog::CookedPreparationResult FileSceneAssetCatalog::ResolvePreparedCookedMesh(
    std::string_view reference, const ProjectMeshPreparationService::Completion& completion,
    const ProjectMeshPreparationService& preparation, const Renderer& renderer, const FileSceneRuntime& runtime)
{
    if(completion.status != Status::Available) { return Fail(completion.status, Failure::Completion, completion.error); }
    if(completion.identity.mode != ProjectMeshPreparationService::PreparationMode::Geometry || !completion.geometry)
    {
        return Fail(Status::Invalid, Failure::Completion, "Cooked GPU preparation requires a full CPU geometry completion.");
    }

    if(!MeshResidencyPublication::CanPrepareGeometry(renderer, runtime))
    {
        return Fail(Status::Retry, Failure::Binding, "Cooked GPU preparation requires the active runtime and idle checked renderer boundary.");
    }

    auto result = RetainPreparedCookedMetadata(reference, completion, preparation);
    if(result.status != Status::Available) { return result; }

    try
    {
        const auto key = completion.identity.CacheKey();
        auto existing = m_projectMeshes.find(key);

        if(existing != m_projectMeshes.end())
        {
            if(!existing->second->cooked || *existing->second->cooked->asset != *result.evidence->asset)
            {
                return Fail(Status::Stale, Failure::Completion, "Cooked catalog identity changed; reload before preparing another mapping.");
            }

            // Identical payload AND mapping: reuse GPU allocation, refresh the
            // request cancellation evidence. Older receipts become invalid.
            existing->second->cooked = result.evidence;
            result.mesh = &existing->second->mesh;
        }
        else
        {
            for(const auto& [retainedKey, asset] : m_projectMeshes)
            {
                if(std::get<5>(retainedKey) == MeshPreparationSourceKind::Cooked &&
                    std::get<0>(retainedKey) == std::get<0>(key) && std::get<1>(retainedKey) == std::get<1>(key))
                {
                    return Fail(Status::Stale, Failure::Completion, "Cooked payload changed; retained GPU geometry requires catalog reload.");
                }
            }

            result.mesh = CreateProjectMesh(key, completion.geometry, result.evidence);
            if(!result.mesh) { return Fail(Status::Failed, Failure::GPU, m_meshError); }
        }

        // Upload completion is proved by the existing synchronous uploader.
        // A changed generation never substitutes bytes into this completion.
        std::string error;
        const auto validity = preparation.ValidateCookedCompletion(completion, error);
        if(validity != Status::Available) { return Fail(validity, Failure::Completion, std::move(error)); }
        result.candidate = DescribeMeshResidencyCandidate(reference, *result.mesh, &error);

        if(!result.candidate) { return Fail(Status::Stale, Failure::Completion, std::move(error)); }
        return result;
    }
    catch(const std::bad_alloc&) { return Fail(Status::AllocationFailure, Failure::GPU, "Allocation"); }
    catch(const std::exception& error) { return Fail(Status::Failed, Failure::GPU, error.what()); }
}
