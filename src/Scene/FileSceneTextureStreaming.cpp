#include "Scene/FileSceneAssetCatalog.h"
#include "Scene/Scene.h"
#include "Scene/SceneSwitchTiming.h"
#include "Renderer/Renderer.h"
#include "assets/MaterialAssetSerializer.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <type_traits>

namespace
{
    using Clock = std::chrono::steady_clock;
    using TextureMember = Texture* Material::*;
    constexpr std::array<TextureMember, 7> kTextureMembers = {&Material::texture, &Material::normalTexture,
        &Material::roughnessTexture, &Material::metallicTexture, &Material::armTexture,
        &Material::aoTexture, &Material::heightTexture};

    std::array<std::optional<std::string>, 7> TextureReferences(const MaterialAssetData& data)
    {
        return {data.baseColorTexture, data.normalTexture, data.roughnessTexture, data.metallicTexture,
            data.armTexture, data.aoTexture, data.heightTexture};
    }

    ProjectTextureInterpretation Interpretation(size_t slot)
    {
        if(slot == 0)
        {
            return ProjectTextureInterpretation::SRGBColor;
        }

        return slot == 1 ? ProjectTextureInterpretation::NormalMap : ProjectTextureInterpretation::LinearData;
    }
}

void FileSceneAssetCatalog::BeginTextureStreaming()
{
    m_streamingActivated = Clock::now();
    m_streamingReported = false;
}

void FileSceneAssetCatalog::QueueMaterialTextures(const MaterialCacheKey& key, const MaterialAssetData& data)
{
    const auto references = TextureReferences(data);

    for(size_t slot = 0; slot < references.size(); ++slot)
    {
        if(!references[slot])
        {
            continue;
        }

        const auto id = AssetID::ParseReference(*references[slot]);
        const TextureCacheKey textureKey{key.first, id->ToString(), Interpretation(slot)};
        auto found = m_streamingTextures.find(textureKey);

        if(SceneSwitchTiming::active)
        {
            ++SceneSwitchTiming::active->textureReferences;
        }

        if(found == m_streamingTextures.end())
        {
            if(m_streamingTextures.size() >= kStreamingTextureCapacity)
            {
                ++m_streamingStatistics.failed;

                if(m_streamingStatistics.diagnostics++ < 8)
                {
                    Log::Warning("Scene texture request capacity reached; retaining fallback.");
                }

                continue;
            }

            found = m_streamingTextures.try_emplace(textureKey).first;
        }
        else
        {
            ++m_streamingStatistics.deduplicated;

            if(SceneSwitchTiming::active)
            {
                ++SceneSwitchTiming::active->textureDeduplicated;
            }

            // A newly resolved material can reuse this asset, but publication still
            // rechecks its own binding revision and the source content.
            if(found->second.phase == StreamingTexture::Phase::Resident)
            {
                found->second.phase = StreamingTexture::Phase::Requested;
            }
        }

        found->second.bindings.push_back({key, slot, m_materialBindingRevisions.at(key)});
        m_streamingReported = false;
    }
}

bool FileSceneAssetCatalog::IsCurrentStreamingBinding(const StreamingBinding& binding,
    const TextureCacheKey& texture, const Scene& scene) const
{
    const auto material = m_projectMaterials.find(binding.material);
    const auto baseline = m_materialSources.find(binding.material);
    const auto revision = m_materialBindingRevisions.find(binding.material);
    const auto id = AssetID::Parse(binding.material.second);
    const auto* record = id && m_registry ? m_registry->Find(*id) : nullptr;

    if(material == m_projectMaterials.end() || baseline == m_materialSources.end() ||
        revision == m_materialBindingRevisions.end() || revision->second != binding.revision ||
        !record || record->type != AssetType::Material || binding.material.first != std::get<0>(texture))
    {
        return false;
    }

    const auto references = TextureReferences(baseline->second);
    const auto requested = references[binding.slot] ? AssetID::ParseReference(*references[binding.slot]) : std::nullopt;
    const auto* fallback = binding.slot == 0 ? &m_whiteTexture : nullptr;

    if(!requested || requested->ToString() != std::get<1>(texture) ||
        Interpretation(binding.slot) != std::get<2>(texture) || material->second.*kTextureMembers[binding.slot] != fallback)
    {
        return false;
    }

    // Catalog versions are retained for transactions/history. Only current scene
    // assignments authorize residency publication; no runtime index is retained.
    for(const auto& object : scene.GetRenderObjects())
    {
        if(object.material == &material->second)
        {
            return true;
        }

        for(const auto& [slot, assigned] : object.slotMaterials)
        {
            if(assigned == &material->second)
            {
                return true;
            }
        }
    }

    return false;
}

void FileSceneAssetCatalog::ResetStreamingTexture(StreamingTexture& texture)
{
    if(m_texturePreparation && texture.cpuTicket)
    {
        m_texturePreparation->Cancel(texture.cpuTicket);
    }

    if(m_textureRenderer && texture.gpuTicket)
    {
        // Renderer retains submitted/uncertain uploads until checked retirement.
        m_textureRenderer->CancelPreparedTextureUpload(texture.gpuTicket);
    }

    texture.completion.reset();
    texture.uploaded.reset(); // Only upload-complete, unpublished textures live here.
    texture.cpuTicket = 0;
    texture.gpuTicket = 0;

    texture.phase = StreamingTexture::Phase::Requested;
    texture.retryAfter = {};
}

void FileSceneAssetCatalog::CancelTextureStreaming()
{
    for(auto& [key, texture] : m_streamingTextures)
    {
        if(texture.phase != StreamingTexture::Phase::Resident && texture.phase != StreamingTexture::Phase::Failed)
        {
            ResetStreamingTexture(texture);
        }
    }

    m_texturePreparation = nullptr;
    m_textureRenderer = nullptr;
    m_streamingRuntime = 0;
    m_streamingTable = 0;
    m_streamingGeneration = 0;
}

void FileSceneAssetCatalog::FailStreamingTexture(StreamingTexture& texture, const std::string& error)
{
    ResetStreamingTexture(texture);
    texture.phase = StreamingTexture::Phase::Failed;
    ++m_streamingStatistics.failed;

    if(m_streamingStatistics.diagnostics++ < 8)
    {
        Log::Warning("Scene texture retained fallback: " + error);
    }
}

FileSceneAssetCatalog::TextureSource FileSceneAssetCatalog::ValidateStreamingSource(
    const TextureCacheKey& key, const StreamingTexture& texture) const
{
    if(!texture.completion || !m_texturePreparation->IsCurrent(*texture.completion) ||
        texture.completion->identity.project != std::get<0>(key) ||
        texture.completion->identity.assetID != std::get<1>(key) ||
        texture.completion->identity.interpretation != std::get<2>(key))
    {
        throw std::runtime_error("Texture completion scene generation or request identity changed.");
    }

    const auto id = AssetID::Parse(std::get<1>(key));
    const auto* record = id ? m_registry->Find(*id) : nullptr;
    const auto resolved = id ? ProjectAssetResolver::Resolve(ProjectPaths(std::get<0>(key)), *m_registry, *id) : AssetResolution{};

    if(!record || record->type != AssetType::Texture || resolved.status != AssetAvailability::Available ||
        resolved.resolvedPath != texture.completion->identity.path)
    {
        throw std::runtime_error("Texture registration or contained source path changed.");
    }

    auto source = InspectTextureSource(resolved.resolvedPath);

    if(source.digest != texture.completion->identity.digest)
    {
        throw std::runtime_error("Texture source changed before GPU admission or material publication.");
    }

    return source;
}

void FileSceneAssetCatalog::AdvanceTextureStreaming(ProjectTexturePreparationService& preparation, Renderer& renderer,
    std::uint64_t runtimeIdentity, const Scene& scene,
    const std::function<bool(const AssetID&, const MaterialAssetData&)>& mayPublish)
{
    if(!m_textureStreamingEnabled || !m_initialized || !m_registry || !runtimeIdentity ||
        renderer.GetMaterialTableIdentity() == 0 || m_streamingTextures.empty())
    {
        return;
    }

    if(m_streamingActivated == Clock::time_point{})
    {
        BeginTextureStreaming();
    }

    if(m_streamingRuntime != runtimeIdentity || m_streamingTable != renderer.GetMaterialTableIdentity() ||
        m_streamingGeneration != preparation.Generation())
    {
        CancelTextureStreaming();
        m_streamingRuntime = runtimeIdentity;
        m_streamingTable = renderer.GetMaterialTableIdentity();
        m_streamingGeneration = preparation.Generation();
    }

    m_texturePreparation = &preparation;
    m_textureRenderer = &renderer;

    // Permit one expensive transition per update. Skip settled/cooldown jobs and
    // cheap incomplete polls so the final pending asset is still serviced each frame.
    for(size_t visited = 0; visited < m_streamingTextures.size(); ++visited)
    {
        auto selected = m_streamingTextures.begin();
        std::advance(selected, m_streamingCursor++ % m_streamingTextures.size());
        auto& texture = selected->second;
        const auto phase = texture.phase;

        if(phase == StreamingTexture::Phase::Resident || phase == StreamingTexture::Phase::Failed ||
            Clock::now() < texture.retryAfter)
        {
            continue;
        }

        const auto deferred = m_streamingStatistics.deferred;

        try
        {
            AdvanceStreamingTexture(selected->first, texture, scene, mayPublish);
        }
        catch(const std::exception& error)
        {
            FailStreamingTexture(texture, error.what());
        }

        if(texture.phase != phase || m_streamingStatistics.deferred != deferred)
        {
            break;
        }
    }

    ReportTextureStreaming();
}

void FileSceneAssetCatalog::AdvanceStreamingTexture(const TextureCacheKey& key, StreamingTexture& texture,
    const Scene& scene, const std::function<bool(const AssetID&, const MaterialAssetData&)>& mayPublish)
{
    using Phase = StreamingTexture::Phase;

    if(texture.phase == Phase::Resident || texture.phase == Phase::Failed)
    {
        return;
    }

    const bool wanted = std::any_of(texture.bindings.begin(), texture.bindings.end(), [&](const auto& binding)
    {
        return IsCurrentStreamingBinding(binding, key, scene);
    });

    if(!wanted)
    {
        ResetStreamingTexture(texture);
        texture.phase = Phase::Superseded;
        return;
    }

    if(texture.phase == Phase::Superseded)
    {
        texture.phase = Phase::Requested;
        m_streamingReported = false;
    }

    if(Clock::now() < texture.retryAfter)
    {
        return;
    }

    const auto defer = [&]()
    {
        ++m_streamingStatistics.deferred;
        texture.retryAfter = Clock::now() + std::chrono::milliseconds(250);
    };

    std::string error;

    if(texture.phase == Phase::Requested)
    {
        const auto id = AssetID::Parse(std::get<1>(key));

        if(!m_texturePreparation->Request(ProjectPaths(std::get<0>(key)), *m_registry, *id,
            std::get<2>(key), {}, m_streamingGeneration, texture.cpuTicket, error))
        {
            if(error.starts_with("Deferred:"))
            {
                defer();
                return;
            }

            ++m_streamingStatistics.cpuFailed;
            FailStreamingTexture(texture, error);
            return;
        }

        ++m_streamingStatistics.submitted;
        texture.phase = Phase::Preparing;
        return;
    }

    if(texture.phase == Phase::Preparing)
    {
        auto completion = m_texturePreparation->Poll(texture.cpuTicket, ProjectPaths(std::get<0>(key)), *m_registry);

        if(!completion)
        {
            return;
        }

        if(!completion->prepared || !m_texturePreparation->IsCurrent(*completion))
        {
            if(completion->error.starts_with("Deferred:"))
            {
                ResetStreamingTexture(texture);
                defer();
                return;
            }

            ++m_streamingStatistics.cpuFailed;
            FailStreamingTexture(texture, completion->error);
            return;
        }

        texture.completion = std::move(completion);
        texture.phase = Phase::CPUReady;
        ++m_streamingStatistics.cpuCompleted;
        return;
    }

    if(texture.phase == Phase::CPUReady)
    {
        ValidateStreamingSource(key, texture);

        if(!m_textureRenderer->QueuePreparedTextureUpload(*texture.completion->prepared, texture.gpuTicket, error))
        {
            if(error.starts_with("Deferred:"))
            {
                defer();
                return;
            }

            FailStreamingTexture(texture, error);
            return;
        }

        // QueuePreparedTextureUpload owns a copy. Keep identity/revocation but release
        // the service's pixel lease; do not accumulate two full CPU copies per job.
        texture.completion->prepared.reset();
        texture.phase = Phase::Uploading;
        return;
    }

    if(texture.phase == Phase::Uploading)
    {
        const auto state = m_textureRenderer->GetPreparedTextureUploadState(texture.gpuTicket, &error);

        if(!state || *state == PreparedTextureUpload::State::Rejected ||
            *state == PreparedTextureUpload::State::Cancelled || *state == PreparedTextureUpload::State::Uncertain)
        {
            FailStreamingTexture(texture, "GPU upload unavailable or rejected: " + error);
            return;
        }

        if(*state != PreparedTextureUpload::State::Ready)
        {
            return;
        }

        auto uploaded = std::make_unique<LiveTexture>();
        uploaded->assetID = std::get<1>(key);
        uploaded->interpretation = std::get<2>(key);
        uploaded->source = ValidateStreamingSource(key, texture);

        if(!m_textureRenderer->TakeCompletedPreparedTextureUpload(texture.gpuTicket, uploaded->texture))
        {
            return;
        }

        texture.gpuTicket = 0;
        texture.uploaded = std::move(uploaded);
        texture.phase = Phase::Ready;
        ++m_streamingStatistics.gpuCompleted;
        return;
    }

    if(texture.phase == Phase::Ready && !PublishStreamingTexture(key, texture, scene, mayPublish))
    {
        defer();
    }
}

bool FileSceneAssetCatalog::PublishStreamingTexture(const TextureCacheKey& key, StreamingTexture& texture,
    const Scene& scene, const std::function<bool(const AssetID&, const MaterialAssetData&)>& mayPublish)
{
    struct Candidate
    {
        Material* target;
        Material value;
    };

    std::vector<Candidate> candidates;
    std::map<MaterialCacheKey, MaterialAssetData> checkedMaterials;
    const ProjectPaths paths(std::get<0>(key));

    for(auto& binding : texture.bindings)
    {
        if(!IsCurrentStreamingBinding(binding, key, scene))
        {
            continue;
        }

        const auto asset = AssetID::Parse(binding.material.second);
        const auto& baseline = m_materialSources.at(binding.material);

        if(!mayPublish(*asset, baseline) ||
            (m_savedRevisionJob && m_savedRevisionJob->expected.assetID == *asset))
        {
            return false; // An editor texture edit owns the next binding revision.
        }

        auto checked = checkedMaterials.find(binding.material);

        if(checked == checkedMaterials.end())
        {
            const auto resolved = ProjectAssetResolver::Resolve(paths, *m_registry, *asset);
            MaterialAssetData data;
            std::string error;

            if(resolved.status != AssetAvailability::Available ||
                !MaterialAssetSerializer::LoadFromFile(resolved.resolvedPath, data, &error))
            {
                binding.revision = 0;
                continue;
            }

            checked = checkedMaterials.emplace(binding.material, std::move(data)).first;
        }

        if(TextureReferences(checked->second)[binding.slot] != TextureReferences(baseline)[binding.slot])
        {
            binding.revision = 0; // Superseded binding cannot authorize this completion.
            continue;
        }

        auto* target = &m_projectMaterials.at(binding.material);
        auto candidate = std::find_if(candidates.begin(), candidates.end(), [target](const Candidate& value)
        {
            return value.target == target;
        });

        if(candidate == candidates.end())
        {
            candidates.push_back({target, *target});
            candidate = std::prev(candidates.end());
        }

        candidate->value.*kTextureMembers[binding.slot] = &texture.uploaded->texture;
    }

    if(candidates.empty())
    {
        ResetStreamingTexture(texture);
        texture.phase = StreamingTexture::Phase::Superseded;
        return true;
    }

    ValidateStreamingSource(key, texture);
    std::vector<GPUMaterial> table;
    std::string error;

    if(!m_textureRenderer->CopyMaterialTableSnapshot(m_streamingTable, table, &error))
    {
        throw std::runtime_error(error);
    }

    for(const auto& candidate : candidates)
    {
        if(candidate.target->gpuIndex >= table.size())
        {
            throw std::runtime_error("Texture publication material-table index changed.");
        }

        table[candidate.target->gpuIndex] = candidate.value.GetGPUData();
    }

    // Retain the complete resulting table, including materials unused by current
    // objects but still owned for editor transactions and undo/redo.
    std::vector<Texture::Dependency> leases;

    for(auto* material : GetMaterials())
    {
        const auto candidate = std::find_if(candidates.begin(), candidates.end(), [material](const Candidate& value)
        {
            return value.target == material;
        });
        const auto& value = candidate == candidates.end() ? *material : candidate->value;

        for(auto member : kTextureMembers)
        {
            if(auto* dependency = value.*member)
            {
                leases.push_back(dependency->Retain());
            }
        }
    }

    // Allocate all ownership bookkeeping before Renderer accepts the new revision.
    m_liveTextures.reserve(m_liveTextures.size() + 1);

    if(!m_textureRenderer->SubmitMaterialTableRevision(m_streamingTable, table, std::move(leases), &error))
    {
        if(error.starts_with("Deferred:"))
        {
            return false;
        }

        throw std::runtime_error(error);
    }

    static_assert(std::is_nothrow_copy_assignable_v<Material>);

    for(const auto& candidate : candidates)
    {
        *candidate.target = candidate.value;
    }

    m_liveTextures.push_back(std::move(texture.uploaded));
    ResetStreamingTexture(texture);
    texture.phase = StreamingTexture::Phase::Resident;
    ++m_streamingStatistics.publications;
    return true;
}

void FileSceneAssetCatalog::ReportTextureStreaming()
{
    using Phase = StreamingTexture::Phase;
    const bool settled = std::all_of(m_streamingTextures.begin(), m_streamingTextures.end(), [](const auto& entry)
    {
        const auto phase = entry.second.phase;
        return phase == Phase::Resident || phase == Phase::Failed || phase == Phase::Superseded;
    });

    if(!settled || m_streamingReported)
    {
        return;
    }

    const auto resident = std::count_if(m_streamingTextures.begin(), m_streamingTextures.end(), [](const auto& entry)
    {
        return entry.second.phase == Phase::Resident;
    });
    const auto superseded = std::count_if(m_streamingTextures.begin(), m_streamingTextures.end(), [](const auto& entry)
    {
        return entry.second.phase == Phase::Superseded;
    });
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - m_streamingActivated).count();
    const auto& counts = m_streamingStatistics;
    Log::Info("File scene texture residency settled: submitted=" + std::to_string(counts.submitted) +
        ", deduplicated=" + std::to_string(counts.deduplicated) + ", deferred/retried=" + std::to_string(counts.deferred) +
        ", CPU completed=" + std::to_string(counts.cpuCompleted) + ", CPU failed=" + std::to_string(counts.cpuFailed) +
        ", GPU completed=" + std::to_string(counts.gpuCompleted) + ", publications=" + std::to_string(counts.publications) +
        ", resident=" + std::to_string(resident) + ", failed=" + std::to_string(counts.failed) +
        ", superseded=" + std::to_string(superseded) + ", activation-to-settled ms=" + std::to_string(elapsed));
    m_streamingReported = true;
}
