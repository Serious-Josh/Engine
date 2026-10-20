#include "assets/CookedScene.h"
#include "Scene/PreparedScene.h"
#include "Scene/SceneCommit.h"
#include "Scene/SceneDocumentAdapter.h"
#include "Scene/FileSceneAssetCatalog.h"
#include "assets/ProjectAssetResolver.h"
#include "assets/MaterialAssetSerializer.h"

#include <array>
#include <exception>
#include <set>
#include <type_traits>
#include <utility>

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

bool FileSceneAssetCatalog::ValidateMetadataMeshReference(std::string_view reference, std::string* error) const
{
    if(m_archiveScene)
    {
        if(reference.empty()) return m_initialized && m_archiveScene->ValidateReferences(error);
        return ValidateArchiveMesh(reference,nullptr,nullptr,error);
    }
    if(!m_initialized || !m_registry)
    {
        return Fail(error, "Metadata mesh validation requires an initialized catalog.");
    }

    try
    {
        const auto project = std::filesystem::canonical(m_projectRoot);

        if(reference.empty() || ClassifyMeshReference(reference) == MeshReferenceKind::Builtin)
        {
            return true;
        }

        const auto id = AssetID::ParseReference(reference);
        const auto* record = id ? m_registry->Find(*id) : nullptr;

        if(!id || id->ToReference() != reference || !record || record->type != AssetType::Mesh)
        {
            return Fail(error, "Metadata mesh reference must identify a registered canonical Mesh asset: " +
                std::string(reference));
        }

        const auto resolved = ProjectAssetResolver::Resolve(ProjectPaths(project), *m_registry, *id);

        if(resolved.status != AssetAvailability::Available)
        {
            return Fail(error, "Metadata mesh source is missing, inaccessible, or outside the project Meshes directory: " +
                std::string(reference));
        }

        if(_wcsicmp(resolved.resolvedPath.extension().c_str(), L".fbx") != 0)
        {
            return Fail(error, "Registered metadata mesh source must have the .fbx extension.");
        }

        return true;
    }
    catch(const std::exception& exception)
    {
        return Fail(error, "Metadata mesh validation failed: " + std::string(exception.what()));
    }
}

bool FileSceneAssetCatalog::ValidateMetadataMaterialReference(std::string_view reference, std::string* error) const
{
    auto* pass = ActiveValidationPass();

    if(!pass || m_archiveScene || reference.empty() || reference == "builtin:default")
    {
        return ValidateMetadataMaterialReferenceOnDisk(reference, error);
    }

    // The result is a function of the exact reference, this catalog and the disk.
    // Within one pass the catalog is unchanged, so equal references share it.
    const std::string key(reference);
    auto found = pass->materials.find(key);

    if(found == pass->materials.end())
    {
        ValidationPassState::MaterialResult result;
        result.valid = ValidateMetadataMaterialReferenceOnDisk(reference, &result.error);
        found = pass->materials.emplace(key, std::move(result)).first;
    }

    if(!found->second.valid)
    {
        return Fail(error, found->second.error);
    }

    return true;
}

bool FileSceneAssetCatalog::ValidateMetadataMaterialReferenceOnDisk(std::string_view reference, std::string* error) const
{
    if(m_archiveScene)
    {
        const auto id = AssetID::ParseReference(reference);
        return m_initialized && (reference.empty() || reference == "builtin:default" || (id && id->ToReference() == reference));
    }
    if(!m_initialized || !m_registry)
    {
        return Fail(error, "Metadata material validation requires an initialized project catalog.");
    }

    if(reference.empty() || reference == "builtin:default")
    {
        return true;
    }

    try
    {
        const auto project = std::filesystem::canonical(m_projectRoot);
        const auto id = AssetID::ParseReference(reference);
        const auto* record = id ? m_registry->Find(*id) : nullptr;
        const ProjectPaths paths(project);
        const auto resolved = id ? ProjectAssetResolver::Resolve(paths, *m_registry, *id) : AssetResolution{};

        if(!id || id->ToReference() != reference || !record || record->type != AssetType::Material ||
            resolved.status != AssetAvailability::Available)
        {
            return Fail(error, "Metadata material reference is invalid, missing, or outside the project: " +
                std::string(reference));
        }

        MaterialAssetData data;

        if(!MaterialAssetSerializer::LoadFromFile(resolved.resolvedPath, data, error))
        {
            return false;
        }

        const auto baseline = m_materialSources.find({project, id->ToString()});

        if(baseline != m_materialSources.end() &&
            MaterialAssetSerializer::Serialize(data) != MaterialAssetSerializer::Serialize(baseline->second))
        {
            return Fail(error, "Material source changed after metadata preparation: " + std::string(reference));
        }

        const std::array references = {data.baseColorTexture, data.normalTexture, data.roughnessTexture,
            data.metallicTexture, data.armTexture, data.aoTexture, data.heightTexture};

        for(const auto& texture : references)
        {
            if(!texture)
            {
                continue;
            }

            const auto textureID = AssetID::ParseReference(*texture);
            const auto* textureRecord = textureID ? m_registry->Find(*textureID) : nullptr;
            const auto resolution = textureID ? ProjectAssetResolver::Resolve(paths, *m_registry, *textureID) : AssetResolution{};

            if(!textureID || textureID->ToReference() != *texture || !textureRecord ||
                textureRecord->type != AssetType::Texture || resolution.status != AssetAvailability::Available)
            {
                return Fail(error, "Metadata material has an invalid or unavailable texture reference: " + *texture);
            }
        }

        return true;
    }
    catch(const std::exception& exception)
    {
        return Fail(error, "Metadata material validation failed: " + std::string(exception.what()));
    }
}

Material* FileSceneAssetCatalog::ResolveMetadataMaterial(std::string_view reference)
{
    if(!m_textureStreamingEnabled)
    {
        m_materialError = "Metadata preparation requires the catalog's fallback texture policy.";
        return nullptr;
    }

    return ResolveMaterial(reference);
}

bool PrepareSceneMetadata(const SceneDocument& document, FileSceneAssetCatalog& catalog,
    PreparedMetadataScene& output, std::string* error)
{
    if(error)
    {
        error->clear();
    }

    if(!ValidateSceneDocumentPersistence(document, error))
    {
        return false;
    }

    // Even an empty scene must have a valid initialized canonical project context.
    if(!catalog.ValidateMetadataMeshReference({}, error))
    {
        return false;
    }

    PreparedMetadataScene candidate;
    candidate.document = document;
    candidate.objects.reserve(document.objects.size());
    std::set<std::string> checkedMaterials;

    for(const auto& authored : document.objects)
    {
        if(!catalog.ValidateMetadataMeshReference(authored.meshReference, error))
        {
            return false;
        }

        if(authored.meshReference.empty() && !authored.materialSlotBindings.empty())
        {
            return Fail(error, "An empty mesh assignment cannot have material-slot overrides: " + authored.id);
        }

        const auto reference = authored.materialReference.empty() ? "builtin:default" : authored.materialReference;

        if(checkedMaterials.insert(reference).second && !catalog.ValidateMetadataMaterialReference(reference, error))
        {
            return false;
        }

        const bool deferred = catalog.IsArchiveActivation() &&
            (AssetID::ParseReference(reference).has_value() || !authored.materialSlotBindings.empty());
        if(deferred)
        {
            ResolvedSceneObject object;
            object.id = authored.id; object.name = authored.name; object.meshBindingEmpty = authored.meshReference.empty();
            object.renderObject.materialsDeferred = true;
            object.renderObject.transform = authored.transform; object.renderObject.previousTransform = authored.transform;
            for(const auto& [slot, ref] : authored.materialSlotBindings)
            {
                if(!catalog.ValidateMetadataMaterialReference(ref,error)) return false;
                object.renderObject.slotMaterials.emplace(slot,nullptr);
            }
            candidate.objects.push_back(std::move(object));
            continue;
        }

        Material* material = catalog.ResolveMetadataMaterial(reference);

        if(!material || !catalog.OwnsMaterial(*material))
        {
            return Fail(error, "Metadata material resolution failed for object " + authored.id + ": " +
                catalog.GetLastMaterialError());
        }

        ResolvedSceneObject object;
        object.id = authored.id;
        object.name = authored.name;
        object.meshBindingEmpty = authored.meshReference.empty();

        object.renderObject.material = material;
        object.renderObject.transform = authored.transform;
        object.renderObject.previousTransform = authored.transform;

        // These are retained explicit bindings only, not an invented complete
        // mesh-slot table. Slot existence must be checked when metadata arrives.
        for(const auto& [slot, overrideReference] : authored.materialSlotBindings)
        {
            if(checkedMaterials.insert(overrideReference).second &&
                !catalog.ValidateMetadataMaterialReference(overrideReference, error))
            {
                return false;
            }

            Material* overrideMaterial = catalog.ResolveMetadataMaterial(overrideReference);

            if(!overrideMaterial || !catalog.OwnsMaterial(*overrideMaterial))
            {
                return Fail(error, "Metadata slot material resolution failed for object " + authored.id +
                    ", slot " + slot + ": " + catalog.GetLastMaterialError());
            }

            object.renderObject.slotMaterials.emplace(slot, overrideMaterial);
        }

        if(!MatchesCommittedSceneObjectBindings(authored, object.renderObject, object.renderObject))
        {
            return Fail(error, "Metadata object has an inconsistent authored/pending binding: " + authored.id);
        }

        candidate.objects.push_back(std::move(object));
    }

    output = std::move(candidate);
    return true;
}

bool CommitPreparedMetadataScene(const PreparedMetadataScene& prepared, Scene& scene,
    SceneObjectMapping& mapping, std::string* error, SceneHierarchyTopology hierarchy)
{
    if(!ValidateSceneDocumentPersistence(prepared.document, error))
    {
        return false;
    }

    if(prepared.objects.size() != prepared.document.objects.size())
    {
        return Fail(error, "Prepared metadata object count does not match its authored document.");
    }

    SceneDocument validation = prepared.document;
    SceneObjectMapping newMapping;
    std::vector<RenderObject> objects;
    objects.reserve(prepared.objects.size());
    newMapping.committedObjects.reserve(prepared.objects.size());
    newMapping.bindingRevisions.reserve(prepared.objects.size());
    newMapping.idToRuntimeIndex.reserve(prepared.objects.size());

    for(size_t index = 0; index < prepared.objects.size(); ++index)
    {
        const auto& authored = prepared.document.objects[index];
        const auto& object = prepared.objects[index];
        const auto& runtime = object.renderObject;

        if(object.id != authored.id || object.name != authored.name || runtime.mesh || (!runtime.material && !runtime.materialsDeferred) ||
            (runtime.materialsDeferred && runtime.material) ||
            object.meshBindingEmpty != authored.meshReference.empty() ||
            !MatchesCommittedSceneObjectBindings(authored, runtime, runtime))
        {
            return Fail(error, "Prepared metadata object identity or pending binding is inconsistent: " + authored.id);
        }

        if(runtime.slotMaterials.size() != authored.materialSlotBindings.size())
        {
            return Fail(error, "Prepared metadata must retain exactly the authored slot overrides: " + authored.id);
        }

        for(const auto& [slot, reference] : authored.materialSlotBindings)
        {
            const auto retained = runtime.slotMaterials.find(slot);

            if(retained == runtime.slotMaterials.end() || (runtime.materialsDeferred ? retained->second != nullptr : retained->second == nullptr))
            {
                return Fail(error, "Prepared metadata has an unresolved slot override: " + slot);
            }
        }

        validation.objects[index].transform = runtime.transform;
        newMapping.idToRuntimeIndex.emplace(authored.id, index);
        newMapping.committedObjects.push_back(runtime);
        newMapping.bindingRevisions.push_back(1);
        objects.push_back(runtime);
    }

    if(!SceneDocumentsEqual(prepared.document, validation))
    {
        return Fail(error, "Prepared metadata transforms differ from the authored document.");
    }

    std::string environment=prepared.document.environmentReference;
    // ReplaceRenderObjects allocates all dirty flags and evaluates the hierarchy before touching
    // scene storage. Everything following its commit point is assignment/swap of prepared state.
    if(!scene.ReplaceRenderObjects(std::move(objects), std::move(hierarchy), error))
    {
        return false;
    }

    Camera& camera = scene.GetCamera();
    const auto& source = prepared.document.camera;
    camera.SetPosition(source.position);
    camera.SetTarget(source.target);
    camera.SetVerticalFov(source.verticalFov);
    camera.SetAspectRatio(source.aspectRatio);

    camera.SetNearPlane(source.nearPlane);
    camera.SetFarPlane(source.farPlane);
    scene.GetDirectionalLight() = prepared.document.directionalLight;
    scene.SetEnvironmentReference(std::move(environment));

    newMapping.structureRevision = scene.GetStructureRevision();
    newMapping.scene = &scene;
    static_assert(std::is_nothrow_move_assignable_v<SceneObjectMapping>);
    mapping = std::move(newMapping);
    return true;
}
