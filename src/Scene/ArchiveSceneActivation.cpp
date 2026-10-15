#include "Scene/FileSceneAssetCatalog.h"
#include "assets/CookedScene.h"
#include <stdexcept>

void FileSceneAssetCatalog::SetArchiveScene(std::shared_ptr<const CookedScene::ArchiveScene> source)
{
    if(!m_initialized || m_registry || !m_cookedMetadata.empty() || !source || !source->ValidateReferences())
        throw std::runtime_error("Archive catalog requires a fresh initialized catalog and validated source.");
    m_projectRoot.clear(); m_registry = nullptr; m_archiveScene = std::move(source);
}
bool FileSceneAssetCatalog::ValidateArchiveMesh(std::string_view reference, const CookedMeshSource::AssetEvidence* evidence,
    const CookedMesh::MetadataSnapshot* metadata, std::string* error) const
{
    const auto fail = [&](const char* message) { if(error) *error = message; return false; };
    if(!m_initialized || !m_archiveScene || !m_archiveScene->ValidateReferences(error)) return false;
    bool found = false;
    const auto& source = m_archiveScene->metadata;
    for(size_t i=0;i<source.document.objects.size();++i)
    {
        if(source.document.objects[i].meshReference != reference) continue;
        const auto& binding = source.meshes[i];
        if(!binding) return fail("Missing packaged mesh binding.");
        found = true;
        if(evidence && (evidence->mapping != m_archiveScene->provider->Mapping() || evidence->contentGeneration != binding->generation || evidence->payload != binding->payload))
            return fail("Cooked completion belongs to an incompatible scene archive generation.");
        if(metadata && (metadata->evidence != binding->payload || metadata->metadata.materialSlots != binding->slots))
            return fail("Cooked scene slot layout differs from verified mesh metadata.");
    }
    return found || fail("Mesh reference is absent from the packaged scene.");
}
