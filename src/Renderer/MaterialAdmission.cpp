#include "Renderer/Renderer.h"
#include "Scene/FileSceneAssetCatalog.h"
#include "Scene/FileSceneRuntime.h"
#include "Scene/MaterialParameterPropagation.h"
#include <chrono>
#include <limits>

struct MaterialAdmissionState
{
    using Status = MaterialAdmissionStatus;
    using Result = MaterialAdmissionResult;
    using Candidate = ProjectMaterialPreparationService::Candidate;
    struct Dependency
    {
        size_t slot = 0;
        std::uint64_t ticket = 0;
        Texture* reused = nullptr;
        Texture::Dependency reusedLease;
        std::unique_ptr<FileSceneAssetCatalog::LiveTexture> uploaded;
    };
    std::shared_ptr<const Candidate> candidate;
    FileSceneAssetCatalog* catalog = nullptr;
    FileSceneRuntime* runtime = nullptr; // Scene release cancels before owner teardown.
    const ProjectAssetRegistry* registry = nullptr;
    std::uint64_t table = 0, revision = 0, catalogIdentity = 0, session = 0;
    std::filesystem::path catalogRoot;
    std::vector<Dependency> dependencies;
    std::array<int, 7> slots{-1, -1, -1, -1, -1, -1, -1};
    std::chrono::steady_clock::time_point retryAfter{};

    bool Matches(Renderer& renderer, FileSceneAssetCatalog& assets, const ProjectAssetRegistry& liveRegistry,
        FileSceneRuntime& runtime) const
    {
        return catalog == &assets && registry == &liveRegistry && assets.m_registry == registry &&
            assets.m_initialized && assets.m_catalogIdentity == catalogIdentity && assets.m_projectRoot == catalogRoot &&
            assets.m_device == renderer.GetDevice() && assets.m_descriptorHeap == &renderer.m_resourceDescriptorHeap &&
            renderer.HasPublishedMaterial(assets.m_defaultMaterial) &&
            table == renderer.m_sceneResourceLifetime && revision == renderer.m_materialPublication.revision &&
            session == runtime.GetRuntimeIdentity() && renderer.m_sceneResourceScene == &runtime.GetScene();
    }
    bool LifecycleCurrent(Renderer& renderer) const
    {
        if(!catalog->m_initialized || catalog->m_registry != registry || catalog->m_catalogIdentity != catalogIdentity)
            return false;
        if(!Matches(renderer, *catalog, *registry, *runtime) || !candidate->HasPinnedSources()) return false;
        const ProjectPaths paths(candidate->project);
        const auto valid = [&](const std::string& identity, AssetType type, const std::filesystem::path& path)
        {
            const auto id = AssetID::Parse(identity);
            const auto* record = id ? registry->Find(*id) : nullptr;
            const auto resolved = id ? ProjectAssetResolver::Resolve(paths, *registry, *id) : AssetResolution{};
            return record && record->type == type && resolved.status == AssetAvailability::Available && resolved.resolvedPath == path;
        };
        if(!valid(candidate->assetID, AssetType::Material, candidate->path)) return false;
        for(const auto& completion : candidate->textures)
            if(completion && !valid(completion->identity.assetID, AssetType::Texture, completion->identity.path)) return false;
        return true;
    }
    Result Retry(std::string message)
    {
        retryAfter = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        return {Status::Retry, nullptr, std::move(message)};
    }
    void BuildDependencies(FileSceneAssetCatalog& assets)
    {
        for(size_t slot = 0; slot < candidate->textures.size(); ++slot)
        {
            if(!candidate->textures[slot]) continue;
            const auto& completion = *candidate->textures[slot];
            auto found = std::find_if(dependencies.begin(), dependencies.end(), [&](const Dependency& dependency)
            { return candidate->textures[dependency.slot]->identity == completion.identity; });
            if(found != dependencies.end()) { slots[slot] = static_cast<int>(found - dependencies.begin()); continue; }
            FileSceneAssetCatalog::TextureSource source{completion.identity.path, completion.sourceBytes,
                completion.sourceModified, completion.identity.digest};
            Dependency dependency;
            dependency.slot = slot;
            dependency.reused = assets.FindReusableTexture(completion.identity.assetID, completion.identity.interpretation, source);
            if(dependency.reused)
            {
                dependency.reusedLease = dependency.reused->Retain();
                if(!dependency.reusedLease) throw std::runtime_error("Cannot retain reusable admission texture.");
            }
            if(!dependency.reused)
            {
                dependency.uploaded = std::make_unique<FileSceneAssetCatalog::LiveTexture>();
                dependency.uploaded->assetID = completion.identity.assetID;
                dependency.uploaded->interpretation = completion.identity.interpretation;
                dependency.uploaded->source = std::move(source);
            }
            slots[slot] = static_cast<int>(dependencies.size());
            dependencies.push_back(std::move(dependency));
        }
    }
    Result AdvanceTextures(Renderer& renderer)
    {
        bool pending = false;
        for(auto& dependency : dependencies)
        {
            if(dependency.reused || dependency.uploaded->texture.GetResource()) continue;
            std::string error;
            if(!dependency.ticket)
            {
                if(!renderer.QueuePreparedTextureUpload(*candidate->textures[dependency.slot]->prepared, dependency.ticket, error))
                    return error.starts_with("Deferred:") || error.starts_with("Rejected: cannot retain prepared texture request:") ?
                        Retry(error) : Result{Status::Failed, nullptr, error};
                pending = true;
                continue;
            }
            const auto phase = renderer.GetPreparedTextureUploadState(dependency.ticket, &error);
            if(phase && *phase == PreparedTextureUpload::State::Queued && error.starts_with("Deferred:"))
                return Retry(error);
            if(phase && *phase == PreparedTextureUpload::State::Rejected &&
                error.starts_with("Cannot allocate") && renderer.GetDevice()->GetDeviceRemovedReason() == S_OK)
            {
                renderer.CancelPreparedTextureUpload(dependency.ticket);
                dependency.ticket = 0;
                return Retry(error);
            }
            if(!phase || *phase == PreparedTextureUpload::State::Rejected || *phase == PreparedTextureUpload::State::Cancelled ||
                *phase == PreparedTextureUpload::State::Uncertain)
                return {Status::Failed, nullptr, "Unbound texture upload failed: " + error};
            if(*phase != PreparedTextureUpload::State::Ready) { pending = true; continue; }
            if(!renderer.TakeCompletedPreparedTextureUpload(dependency.ticket, dependency.uploaded->texture))
                return {Status::Failed, nullptr, "Cannot accept completed unbound texture upload."};
            renderer.CancelPreparedTextureUpload(dependency.ticket);
            dependency.ticket = 0;
        }
        return {pending ? Status::Pending : Status::Ready};
    }
    bool ReusedTexturesCurrent(FileSceneAssetCatalog& assets) const
    {
        for(const auto& dependency : dependencies)
            if(dependency.reused)
            {
                const auto& completion = *candidate->textures[dependency.slot];
                const FileSceneAssetCatalog::TextureSource source{completion.identity.path, completion.sourceBytes,
                    completion.sourceModified, completion.identity.digest};
                auto* current = assets.FindReusableTexture(completion.identity.assetID, completion.identity.interpretation, source);
                if(current != dependency.reused || !current || current->Retain() != dependency.reusedLease) return false;
            }
        return true;
    }
    Result Publish(Renderer& renderer, ProjectMaterialPreparationService& preparation, FileSceneAssetCatalog& assets,
        const ProjectPaths& paths, const ProjectAssetRegistry& liveRegistry, FileSceneRuntime& runtime)
    {
        if(!ReusedTexturesCurrent(assets))
            return {Status::Stale, nullptr, "Reusable texture ownership changed."};
        if(std::any_of(renderer.m_materialReplacements.begin(), renderer.m_materialReplacements.end(),
            [](const auto& replacement) { return bool(replacement); }))
            return Retry("Previous material-table generation awaits frame-slot retirement.");
        if(renderer.m_resourceDescriptorHeap.GetAvailableCount() < CommandContext::FrameCount)
            return Retry("Material-table descriptors unavailable.");
        const auto count = renderer.m_materialPublication.desired.size();
        if(count >= Renderer::MaterialAdmissionLimit || count + 1 > UINT_MAX ||
            count + 1 > (std::numeric_limits<size_t>::max)() / sizeof(GPUMaterial))
            return {Status::Unsupported, nullptr, "Incremental material-table capacity exhausted."};

        Material material;
        material.pipeline = &assets.m_pipeline;
        material.texture = &assets.m_whiteTexture;
        MaterialParameterPropagation::Apply(material, candidate->parameters);
        Texture** destinations[] = {&material.texture, &material.normalTexture, &material.roughnessTexture,
            &material.metallicTexture, &material.armTexture, &material.aoTexture, &material.heightTexture};
        for(size_t slot = 0; slot < slots.size(); ++slot)
            if(slots[slot] >= 0)
            {
                const auto& dependency = dependencies[slots[slot]];
                *destinations[slot] = dependency.reused ? dependency.reused : &dependency.uploaded->texture;
            }
        material.gpuIndex = static_cast<unsigned int>(count); // Private staging only.
        auto publication = renderer.m_materialPublication;
        if(!publication.Append(material.GetGPUData(), Renderer::MaterialAdmissionLimit))
            return {Status::Failed, nullptr, "Invalid material append."};
        auto retained = renderer.m_materialDependencies.Desired();
        Texture* textures[] = {material.texture, material.normalTexture, material.roughnessTexture,
            material.metallicTexture, material.armTexture, material.aoTexture, material.heightTexture};
        for(auto* texture : textures)
            if(texture)
            {
                auto lease = texture->Retain();
                if(!lease || !lease->UsesDescriptorHeap(renderer.m_resourceDescriptorHeap) || lease->GetDescriptorIndex() == UINT_MAX)
                    return {Status::Failed, nullptr, "Cannot retain admission texture in the active heap."};
                retained.push_back(std::move(lease));
            }

        // All fallible CPU/catalog allocation is staged before visible mutation.
        const FileSceneAssetCatalog::MaterialCacheKey key{candidate->project, candidate->assetID};
        const auto cached = assets.m_projectMaterials.find(key);
        const auto cachedSource = assets.m_materialSources.find(key);
        const auto cachedBinding = assets.m_materialBindingRevisions.find(key);
        const bool reuseMaterial = cached != assets.m_projectMaterials.end();
        if(reuseMaterial && (renderer.HasPublishedMaterial(cached->second) || cachedSource == assets.m_materialSources.end() ||
            cachedBinding == assets.m_materialBindingRevisions.end() || cachedBinding->second == UINT64_MAX))
            return {Status::Stale, nullptr, "Cached material ownership changed before admission."};
        std::map<FileSceneAssetCatalog::MaterialCacheKey, Material> materials;
        std::map<FileSceneAssetCatalog::MaterialCacheKey, MaterialAssetData> sources;
        std::map<FileSceneAssetCatalog::MaterialCacheKey, std::uint64_t> bindings;
        std::map<std::string, std::string> digests;
        materials.emplace(key, material); sources.emplace(key, candidate->parameters);
        bindings.emplace(key, 1); digests.emplace(candidate->assetID, candidate->digest);
        auto* publishedMaterial = reuseMaterial ? &cached->second : &materials.begin()->second;
        auto identities = renderer.m_publishedMaterialIdentities;
        identities.push_back(publishedMaterial);
        assets.m_liveTextures.reserve(assets.m_liveTextures.size() + dependencies.size());
        std::array<std::unique_ptr<MaterialTableReplacement>, CommandContext::FrameCount> replacements;
        for(auto& replacement : replacements)
        {
            replacement = std::make_unique<MaterialTableReplacement>();
            if(!replacement->buffer.InitializeStorage(renderer.GetDevice(), sizeof(GPUMaterial),
                static_cast<unsigned int>(count + 1), renderer.m_resourceDescriptorHeap) ||
                !replacement->upload.Initialize(renderer.GetDevice(), (count + 1) * sizeof(GPUMaterial), 1))
                return renderer.GetDevice()->GetDeviceRemovedReason() == S_OK ? Retry("Material-table GPU allocation unavailable.") :
                    Result{Status::Failed, nullptr, "Device lost allocating material table."};
        }
        std::string error;
        if(!Matches(renderer, assets, liveRegistry, runtime) ||
            !ReusedTexturesCurrent(assets) ||
            (!reuseMaterial && (assets.m_projectMaterials.contains(key) || assets.m_materialSources.contains(key) ||
                assets.m_materialBindingRevisions.contains(key))) ||
            assets.m_admittedMaterialDigests.contains(candidate->assetID) ||
            !preparation.Validate(*candidate, paths, liveRegistry, runtime.GetRuntimeIdentity(), error))
            return {Status::Stale, nullptr, "Final admission evidence changed: " + error};
        if(!renderer.m_materialDependencies.SetDesired(std::move(retained)))
            return Retry("Material texture retention capacity unavailable.");

        // Nonallocating commit. No bindings, AS structures or history are changed.
        if(reuseMaterial)
        {
            cached->second = material;
            static_assert(std::is_nothrow_swappable_v<MaterialAssetData>);
            std::swap(cachedSource->second, sources.begin()->second);
            ++cachedBinding->second; // Supersede any old, unpublished streaming bindings.
        }
        else
        {
            assets.m_projectMaterials.insert(materials.extract(materials.begin()));
            assets.m_materialSources.insert(sources.extract(sources.begin()));
            assets.m_materialBindingRevisions.insert(bindings.extract(bindings.begin()));
        }
        assets.m_admittedMaterialDigests.insert(digests.extract(digests.begin()));
        for(auto& dependency : dependencies)
            if(dependency.uploaded) assets.m_liveTextures.push_back(std::move(dependency.uploaded));
        renderer.m_materialPublication = std::move(publication);
        renderer.m_publishedMaterialIdentities.swap(identities);
        renderer.m_materialReplacements.swap(replacements);
        return {Status::Ready, publishedMaterial, {}};
    }
};

void Renderer::CancelMaterialAdmission()
{
    m_invalidatedMaterialCandidate.reset();
    if(!m_materialAdmission) return;
    for(const auto& dependency : m_materialAdmission->dependencies)
        if(dependency.ticket) CancelPreparedTextureUpload(dependency.ticket);
    m_materialAdmission.reset(); // Only completed textures/unsubmitted buffers are owned here.
}

void Renderer::InvalidateMaterialAdmission()
{
    if(!m_materialAdmission) return;
    auto candidate = m_materialAdmission->candidate;
    CancelMaterialAdmission();
    m_invalidatedMaterialCandidate = candidate;
}

void Renderer::ValidateMaterialAdmissionLifecycle()
{
    if(!m_materialAdmission) return;
    try { if(!m_materialAdmission->LifecycleCurrent(*this)) InvalidateMaterialAdmission(); }
    catch(...) { InvalidateMaterialAdmission(); }
}

MaterialAdmissionResult Renderer::AdvanceMaterialAdmission(ProjectMaterialPreparationService& preparation,
    std::shared_ptr<const ProjectMaterialPreparationService::Candidate> candidate, FileSceneAssetCatalog& catalog,
    const ProjectPaths& paths, const ProjectAssetRegistry& registry, FileSceneRuntime& runtime)
{
    using Status = MaterialAdmissionStatus;
    if(candidate && m_invalidatedMaterialCandidate.lock() == candidate)
        return {Status::Stale, nullptr, "Admission was invalidated by Renderer lifecycle/source checks."};
    const auto reject = [&](Status status, std::string error)
    { CancelMaterialAdmission(); return MaterialAdmissionResult{status, nullptr, std::move(error)}; };
    try
    {
        std::string error;
        if(!candidate || !preparation.Validate(*candidate, paths, registry, runtime.GetRuntimeIdentity(), error))
            return reject(Status::Stale, "Invalid admission candidate: " + error);
        if(!m_sceneResourceLifetime || m_sceneResourceScene != &runtime.GetScene())
            return reject(Status::Stale, "Admission renderer/scene is no longer current.");
        if(m_materialAdmission && (m_materialAdmission->candidate != candidate ||
            !m_materialAdmission->Matches(*this, catalog, registry, runtime)))
            return reject(Status::Stale, "Admission project, scene, renderer or material table changed.");
        if(!CanEditPendingMaterialBindings(runtime.GetScene()) || runtime.HasPreparedScene() || runtime.HasMaterialAssignment())
            return {Status::Retry, nullptr, "Renderer/scene boundary unavailable."};
        if(!m_materialAdmission)
        {
            auto state = std::make_shared<MaterialAdmissionState>();
            state->candidate = std::move(candidate); state->catalog = &catalog; state->registry = &registry;
            state->runtime = &runtime;
            state->table = m_sceneResourceLifetime; state->revision = m_materialPublication.revision;
            state->catalogIdentity = catalog.m_catalogIdentity; state->session = runtime.GetRuntimeIdentity();
            state->catalogRoot = catalog.m_projectRoot;
            if(!state->Matches(*this, catalog, registry, runtime) ||
                std::filesystem::canonical(catalog.m_projectRoot) != state->candidate->project)
                return reject(Status::Stale, "Admission catalog identity changed.");
            const auto key = FileSceneAssetCatalog::MaterialCacheKey{state->candidate->project, state->candidate->assetID};
            if(auto* existing = catalog.FindPublishedMaterial("asset:" + state->candidate->assetID, *this))
            {
                const auto digest = catalog.m_admittedMaterialDigests.find(state->candidate->assetID);
                if(digest != catalog.m_admittedMaterialDigests.end() && digest->second != state->candidate->digest)
                    return {Status::Unsupported, nullptr, "Material is already admitted with another version; use material revision publication."};
                return {Status::Ready, existing, {}};
            }
            if(catalog.m_projectMaterials.contains(key) != catalog.m_materialSources.contains(key) ||
                catalog.m_projectMaterials.contains(key) != catalog.m_materialBindingRevisions.contains(key))
                return {Status::Unsupported, nullptr, "Inconsistent unpublished catalog material ownership."};
            if(m_materialPublication.desired.size() >= MaterialAdmissionLimit)
                return {Status::Unsupported, nullptr, "Incremental material capacity exhausted."};
            if(std::any_of(m_materialReplacements.begin(), m_materialReplacements.end(), [](const auto& value) { return bool(value); }))
                return {Status::Retry, nullptr, "Material-table generation awaits completed frame-slot reuse."};
            state->BuildDependencies(catalog);
            m_materialAdmission = std::move(state);
        }
        auto& state = *m_materialAdmission;
        if(std::chrono::steady_clock::now() < state.retryAfter)
            return {Status::Retry, nullptr, "Admission resource retry is throttled."};
        auto textures = state.AdvanceTextures(*this);
        if(textures.status == Status::Failed) return reject(textures.status, textures.message);
        if(textures.status != Status::Ready) return textures;
        auto result = state.Publish(*this, preparation, catalog, paths, registry, runtime);
        if(result.status != Status::Retry) CancelMaterialAdmission();
        return result;
    }
    catch(const std::bad_alloc&)
    {
        if(m_materialAdmission) return m_materialAdmission->Retry("Admission CPU allocation unavailable.");
        return {Status::Retry, nullptr, "Admission CPU allocation unavailable."};
    }
    catch(const std::exception& exception) { return reject(Status::Failed, exception.what()); }
}
