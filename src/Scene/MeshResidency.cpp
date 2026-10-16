#include "assets/CookedSurface.h"
#include "Scene/MeshResidency.h"
#include "Scene/FileSceneRuntime.h"
#include "Scene/FileSceneAssetCatalog.h"
#include "Renderer/Preview/MaterialThumbnailValidation.h"
#include "assets/ProjectAssetResolver.h"

#include <Windows.h>
#include <algorithm>
#include <exception>
#include <limits>
#include <type_traits>

namespace
{
    bool Fail(std::string* error, const std::string& message)
    {
        if(error)
        {
            *error = message;
        }

        return false;
    }
}

MeshResidencySourceLock::~MeshResidencySourceLock()
{
    if(m_handle)
    {
        CloseHandle(m_handle);
    }
}

std::optional<MeshResidencyCandidate> FileSceneAssetCatalog::DescribeMeshResidencyCandidate(
    std::string_view reference, const Mesh& mesh, std::string* error) const
{
    if (ClassifyMeshReference(reference) == MeshReferenceKind::Builtin && &mesh == &m_sphereMesh)
    {
        MeshResidencyCandidate candidate;
        // The receipt exposes the catalog's existing mutable runtime borrow;
        // this const observation never changes the primitive.
        candidate.mesh = const_cast<Mesh*>(&m_sphereMesh);
        candidate.catalogIdentity = m_catalogIdentity;
        candidate.vertexAddress = mesh.GetVertexBufferAddress();

        candidate.indexAddress = mesh.GetIndexBufferAddress();
        candidate.sourceKind = MeshResidencyCandidate::SourceKind::Builtin;
        candidate.builtinReference = reference;
        auto source = ValidateMeshResidencyCandidate(candidate, reference, error);

        if (!source)
        {
            return std::nullopt;
        }

        return candidate;
    }

    for(const auto& [key, asset] : m_projectMeshes)
    {
        if(&asset->mesh != &mesh)
        {
            continue;
        }

        MeshResidencyCandidate candidate{key, &asset->mesh, m_catalogIdentity,
            mesh.GetVertexBufferAddress(), mesh.GetIndexBufferAddress()};
        if(asset->cooked)
        {
            candidate.sourceKind = MeshResidencyCandidate::SourceKind::Cooked;
            candidate.cooked = asset->cooked;
        }
        auto source = ValidateMeshResidencyCandidate(candidate, reference, error);

        if(!source)
        {
            return std::nullopt;
        }

        return candidate;
    }

    Fail(error, "Residency candidate is not a prepared supported mesh owned by this catalog.");
    return std::nullopt;
}

MeshResidencySourceLock FileSceneAssetCatalog::ValidateMeshResidencyCandidate(
    const MeshResidencyCandidate& candidate, std::string_view reference, std::string* error) const
{
    if(error)
    {
        error->clear();
    }

    const auto reject = [&](const std::string& message)
    {
        Fail(error, message);
        return MeshResidencySourceLock{};
    };

    if(!m_initialized || !candidate.catalogIdentity || candidate.catalogIdentity != m_catalogIdentity)
    {
        return reject("Residency candidate catalog lifetime is stale.");
    }

    if(m_archiveScene && candidate.sourceKind != MeshResidencyCandidate::SourceKind::Cooked)
        return reject("Archive catalogs accept only verified cooked candidates; no loose or builtin mesh fallback.");
    // Prove catalog ownership before dereferencing any copied mesh borrow.
    const Mesh* ownedMesh = nullptr;
    const ImportedMesh* ownedGeometry = nullptr;
    const bool builtin = candidate.sourceKind == MeshResidencyCandidate::SourceKind::Builtin;
    const bool cooked = candidate.sourceKind == MeshResidencyCandidate::SourceKind::Cooked;

    if (builtin)
    {
        if (ClassifyMeshReference(reference) != MeshReferenceKind::Builtin ||
            candidate.builtinReference != reference || candidate.mesh != &m_sphereMesh ||
            candidate.key != ProjectMeshCPUCache::Key{} || candidate.cooked)
        {
            return reject("Built-in residency reference or catalog geometry identity changed.");
        }

        ownedMesh = &m_sphereMesh;
        ownedGeometry = &m_sphereGeometry;
    }
    else
    {
        const auto owned = m_projectMeshes.find(candidate.key);

        if ((!cooked && candidate.sourceKind != MeshResidencyCandidate::SourceKind::ProjectAsset) ||
            !candidate.builtinReference.empty() || (!m_registry && !m_archiveScene) || owned == m_projectMeshes.end() ||
            candidate.mesh != &owned->second->mesh || !owned->second->published || !owned->second->geometry)
        {
            return reject("Residency candidate is not retained and upload-complete in this catalog.");
        }

        const auto id = AssetID::ParseReference(reference);

        if (!id || id->ToReference() != reference || id->ToString() != std::get<1>(candidate.key))
        {
            return reject("Residency candidate authored AssetID does not match.");
        }

        if(cooked)
        {
            if(!candidate.cooked || candidate.cooked != owned->second->cooked)
            {
                return reject("Cooked candidate evidence is not the catalog's retained preparation receipt.");
            }

            std::string validation;
            if(!ValidateCookedMeshResidencyEvidence(*candidate.cooked, reference, &validation)) { return reject(validation); }
            ProjectMeshPreparationService::Identity identity;
            identity.sourceKind = MeshPreparationSourceKind::Cooked;
            identity.cooked = candidate.cooked->asset;

            if(candidate.key != identity.CacheKey())
            {
                return reject("Cooked candidate cache/payload compatibility identity differs.");
            }
        }
        else if(candidate.cooked || owned->second->cooked || std::get<5>(candidate.key) != MeshPreparationSourceKind::FBX ||
            std::get<3>(candidate.key) != ModelImporter::kImportRevision || std::get<4>(candidate.key) != MeshUVOrientation::FlipV)
        {
            return reject("FBX residency candidate import/source policy does not match.");
        }

        ownedMesh = &owned->second->mesh;
        ownedGeometry = owned->second->geometry.get();
    }

    const auto& mesh = *ownedMesh;
    const auto& geometry = *ownedGeometry;
    std::string metadataError;

    if(!mesh.GetVertexBufferResource() || !mesh.GetIndexBufferResource() || !mesh.IsIndexed() ||
        !candidate.vertexAddress || !candidate.indexAddress ||
        candidate.vertexAddress != mesh.GetVertexBufferAddress() || candidate.indexAddress != mesh.GetIndexBufferAddress() ||
        mesh.GetVertexBufferDescriptorIndex() == UINT_MAX || mesh.GetIndexBufferDescriptorIndex() == UINT_MAX ||
        mesh.GetVertexStride() != sizeof(Vertex) || mesh.GetIndexFormat() != DXGI_FORMAT_R32_UINT ||
        mesh.GetVertexCount() != geometry.vertices.size() || mesh.GetIndexCount() != geometry.indices.size() ||
        !mesh.GetBounds().valid || mesh.GetMetadata() != geometry.metadata ||
        !ValidateMeshMetadata(mesh.GetMetadata(), mesh.GetIndexCount(), metadataError))
    {
        return reject("Residency candidate geometry/metadata is incomplete or changed: " + metadataError);
    }

    if (builtin)
    {
        // Immutable primitive geometry is catalog-owned for this lifetime. There
        // are no source bytes to lock/hash, and no synthetic Windows handle.
        MeshResidencySourceLock source;
        source.m_builtinValidated = true;
        return source;
    }

    if(cooked)
    {
        const auto& metadata = *candidate.cooked->metadata;
        const auto id = AssetID::ParseReference(reference);
        const auto* record = m_registry ? m_registry->Find(*id) : nullptr;

        if((m_archiveScene ? !ValidateArchiveMesh(reference,candidate.cooked->asset.get(),&metadata,error) : (!record || record->type != AssetType::Mesh)) || metadata.metadata != geometry.metadata ||
            metadata.vertexCount != geometry.vertices.size() || metadata.indexCount != geometry.indices.size() ||
            metadata.boundsMin.x != geometry.boundsMin.x || metadata.boundsMin.y != geometry.boundsMin.y ||
            metadata.boundsMin.z != geometry.boundsMin.z || metadata.boundsMax.x != geometry.boundsMax.x ||
            metadata.boundsMax.y != geometry.boundsMax.y || metadata.boundsMax.z != geometry.boundsMax.z)
        {
            return reject("Cooked catalog geometry, registration or CPU metadata is inconsistent.");
        }

        // Repeat current mapping acceptance under write/delete exclusion. The
        // coordinator holds this lease across its no-fail ownership transfer.
        MeshResidencySourceLock source;
        source.m_cookedPreparation = candidate.cooked;
        const auto acceptance = candidate.cooked->provider->PinAcceptance(*candidate.cooked->asset);
        if(acceptance.status != CookedMeshSource::Status::Success)
        {
            return reject(std::string(acceptance.message));
        }
        source.m_cookedAcceptance = acceptance.lease;
        return source;
    }

    try
    {
        const auto id = AssetID::ParseReference(reference);
        const auto project = std::filesystem::canonical(m_projectRoot);
        const auto* record = m_registry ? m_registry->Find(*id) : nullptr;
        const auto resolved = ProjectAssetResolver::Resolve(ProjectPaths(project), *m_registry, *id);

        if(project != std::get<0>(candidate.key) || !record || record->type != AssetType::Mesh ||
            resolved.status != AssetAvailability::Available ||
            _wcsicmp(resolved.resolvedPath.extension().c_str(), L".fbx") != 0)
        {
            return reject("Residency candidate project, registration, type or contained source changed.");
        }

        HANDLE handle = CreateFileW(resolved.resolvedPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

        if(handle == INVALID_HANDLE_VALUE)
        {
            return reject("Cannot lock residency source for freshness verification.");
        }

        MeshResidencySourceLock source(handle);
        MaterialThumbnailValidation validation;

        if(validation.Digest(resolved.resolvedPath) != std::get<2>(candidate.key))
        {
            return reject("Residency candidate source content changed; prepare a new candidate.");
        }

        return source;
    }
    catch(const std::exception& exception)
    {
        return reject("Residency source validation failed: " + std::string(exception.what()));
    }
}

Material* FileSceneAssetCatalog::ResolveResidencyMaterial(std::string_view reference, Material* retained, std::string* error)
{
    const auto effective = reference.empty() ? std::string_view("builtin:default") : reference;
    Material* resolved = ResolveMaterial(effective); // Existing type/containment and fallback policy.

    if(!resolved)
    {
        Fail(error, GetLastMaterialError());
        return nullptr;
    }

    if(!retained || retained == resolved)
    {
        return resolved;
    }

    const auto id = AssetID::ParseReference(effective);

    // Preserve a currently assigned fresh version, rather than reverting the
    // user's assignment to the ordinary material cache entry for the same AssetID.
    for(const auto& version : m_materialVersions)
    {
        if(id && version->assetID == id && version->projectIdentity == std::filesystem::canonical(m_projectRoot) &&
            &version->material == retained)
        {
            return retained;
        }
    }

    Fail(error, "Current retained material does not belong to its authored asset reference.");
    return nullptr;
}

std::optional<MeshResidencyToken> FileSceneRuntime::PrepareMeshResidency(const SceneObjectMetadata& selection,
    FileSceneAssetCatalog& catalog, const MeshResidencyCandidate& candidate, std::string* error)
{
    if(error)
    {
        error->clear();
    }

    if(m_meshResidency || m_materialAssignment || HasPreparedScene() ||
        m_nextMeshResidency == (std::numeric_limits<std::uint64_t>::max)())
    {
        Fail(error, "Residency preparation requires no outstanding residency, scene preparation or material assignment.");
        return std::nullopt;
    }

    const auto current = GetObjectMetadata(selection.objectID, error);

    if(!current || current->runtimeIdentity != selection.runtimeIdentity || current->sceneID != selection.sceneID ||
        current->structureRevision != selection.structureRevision || current->runtimeIndex != selection.runtimeIndex ||
        current->meshReference != selection.meshReference || current->bindingRevision != selection.bindingRevision ||
        current->materialReference != selection.materialReference || current->materialSlotBindings != selection.materialSlotBindings)
    {
        Fail(error, "Residency object session, authored assignment or binding revision is stale.");
        return std::nullopt;
    }

    const auto& object = m_scene.GetRenderObjects()[current->runtimeIndex];

    if(object.materialsDeferred)
    {
        Fail(error,"Packaged material references are deferred; mesh publication cannot substitute loose/default materials.");
        return std::nullopt;
    }

    if(current->GetMeshResidency(object) != SceneMeshResidency::Pending)
    {
        Fail(error, "Residency preparation requires a nonempty authored mesh assignment with no resident geometry.");
        return std::nullopt;
    }

    auto source = catalog.ValidateMeshResidencyCandidate(candidate, current->meshReference, error);

    if(!source)
    {
        return std::nullopt;
    }

    const auto& slots = candidate.mesh->GetMetadata().materialSlots;

    for(const auto& [slotID, reference] : current->materialSlotBindings)
    {
        const bool exists = std::any_of(slots.begin(), slots.end(), [&](const auto& slot) { return slot.id == slotID; });

        if(!exists)
        {
            Fail(error, "Authored material-slot override is unknown to the arriving mesh: " + slotID);
            return std::nullopt;
        }
    }

    MeshResidencyPreparation prepared;
    prepared.selection = *current;
    prepared.candidate = candidate;
    prepared.catalog = &catalog;

    prepared.fallback = catalog.ResolveResidencyMaterial(current->materialReference, object.material, error);

    if(!prepared.fallback)
    {
        return std::nullopt;
    }

    for(const auto& slot : slots)
    {
        Material* material = prepared.fallback;
        const auto authored = current->materialSlotBindings.find(slot.id);

        if(authored != current->materialSlotBindings.end())
        {
            const auto retained = object.slotMaterials.find(slot.id);
            material = catalog.ResolveResidencyMaterial(authored->second,
                retained == object.slotMaterials.end() ? nullptr : retained->second, error);

            if(!material)
            {
                return std::nullopt;
            }
        }

        prepared.runtimeSlots.emplace(slot.id, material);
    }

    prepared.committedSlots = prepared.runtimeSlots;
    prepared.originalSlots = object.slotMaterials;
    if(candidate.sourceKind == MeshResidencyCandidate::SourceKind::Cooked &&
        !catalog.ValidateMeshResidencyCandidate(candidate, current->meshReference, error))
    {
        // Recheck after fallible material work; never substitute a new mapping.
        return std::nullopt;
    }

    prepared.token = {m_instanceIdentity, m_nextMeshResidency, current->bindingRevision};
    const auto token = prepared.token;
    // Map moves may allocate sentinel storage here, before any scene mutation.
    m_meshResidency.emplace(std::move(prepared));
    ++m_nextMeshResidency;
    return token;
}

std::optional<SceneObjectMetadata> FileSceneRuntime::ValidatePreparedMeshResidencyIdentity(const MeshResidencyToken& token, const FileSceneAssetCatalog& catalog, std::string* error) const
{
    if(error)
    {
        error->clear();
    }

    if(!m_meshResidency || m_meshResidency->token != token || token.runtimeIdentity != m_instanceIdentity ||
        m_materialAssignment || HasPreparedScene())
    {
        Fail(error, "Residency commit token is stale or another scene transaction is outstanding.");
        return {};
    }

    const auto& prepared = *m_meshResidency;
    const auto current = GetObjectMetadata(prepared.selection.objectID, error);

    if(!current || prepared.catalog != &catalog || current->sceneID != prepared.selection.sceneID ||
        current->runtimeIdentity != token.runtimeIdentity || current->structureRevision != prepared.selection.structureRevision ||
        current->runtimeIndex != prepared.selection.runtimeIndex || current->bindingRevision != token.bindingRevision ||
        current->meshReference != prepared.selection.meshReference ||
        current->materialReference != prepared.selection.materialReference ||
        current->materialSlotBindings != prepared.selection.materialSlotBindings)
    {
        Fail(error, "Residency object, catalog, authored reference or binding revision changed before publication.");
        return {};
    }

    const auto& object = m_scene.GetRenderObjects()[current->runtimeIndex];

    if(current->GetMeshResidency(object) != SceneMeshResidency::Pending || object.material != prepared.fallback ||
        object.slotMaterials != prepared.originalSlots ||
        current->bindingRevision == (std::numeric_limits<std::uint64_t>::max)() ||
        m_residencyRevision == (std::numeric_limits<std::uint64_t>::max)())
    {
        Fail(error, "Residency pending bindings changed or revision identities are exhausted.");
        return {};
    }

    return current;
}

MeshResidencySourceLock FileSceneRuntime::ValidatePreparedMeshResidency(const MeshResidencyToken& token,
    FileSceneAssetCatalog& catalog, std::string* error) const
{
    const auto current = ValidatePreparedMeshResidencyIdentity(token, catalog, error);

    if (!current)
    {
        return {};
    }

    const auto& prepared = *m_meshResidency;
    if(prepared.candidate.sourceKind == MeshResidencyCandidate::SourceKind::Cooked && !m_cookedPublicationAuthorized)
    {
        Fail(error, "Cooked residency publication requires Engine archive bootstrap authorization.");
        return {};
    }

    auto source = catalog.ValidateMeshResidencyCandidate(prepared.candidate, current->meshReference, error);

    if(!source || !catalog.ValidateRetainedResidencyMaterial(current->materialReference, prepared.fallback, error))
    {
        return {};
    }

    const auto& slots = prepared.candidate.mesh->GetMetadata().materialSlots;

    if (prepared.runtimeSlots != prepared.committedSlots || prepared.runtimeSlots.size() != slots.size())
    {
        Fail(error, "Residency staged binding maps are incomplete or inconsistent.");
        return {};
    }

    for (const auto& slot : slots)
    {
        const auto staged = prepared.runtimeSlots.find(slot.id);
        const auto authored = current->materialSlotBindings.find(slot.id);
        const auto reference = authored == current->materialSlotBindings.end() ?
            std::string_view(current->materialReference) : std::string_view(authored->second);

        if (staged == prepared.runtimeSlots.end() ||
            !catalog.ValidateRetainedResidencyMaterial(reference, staged->second, error) ||
            (authored == current->materialSlotBindings.end() && staged->second != prepared.fallback))
        {
            Fail(error, "Residency effective slot binding is stale: " + slot.id);
            return {};
        }
    }

    return source;
}

bool FileSceneRuntime::CommitMeshResidency(const MeshResidencyToken& token, FileSceneAssetCatalog& catalog, std::string* error)
{
    if (m_scene.HasPhysicsPresentation())
        return Fail(error, "Active physics presentation requires coordinated renderer residency publication.");
    auto source = ValidatePreparedMeshResidency(token, catalog, error);

    if (!source)
    {
        return false;
    }

    ApplyPreparedMeshResidency();
    return true;
}

void FileSceneRuntime::ApplyPreparedMeshResidency() noexcept
{
    auto& prepared = *m_meshResidency;
    const auto index = prepared.selection.runtimeIndex;
    auto& object = m_scene.GetRenderObjects()[index];

    // FINAL ACCEPTANCE: the coordinator has established matching renderer/DXR
    // readiness, outside recording. Source stays pinned. Only no-throw swaps, pointer
    // assignments and counters follow; current transforms/names/order are untouched.
    static_assert(std::is_nothrow_swappable_v<std::map<std::string, Material*>>);
    auto& committed = m_objectMapping.committedObjects[index];
    object.slotMaterials.swap(prepared.runtimeSlots);
    committed.slotMaterials.swap(prepared.committedSlots);
    object.mesh = prepared.candidate.mesh;
    committed.mesh = prepared.candidate.mesh;

    ++m_objectMapping.bindingRevisions[index];
    ++m_residencyRevision;
    m_meshResidency.reset();
}

void FileSceneRuntime::CancelMeshResidency() noexcept
{
    m_meshResidency.reset(); // Catalog ownership is unchanged; no GPU resource is released.
}

bool FileSceneAssetCatalog::ValidateRetainedResidencyMaterial(std::string_view reference,
    const Material* material, std::string* error) const
{
    const auto effective = reference.empty() ? std::string_view("builtin:default") : reference;
    if(m_archiveScene && effective != "builtin:default")
    {
        const auto id = AssetID::ParseReference(effective);
        const auto* entry = id ? m_archiveScene->provider->ArchiveEntry(*id) : nullptr;
        if(!entry || entry->type != EngineAssetPak::Type::Material || entry->payloadVersion != 1 || !material)
            return Fail(error,"Packaged material identity/admission missing.");
        const auto cached = m_projectMaterials.find({std::filesystem::path{},id->ToString()});
        const auto digest = m_admittedMaterialDigests.find(id->ToString());
        return (cached != m_projectMaterials.end() && &cached->second == material && digest != m_admittedMaterialDigests.end() &&
            digest->second == CookedSurface::Identity(*m_archiveScene->provider,*id)) || Fail(error,"Packaged material snapshot or ownership stale.");
    }

    if (!m_initialized || !material || !ValidateMetadataMaterialReference(effective, error))
    {
        Fail(error, "Residency material reference or retained binding is unavailable.");
        return false;
    }

    if (effective == "builtin:default")
    {
        if (material != &m_defaultMaterial)
        {
            return Fail(error, "Residency default material ownership changed.");
        }

        return true;
    }

    const auto id = AssetID::ParseReference(effective);
    std::error_code filesystemError;
    const auto root = std::filesystem::canonical(m_projectRoot, filesystemError);

    if (!id || filesystemError)
    {
        return Fail(error, "Residency material project identity is unavailable.");
    }

    const auto cached = m_projectMaterials.find({root, id->ToString()});

    if (cached != m_projectMaterials.end() && &cached->second == material)
    {
        return true;
    }

    for (const auto& version : m_materialVersions)
    {
        if (version->assetID == id && version->projectIdentity == root && &version->material == material)
        {
            return true;
        }
    }

    return Fail(error, "Residency material does not belong to its current authored reference.");
}
