#include "assets/CookedScene.h"

#include "DemoContent/FileBackedDemoScene.h"
#include "DemoContent/SceneRegistry.h"

#include "Scene/Camera.h"
#include "Scene/DirectionalLight.h"

#include "Core/Log.h"

#include "Project/ProjectPaths.h"
#include "assets/ProjectAssetResolver.h"
#include "assets/ProjectAssetRegistry.h"
#include "Scene/SceneDocumentCache.h"
#include "Scene/SceneSerializer.h"
#include "assets/ProjectSceneMembership.h"
#include <fstream>
#include <iterator>
#include "Renderer/Mesh.h"
#include "Renderer/Renderer.h"

#include <memory>
#include <string>
#include <utility>
#include <algorithm>

namespace
{
    std::string ProjectPathText(const std::filesystem::path& path)
    {
        const auto text = path.u8string();
        return {text.begin(), text.end()};
    }

    // Registered during static initialization, before any project exists, so the path stays
    // project-relative until SetProjectAssets anchors it to the active project root.
    std::unique_ptr<IDemoScene> CreateFileBackedTestScene()
    {
        const std::filesystem::path relativeScenePath =
            std::filesystem::path("Scenes") / "FirstFileBackedScene.scene";

        return std::make_unique<FileBackedDemoScene>(relativeScenePath);
    }

    const bool g_fileBackedTestRegistered =
        SceneRegistry::Instance().Register(
            "File-Backed Test",
            &CreateFileBackedTestScene);
}

FileBackedDemoScene::FileBackedDemoScene(
    std::filesystem::path scenePath)
    : m_scenePath(std::move(scenePath))
{
}

FileBackedDemoScene::FileBackedDemoScene(std::shared_ptr<const CookedScene::ArchiveScene> source)
    : m_archiveScene(std::move(source)) {}

FileBackedDemoScene::FileBackedDemoScene(SceneDocument unsavedDocument)
    : m_unsavedDocument(std::move(unsavedDocument))
{
}

FileBackedDemoScene::FileBackedDemoScene(std::shared_ptr<CompositeWorld> composite)
    : m_unsavedDocument(composite->document), m_compositeWorld(std::move(composite))
{
}

void FileBackedDemoScene::SetProjectAssets(
    const std::filesystem::path& root,
    const ProjectAssetRegistry& registry)
{
    m_projectRoot = root;
    m_registry = &registry;

    if(!m_scenePath.empty() && m_scenePath.is_relative())
    {
        m_scenePath = m_projectRoot / m_scenePath;
    }
}

bool FileBackedDemoScene::Initialize(
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    const Shader* vertexShader,
    const Shader* pixelShader)
{
    return InitializeMetadataOnly(device, uploader, descriptorHeap, vertexShader, pixelShader);
}

#ifdef _DEBUG
bool FileBackedDemoScene::InitializeSynchronousDiagnostic(ID3D12Device* device, ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap, const Shader* vertexShader, const Shader* pixelShader)
{
    return InitializeWithPolicy(device, uploader, descriptorHeap, vertexShader, pixelShader, false);
}
#endif

bool FileBackedDemoScene::InitializeMetadataOnly(ID3D12Device* device, ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap, const Shader* vertexShader, const Shader* pixelShader)
{
    return InitializeWithPolicy(device, uploader, descriptorHeap, vertexShader, pixelShader, true);
}

bool FileBackedDemoScene::InitializeWithPolicy(ID3D12Device* device, ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap, const Shader* vertexShader, const Shader* pixelShader, bool metadataOnly)
{
    if (m_initialized || m_stagingStep != StagingStep::Idle)
    {
        return false;
    }

    BeginStagedInitialization(device, uploader, descriptorHeap, vertexShader, pixelShader);
    m_stagingInputs.metadataOnly = metadataOnly;
    bool done = false;

    while (!done)
    {
        if (!AdvanceStagedInitialization(done))
        {
            return false;
        }
    }

    return true;
}

void FileBackedDemoScene::BeginStagedInitialization(ID3D12Device* device, ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap, const Shader* vertexShader, const Shader* pixelShader)
{
    m_stagingInputs = {device, &uploader, &descriptorHeap, vertexShader, pixelShader, true};
    m_stagingStep = m_initialized ? StagingStep::Idle : StagingStep::Open;
}

bool FileBackedDemoScene::AdvanceStagedInitialization(bool& done)
{
    done = false;
    const bool metadataOnly = m_stagingInputs.metadataOnly;
    std::string error;

    // Any failure leaves the candidate for its stager to shut down; the step is reset so it cannot resume.
    const auto fail = [&]()
    {
        m_stagingStep = StagingStep::Idle;
        Shutdown();
        return false;
    };

    switch (m_stagingStep)
    {
    case StagingStep::Open:
    {
        // Initialization is performed while this scene is inactive. Engine remains responsible for GPU synchronization
        // when replacing a previously active scene.
        Shutdown();

        Log::Info(std::string("Preparing file scene activation: mode=") +
            (metadataOnly ? "metadata-first" : "synchronous-full Debug diagnostic") + ", path=" + ProjectPathText(m_scenePath));

        // Validate the document before allocating GPU resources.
        bool opened = false;

        if (m_archiveScene)
        {
            opened = m_runtime->LoadArchive(m_archiveScene, &error);
        }
        else if (m_unsavedDocument)
        {
            opened = m_runtime->LoadUnsavedDocument(*m_unsavedDocument, &error);

            // W5: a composite World is edited in place; the runtime checks every edited candidate against the ownership rules
            // and gives new entities the editing context's level.
            if(opened && m_compositeWorld)
            {
                m_editingLevel = m_compositeWorld->worldID;
                m_runtime->SetCompositeHooks(
                    [this](const SceneDocument& candidate, std::string& error)
                    {
                        return ValidateCompositeEdit(*m_compositeWorld, candidate, error);
                    },
                    [this](SceneObjectDocument& object)
                    {
                        AssignCompositeEntity(object);
                    });
            }
        }
        else
        {
            opened = m_runtime->Open(m_scenePath, &error);
        }

        if (!opened)
        {
            Log::Error("Failed to open file-backed scene: " + error);
            m_stagingStep = StagingStep::Idle;
            return false;
        }

        m_stagingStep = StagingStep::Assets;
        return true;
    }

    case StagingStep::Assets:
        if (!m_assets.Initialize(m_stagingInputs.device, *m_stagingInputs.uploader, *m_stagingInputs.descriptorHeap,
                m_stagingInputs.vertexShader, m_stagingInputs.pixelShader))
        {
            Log::Error("Failed to initialize file-backed scene assets.");
            return fail();
        }

        if (m_archiveScene)
        {
            m_assets.SetArchiveScene(m_archiveScene);
        }
        else if (m_registry)
        {
            m_assets.SetProjectAssets(ProjectPaths(m_projectRoot), *m_registry);
        }

        m_assets.SetTextureStreamingEnabled(m_textureStreamingEnabled || metadataOnly);
        m_assets.SetMeshCPUCache(m_meshCPUCache);
        m_stagingStep = StagingStep::Prepare;
        return true;

    case StagingStep::Prepare:
        if (!(metadataOnly ? m_runtime->PrepareMetadata(m_assets, &error) : m_runtime->Prepare(m_assets, &error)))
        {
            Log::Error("Failed to prepare file-backed scene: " + error + " " +
                m_assets.GetLastMeshError() + " " + m_assets.GetLastMaterialError());
            return fail();
        }

        m_stagingStep = StagingStep::Commit;
        return true;

    case StagingStep::Commit:
        // The commit re-verifies the registry and disk against the preparation, so frames between the steps are safe.
        if (!(metadataOnly ? m_runtime->CommitMetadata(m_assets, &error) : m_runtime->Commit(&error)))
        {
            Log::Error("Failed to commit file-backed scene: " + error);
            return fail();
        }

        m_stagingStep = StagingStep::Idle;
        m_initialized = true;
        m_metadataOnlyActivation = metadataOnly;

        // W3: resolve the preview environment before the Engine requests the scene environment, so the first request is the
        // effective sky (no request for a sky that is immediately replaced).
        ReadEnvironmentWorld();
        RefreshEnvironmentPreview(false);
        m_publishedEnvironmentReference = m_runtime->GetScene().GetEnvironmentReference();
        done = true;
        return true;

    case StagingStep::Idle:
    default:
        return false;
    }
}

void FileBackedDemoScene::Shutdown()
{
    // This must only run after Engine has safely released any
    // renderer resources referencing this scene.
    m_initialized = false;
    m_metadataOnlyActivation = false;

    m_runtime->DiscardPreparedScene();
    m_runtime->GetScene().ClearRenderObjects();
    m_objectMutationCandidate.reset();

    // The catalog owns the objects referenced by RenderObject.
    m_assets.Shutdown();
}

void FileBackedDemoScene::Update(float deltaTime)
{
    (void)deltaTime;
}

void FileBackedDemoScene::AdvanceTextureStreaming(FileSceneAssetCatalog::TextureStreamingPass pass, double expensiveBudget,
    bool forceExpensive, ProjectTexturePreparationService& preparation, Renderer& renderer,
    const std::function<bool(const AssetID&, const MaterialAssetData&)>& mayPublish)
{
    if(m_archiveScene || !m_initialized || m_objectMutationCandidate || m_runtime->HasMaterialAssignment())
    {
        return;
    }

    m_assets.AdvanceTextureStreaming(preparation, renderer, GetRuntimeIdentity(), GetScene(), mayPublish, pass, expensiveBudget,
        forceExpensive);
}

Scene& FileBackedDemoScene::GetScene()
{
    return m_runtime->GetScene();
}

const Scene& FileBackedDemoScene::GetScene() const
{
    return m_runtime->GetScene();
}

std::vector<Material*> FileBackedDemoScene::GetMaterials()
{
    return m_assets.GetMaterials();
}

void FileBackedDemoScene::ResetCamera()
{
    if (!m_runtime->GetFileScene().HasDocument())
    {
        return;
    }

    const SceneCameraDocument& source =
        m_runtime->GetFileScene().GetDocument().camera;

    Camera& camera = m_runtime->GetScene().GetCamera();

    camera.SetPosition(source.position);
    camera.SetTarget(source.target);
    camera.SetVerticalFov(source.verticalFov);
    camera.SetAspectRatio(source.aspectRatio);
    camera.SetNearPlane(source.nearPlane);
    camera.SetFarPlane(source.farPlane);
}

void FileBackedDemoScene::ResetDirectionalLight()
{
    if (!m_runtime->GetFileScene().HasDocument())
    {
        return;
    }

    // Directional defaults belong to object components; this compatibility hook resets ambient only.
    m_runtime->GetScene().SetLightingSettings(m_runtime->GetFileScene().GetDocument().lightingSettings);
}

void FileBackedDemoScene::SetAspectRatio(float aspectRatio)
{
    m_runtime->GetScene().GetCamera().SetAspectRatio(aspectRatio);
}

const std::string& FileBackedDemoScene::GetDisplayName() const noexcept
{
    return m_runtime->GetFileScene().GetDocument().name;
}

bool FileBackedDemoScene::ReorderObjects(const SceneObjectOrderIdentity& identity,
    const std::vector<std::string>& expectedOrder, const std::vector<std::string>& replacementOrder,
    std::string* errorMessage)
{
    if(!m_initialized || m_objectMutationCandidate)
    {
        if(errorMessage)
        {
            *errorMessage = "Reordering requires an initialized scene without a pending structural candidate.";
        }

        return false;
    }

    return m_runtime->ReorderObjects(identity, expectedOrder, replacementOrder, errorMessage);
}

bool FileBackedDemoScene::SetObjectPhysics(const SceneObjectMetadata& selection,
    const std::optional<Physics::PhysicsComponent>& expected,
    const std::optional<Physics::PhysicsComponent>& replacement, std::string* errorMessage)
{
    if(!m_initialized || m_objectMutationCandidate)
    {
        if(errorMessage) *errorMessage = "Physics editing requires an initialized scene without pending structure changes.";
        return false;
    }
    return m_runtime->SetObjectPhysics(selection, expected, replacement, errorMessage);
}

bool FileBackedDemoScene::SetObjectTriggerCollider(const SceneObjectMetadata& selection,
    const std::optional<Physics::TriggerColliderComponent>& expected,
    const std::optional<Physics::TriggerColliderComponent>& replacement, std::string* errorMessage)
{
    if(!m_initialized || m_objectMutationCandidate)
    {
        if(errorMessage)
        {
            *errorMessage = "Trigger Collider editing requires an initialized scene without pending structure changes.";
        }

        return false;
    }

    return m_runtime->SetObjectTriggerCollider(selection, expected, replacement, errorMessage);
}

bool FileBackedDemoScene::RenameObject(const SceneObjectMetadata& selection, std::string_view expectedName,
    std::string name, std::string* errorMessage, SceneObjectNamePolicy policy)
{
    if(!m_initialized || m_objectMutationCandidate)
    {
        if(errorMessage)
        {
            *errorMessage = "Renaming requires an initialized scene without a pending structural candidate.";
        }

        return false;
    }

    return m_runtime->RenameObject(selection, expectedName, std::move(name), errorMessage, policy);
}

bool FileBackedDemoScene::BuildHierarchySnapshot(
    SceneHierarchySnapshot& snapshot, std::string* errorMessage) const
{
    if(!m_initialized)
    {
        if(errorMessage)
        {
            *errorMessage = "Scene hierarchy is unavailable before scene initialization.";
        }

        return false;
    }

    return m_runtime->BuildHierarchySnapshot(snapshot, errorMessage);
}

std::shared_ptr<const SceneHierarchySnapshot> FileBackedDemoScene::GetHierarchySnapshot(std::string* errorMessage) const
{
    if(!m_initialized)
    {
        if(errorMessage)
        {
            *errorMessage = "Scene hierarchy is unavailable before scene initialization.";
        }

        return nullptr;
    }

    return m_runtime->GetHierarchySnapshot(errorMessage);
}

std::optional<SceneObjectMetadata> FileBackedDemoScene::GetObjectMetadata(
    RenderObjectID runtimeIndex,
    std::string* errorMessage) const
{
    if(!m_initialized)
    {
        if(errorMessage)
        {
            *errorMessage = "File-backed scene metadata is unavailable before successful initialization or after shutdown.";
        }

        return std::nullopt;
    }

    return m_runtime->GetObjectMetadata(runtimeIndex, errorMessage);
}

bool FileBackedDemoScene::Save(std::string* errorMessage)
{
    if (!m_initialized)
    {
        if (errorMessage)
        {
            *errorMessage = "Cannot save an uninitialized file-backed scene.";
        }

        return false;
    }

    if(m_compositeWorld)
    {
        if(errorMessage)
        {
            *errorMessage = "A World preview cannot be saved: the World and its Local Levels are saved from their own documents.";
        }

        return false;
    }

    if (IsUnsaved())
    {
        if (errorMessage)
        {
            *errorMessage = "This scene has never been saved; a file path is required.";
        }

        return false;
    }

    return m_runtime->Save(errorMessage);
}

bool FileBackedDemoScene::SaveAs(const std::filesystem::path& path, std::string* errorMessage, bool replaceExisting)
{
    if (!m_initialized || m_archiveScene || m_compositeWorld)
    {
        if (errorMessage)
        {
            *errorMessage = m_compositeWorld ? "A World preview cannot be saved as a scene: it is composed from the World and its "
                "Local Levels, which keep their own documents." : "Save As requires an initialized, editable file-backed scene.";
        }

        return false;
    }

    if(path.empty() || !path.is_absolute() || !ProjectAssetRegistry::PathsEqual(path.extension(), ".scene"))
    {
        if (errorMessage)
        {
            *errorMessage = "Save As requires an absolute .scene file path.";
        }

        return false;
    }

    // Allocate the wrapper's adopted path before persistence, as FileScene does internally.
    std::filesystem::path savedPath = path;

    if(!m_runtime->SaveAs(savedPath, errorMessage, replaceExisting))
    {
        return false;
    }

    // The runtime adopted the path only after the transactional write succeeded.
    m_scenePath = std::move(savedPath);
    m_unsavedDocument.reset();
    return true;
}

std::optional<SceneObjectMetadata> FileBackedDemoScene::GetObjectMetadata(
    std::string_view objectID, std::string* errorMessage) const
{
    if(!m_initialized)
    {
        if(errorMessage)
        {
            *errorMessage = "Persistent object lookup requires an initialized file-backed scene.";
        }

        return std::nullopt;
    }

    return m_runtime->GetObjectMetadata(objectID, errorMessage);
}

std::optional<MaterialAssignmentToken> FileBackedDemoScene::PrepareMaterialAssignment(
    const SceneObjectMetadata& selection, std::string materialReference, std::string* errorMessage)
{
    if(!m_initialized)
    {
        if(errorMessage)
        {
            *errorMessage = "Material assignment requires an initialized file-backed scene.";
        }

        return std::nullopt;
    }

    return m_runtime->PrepareMaterialAssignment(selection, std::move(materialReference), errorMessage);
}

std::optional<MaterialAssignmentToken> FileBackedDemoScene::PrepareMaterialSlotAssignment(
    const SceneObjectMetadata& selection, std::string slotID, const std::optional<AssetID>& materialID,
    std::string* errorMessage)
{
    const auto fail = [&](const std::string& message) -> std::optional<MaterialAssignmentToken>
    {
        if(errorMessage)
        {
            *errorMessage = message;
        }

        return std::nullopt;
    };

    if(!m_initialized || m_runtime->HasMaterialAssignment() || m_runtime->HasPreparedScene())
    {
        return fail("Material slot assignment requires an initialized scene with no pending preparation/assignment.");
    }

    const auto metadata = GetObjectMetadata(selection.objectID, errorMessage);

    if(!metadata || metadata->runtimeIndex != selection.runtimeIndex || metadata->sceneID != selection.sceneID ||
        metadata->runtimeIdentity != selection.runtimeIdentity || metadata->structureRevision != selection.structureRevision ||
        metadata->meshReference != selection.meshReference || metadata->materialReference != selection.materialReference ||
        metadata->materialSlotBindings != selection.materialSlotBindings || metadata->bindingRevision != selection.bindingRevision)
    {
        return fail("Material slot assignment requires the current persistent object, mesh, and binding snapshot.");
    }

    const auto& object = m_runtime->GetScene().GetRenderObjects()[metadata->runtimeIndex];

    const auto* slotMetadata = object.mesh ? &object.mesh->GetMetadata() :
        m_assets.FindRetainedSlotMetadata(metadata->meshReference);

    if (!slotMetadata)
    {
        return fail("Material slots await verified mesh metadata; authored overrides are retained.");
    }

    const auto& slots = slotMetadata->materialSlots;

    if(std::none_of(slots.begin(), slots.end(), [&](const auto& slot) { return slot.id == slotID; }))
    {
        return fail("Material slot does not exist on the object's current mesh: " + slotID);
    }

    Material* effective = object.material;

    if(materialID)
    {
        const auto* record = m_registry ? m_registry->Find(*materialID) : nullptr;

        if(!record || record->type != AssetType::Material ||
            ProjectAssetResolver::Resolve(ProjectPaths(m_projectRoot), *m_registry, *materialID).status != AssetAvailability::Available)
        {
            return fail("Slot override requires an available registered material AssetID.");
        }

        const auto existing = metadata->materialSlotBindings.find(slotID);

        if(existing != metadata->materialSlotBindings.end() && existing->second == materialID->ToReference())
        {
            // An unchanged binding reuses its resolved, retained material version.
            effective = object.GetSlotMaterial(slotID);
        }
        else
        {
            effective = object.mesh ? ResolveAssignmentMaterial(materialID->ToReference(), errorMessage) :
                ResolvePendingAssignmentMaterial(materialID->ToReference(), errorMessage);

            if(!effective)
            {
                return std::nullopt;
            }
        }
    }

    if(!effective || !m_assets.OwnsMaterial(*effective))
    {
        return fail("The effective slot material is unavailable or not retained by this scene's catalog.");
    }

    return m_runtime->PrepareMaterialSlotAssignment(selection, std::move(slotID), materialID, *effective, errorMessage, &m_assets);
}

Material* FileBackedDemoScene::GetPreparedMaterialAssignmentMaterial(const MaterialAssignmentToken& token) const noexcept
{
    return m_initialized ? m_runtime->GetPreparedMaterialAssignmentMaterial(token) : nullptr;
}

Material* FileBackedDemoScene::ResolveAssignmentMaterial(std::string_view reference, std::string* errorMessage)
{
    if(errorMessage)
    {
        errorMessage->clear();
    }

    Material* material = m_initialized ? m_assets.ResolveFreshMaterial(reference) : nullptr;

    if(!material && errorMessage)
    {
        *errorMessage = m_initialized ? m_assets.GetLastMaterialError() : "File-backed scene is not initialized.";
    }

    return material;
}

bool FileBackedDemoScene::CommitMaterialAssignment(
    const MaterialAssignmentToken& token, Material& sceneOwnedMaterial, std::string* errorMessage)
{
    if(!m_initialized || !m_assets.OwnsMaterial(sceneOwnedMaterial))
    {
        if(errorMessage)
        {
            *errorMessage = "Assigned material must be retained by this initialized scene's asset catalog.";
        }

        return false;
    }

    return m_runtime->CommitMaterialAssignment(token, sceneOwnedMaterial, errorMessage);
}

bool FileBackedDemoScene::RollbackMaterialAssignment(const MaterialAssignmentToken& token, std::string* errorMessage)
{
    if(!m_initialized)
    {
        if(errorMessage)
        {
            *errorMessage = "Material assignment requires an initialized file-backed scene.";
        }

        return false;
    }

    return m_runtime->RollbackMaterialAssignment(token, errorMessage);
}

bool FileBackedDemoScene::FinishMaterialAssignment(const MaterialAssignmentToken& token, std::string* errorMessage)
{
    if(!m_initialized)
    {
        if(errorMessage)
        {
            *errorMessage = "Material assignment requires an initialized file-backed scene.";
        }

        return false;
    }

    return m_runtime->FinishMaterialAssignment(token, errorMessage);
}

bool FileBackedDemoScene::CancelMaterialAssignment(const MaterialAssignmentToken& token, std::string* errorMessage)
{
    if(!m_initialized)
    {
        if(errorMessage)
        {
            *errorMessage = "Material assignment requires an initialized file-backed scene.";
        }

        return false;
    }

    return m_runtime->CancelMaterialAssignment(token, errorMessage);
}

bool FileBackedDemoScene::CheckUnsavedChanges(
    bool& dirty,
    std::string* errorMessage) const
{
    // W5: a composite World is unsaved when any of its source documents differs from its baseline.
    if(m_compositeWorld)
    {
        std::vector<bool> documents;

        if(!GetCompositeDirtyDocuments(documents, errorMessage))
        {
            return false;
        }

        dirty = std::ranges::find(documents, true) != documents.end();
        return true;
    }

    return m_runtime->CheckUnsavedChanges(dirty, errorMessage);
}

bool FileBackedDemoScene::ProjectComposite(CompositeWorldProjection& projection, std::string* errorMessage) const
{
    SceneDocument captured;
    std::string error;

    if(!m_compositeWorld || !m_runtime->CaptureDocumentForSave(captured, &error) ||
        !ProjectCompositeDocuments(*m_compositeWorld, captured, projection, error))
    {
        if(errorMessage)
        {
            *errorMessage = error.empty() ? std::string("No composite World is open.") : error;
        }

        return false;
    }

    return true;
}

bool FileBackedDemoScene::GetCompositeDirtyDocuments(std::vector<bool>& dirty, std::string* errorMessage) const
{
    CompositeWorldProjection projection;

    if(!ProjectComposite(projection, errorMessage))
    {
        return false;
    }

    dirty = DirtyCompositeDocuments(*m_compositeWorld, projection);
    return true;
}

bool FileBackedDemoScene::SaveCompositeDocuments(const ProjectPaths& paths, ProjectAssetRegistry& registry,
    std::optional<std::size_t> onlyDocument, std::size_t& saved, std::string* errorMessage)
{
    saved = 0;
    CompositeWorldProjection projection;

    if(!ProjectComposite(projection, errorMessage))
    {
        return false;
    }

    // Document 0 is the World; document i + 1 is levels[i]. Only dirty documents are written, each against its baseline bytes.
    const auto dirty = DirtyCompositeDocuments(*m_compositeWorld, projection);
    std::vector<ProjectSceneMembership::DocumentSave> saves;
    std::vector<std::size_t> indices;

    for(std::size_t index = 0; index < dirty.size(); ++index)
    {
        if(!dirty[index] || (onlyDocument && *onlyDocument != index))
        {
            continue;
        }

        const auto& baseline = index == 0 ? m_compositeWorld->worldSource : m_compositeWorld->localSources[index - 1];
        const auto& document = index == 0 ? projection.world : projection.locals[index - 1];
        saves.push_back({baseline.relativePath, document, baseline.bytes});
        indices.push_back(index);
    }

    if(saves.empty())
    {
        return true;
    }

    ProjectSceneMembership::Result result;
    std::string error;

    if(!ProjectSceneMembership::SaveSceneDocuments(paths, registry, saves, result, error))
    {
        if(errorMessage)
        {
            *errorMessage = error;
        }

        return false;
    }

    // Committed: the written documents become the new baselines (exactly the bytes the transaction wrote).
    for(std::size_t entry = 0; entry < saves.size(); ++entry)
    {
        auto& baseline = indices[entry] == 0 ? m_compositeWorld->worldSource : m_compositeWorld->localSources[indices[entry] - 1];
        baseline.document = saves[entry].document;
        SerializeSceneDocument(baseline.document, baseline.bytes);
    }

    for(std::size_t index = 0; index < m_compositeWorld->levels.size(); ++index)
    {
        m_compositeWorld->levels[index].placement = m_compositeWorld->worldSource.document.members[index].placement;
    }

    saved = saves.size();
    return true;
}

bool FileBackedDemoScene::SetEditingLevel(const std::string& levelID)
{
    if(!m_compositeWorld)
    {
        return false;
    }

    const bool known = levelID == m_compositeWorld->worldID || std::ranges::any_of(m_compositeWorld->levels,
        [&levelID](const CompositeWorld::Level& level)
        {
            return level.id == levelID;
        });

    if(known)
    {
        m_editingLevel = levelID;
    }

    return known;
}

// New entities: the level of their parent when they have one (duplicates stay beside their source), otherwise the editing context;
// a Local Level's new root entity is parented to its level root. Identities that already carry a level (restored deletions) are kept.
void FileBackedDemoScene::AssignCompositeEntity(SceneObjectDocument& object) const
{
    if(!m_compositeWorld || object.id.empty() || object.id.find('/') != std::string::npos)
    {
        return;
    }

    std::string level = m_editingLevel;

    if(!object.parentID.empty())
    {
        const auto separator = object.parentID.find('/');
        level = separator == std::string::npos ? object.parentID : object.parentID.substr(0, separator);
    }

    object.id = CompositeWorld::RuntimeID(level, object.id);

    if(object.parentID.empty() && level != m_compositeWorld->worldID)
    {
        object.parentID = level;
        object.parentIsLevelNode = true;
    }
}

std::optional<RuntimeMaterialVersions> FileBackedDemoScene::GetMaterialVersions(
    const std::filesystem::path& canonicalProject, const AssetID& id, std::string* errorMessage) const
{
    if(!m_initialized)
    {
        if(errorMessage)
        {
            *errorMessage = "Scene material identity requires an initialized file-backed scene.";
        }

        return std::nullopt;
    }

    auto result = m_assets.GetMaterialVersions(canonicalProject, id, errorMessage);

    if(result)
    {
        result->runtimeIdentity = m_runtime->GetRuntimeIdentity();
        result->structureRevision = m_runtime->GetScene().GetStructureRevision();
    }

    return result;
}

bool FileBackedDemoScene::PublishSavedMaterialParameters(const RuntimeMaterialVersions& expected,
    const MaterialAssetData& data, std::vector<GPUMaterial> table,
    const std::function<bool(const std::vector<GPUMaterial>&)>& publisher, std::string* errorMessage)
{
    const auto current = GetMaterialVersions(expected.projectIdentity, expected.assetID);

    if(!current || !expected.Matches(*current) || m_runtime->HasMaterialAssignment())
    {
        if(errorMessage)
        {
            *errorMessage = "Saved material update rejected: scene identity, mapping, or assignment state changed.";
        }

        return false;
    }

    return m_assets.PublishSavedMaterialParameters(expected, data, std::move(table), publisher, errorMessage);
}

SavedMaterialRevisionResult FileBackedDemoScene::AdvanceSavedMaterialRevision(const RuntimeMaterialVersions& expected,
    const MaterialAssetData& data, const std::string& sourceIdentity, Renderer& renderer,
    const std::function<bool()>& sourceStillCurrent, std::string& error)
{
    const auto current = GetMaterialVersions(expected.projectIdentity, expected.assetID);

    if(!current || !expected.Matches(*current) || m_runtime->HasMaterialAssignment())
    {
        CancelSavedMaterialRevision();
        error = "Saved revision rejected: scene or material assignment identity changed.";
        return SavedMaterialRevisionResult::Rejected;
    }

    return m_assets.AdvanceSavedMaterialRevision(expected, data, sourceIdentity, renderer, sourceStillCurrent, error);
}

void FileBackedDemoScene::CancelSavedMaterialRevision()
{
    m_assets.CancelSavedMaterialRevision();
}

bool FileBackedDemoScene::PublishWorkingMaterialParameters(const RuntimeMaterialVersions& expected,
    const std::optional<MaterialAssetData>& parameters, Renderer& renderer, std::string& error)
{
    if(!m_initialized || expected.runtimeIdentity != m_runtime->GetRuntimeIdentity() ||
       expected.structureRevision != m_runtime->GetScene().GetStructureRevision() || m_runtime->HasMaterialAssignment())
    {
        error = "Working material update rejected: scene or assignment identity changed.";
        return false;
    }

    return m_assets.PublishWorkingMaterialParameters(expected, parameters, renderer, error);
}

std::unique_ptr<FileSceneRuntime> FileBackedDemoScene::PrepareObjectRestoration(
    const SceneObjectDocument& object, size_t insertionIndex, std::string* errorMessage)
{
    return m_runtime->PrepareObjectRestoration(m_assets, object, insertionIndex, errorMessage);
}

bool FileBackedDemoScene::IsAssignmentMaterialAdmitted(std::string_view reference, const Renderer& renderer)
{
    return m_assets.FindPublishedMaterial(reference, renderer) != nullptr;
}

Material* FileBackedDemoScene::ResolvePendingAssignmentMaterial(std::string_view reference, std::string* errorMessage)
{
    Material* material = m_assets.FindRetainedMaterial(reference);

    if (!material || !m_assets.ValidateRetainedResidencyMaterial(reference, material, errorMessage))
    {
        if (errorMessage && errorMessage->empty())
        {
            *errorMessage = "Unsupported pending assignment: material is not retained; deferred material-table growth is required.";
        }

        return nullptr;
    }

    return material;
}

bool FileBackedDemoScene::SetObjectCharacterController(const SceneObjectMetadata& selection,
    const std::optional<Physics::CharacterControllerComponent>& expected,
    const std::optional<Physics::CharacterControllerComponent>& replacement, std::string* errorMessage)
{
    if(!m_initialized || m_objectMutationCandidate)
    {
        if(errorMessage)
        {
            *errorMessage = "Character Controller editing requires an initialized scene without pending structure changes.";
        }

        return false;
    }

    return m_runtime->SetObjectCharacterController(selection, expected, replacement, errorMessage);
}

bool FileBackedDemoScene::SetObjectCameras(std::span<const SceneCameraChange> changes, std::string* errorMessage)
{
    if(!m_initialized || m_objectMutationCandidate)
    {
        if(errorMessage)
        {
            *errorMessage = "Camera editing requires an initialized scene without pending structure changes.";
        }

        return false;
    }

    return m_runtime->SetObjectCameras(changes, errorMessage);
}

bool FileBackedDemoScene::SetObjectLight(const SceneObjectMetadata& selection,
    const std::optional<LightComponent>& expected, const std::optional<LightComponent>& replacement,
    std::string* errorMessage)
{
    if(!m_initialized || m_objectMutationCandidate)
    {
        if(errorMessage)
        {
            *errorMessage = "Light editing requires an initialized scene without pending structure changes.";
        }

        return false;
    }

    return m_runtime->SetObjectLight(selection, expected, replacement, errorMessage);
}

bool FileBackedDemoScene::SetObjectAirMotor(const SceneObjectMetadata& selection,
    const std::optional<AirMotorComponent>& expected, const std::optional<AirMotorComponent>& replacement,
    std::string* errorMessage)
{
    if(!m_initialized || m_objectMutationCandidate)
    {
        if(errorMessage)
        {
            *errorMessage = "Air Motor editing requires an initialized scene without pending structure changes.";
        }

        return false;
    }

    return m_runtime->SetObjectAirMotor(selection, expected, replacement, errorMessage);
}

bool FileBackedDemoScene::SetObjectMistEmitter(const SceneObjectMetadata& selection,
    const std::optional<MistEmitterComponent>& expected, const std::optional<MistEmitterComponent>& replacement,
    std::string* errorMessage)
{
    if(!m_initialized || m_objectMutationCandidate)
    {
        if(errorMessage)
        {
            *errorMessage = "Mist Emitter editing requires an initialized scene without pending structure changes.";
        }

        return false;
    }

    return m_runtime->SetObjectMistEmitter(selection, expected, replacement, errorMessage);
}

bool FileBackedDemoScene::SetObjectScript(const SceneObjectMetadata& selection,
    const std::optional<ScriptComponent>& expected, const std::optional<ScriptComponent>& replacement,
    std::string* errorMessage)
{
    if(!m_initialized || m_objectMutationCandidate)
    {
        if(errorMessage)
        {
            *errorMessage = "Script editing requires an initialized scene without pending structure changes.";
        }

        return false;
    }

    return m_runtime->SetObjectScript(selection, expected, replacement, errorMessage);
}

bool FileBackedDemoScene::SetObjectAudio(const SceneObjectMetadata& selection, const AudioComponentState& expected,
    const AudioComponentState& replacement, std::string* errorMessage)
{
    if(!m_initialized || m_objectMutationCandidate)
    {
        if(errorMessage)
        {
            *errorMessage = "Audio editing requires an initialized scene without pending structure changes.";
        }

        return false;
    }

    return m_runtime->SetObjectAudio(selection, expected, replacement, errorMessage);
}

bool FileBackedDemoScene::SetObjectsVoxelSettings(const std::vector<FileSceneRuntime::VoxelSettingsChange>& changes, std::string* errorMessage)
{
    if(!m_initialized || m_objectMutationCandidate)
    {
        if(errorMessage)
        {
            *errorMessage = "The scene is not ready for voxel edits.";
        }

        return false;
    }

    return m_runtime->SetObjectsVoxelSettings(changes, errorMessage);
}

bool FileBackedDemoScene::SetSceneVoxelBake(const std::optional<VoxelBakeSettings>& expected, const std::optional<VoxelBakeSettings>& replacement,
    std::string* errorMessage)
{
    if(!m_initialized || m_objectMutationCandidate)
    {
        if(errorMessage)
        {
            *errorMessage = "The scene is not ready for voxel edits.";
        }

        return false;
    }

    return m_runtime->SetSceneVoxelBake(expected, replacement, errorMessage);
}

bool FileBackedDemoScene::SetSceneEnvironmentalHaze(const std::optional<EnvironmentalHazeSettings>& expected,
    const std::optional<EnvironmentalHazeSettings>& replacement, std::string* errorMessage)
{
    if(!m_initialized || m_objectMutationCandidate)
    {
        if(errorMessage)
        {
            *errorMessage = "The scene is not ready for environmental haze edits.";
        }

        return false;
    }

    return m_runtime->SetSceneEnvironmentalHaze(expected, replacement, errorMessage);
}

bool FileBackedDemoScene::SetSceneAirEnvironment(const std::optional<AirEnvironmentSettings>& expected,
    const std::optional<AirEnvironmentSettings>& replacement, std::string* errorMessage)
{
    if(!m_initialized || m_objectMutationCandidate)
    {
        if(errorMessage)
        {
            *errorMessage = "The scene is not ready for air environment edits.";
        }

        return false;
    }

    return m_runtime->SetSceneAirEnvironment(expected, replacement, errorMessage);
}

bool FileBackedDemoScene::SetSceneAirSimulation(const std::optional<AirSimulationSettings>& expected,
    const std::optional<AirSimulationSettings>& replacement, std::string* errorMessage)
{
    if(!m_initialized || m_objectMutationCandidate)
    {
        if(errorMessage)
        {
            *errorMessage = "The scene is not ready for AIR simulation edits.";
        }

        return false;
    }

    return m_runtime->SetSceneAirSimulation(expected, replacement, errorMessage);
}

bool FileBackedDemoScene::SetSceneMistRendering(const std::optional<MistRenderSettings>& expected,
    const std::optional<MistRenderSettings>& replacement, std::string* errorMessage)
{
    if(!m_initialized || m_objectMutationCandidate)
    {
        if(errorMessage)
        {
            *errorMessage = "The scene is not ready for mist rendering edits.";
        }

        return false;
    }

    return m_runtime->SetSceneMistRendering(expected, replacement, errorMessage);
}

bool FileBackedDemoScene::SetObjectAcoustics(const SceneObjectMetadata& selection, const std::optional<AcousticSettings>& expected,
    const std::optional<AcousticSettings>& replacement, std::string* errorMessage)
{
    if(!m_initialized || m_objectMutationCandidate)
    {
        if(errorMessage)
        {
            *errorMessage = "The scene is not ready for acoustic edits.";
        }

        return false;
    }

    return m_runtime->SetObjectAcoustics(selection, expected, replacement, errorMessage);
}

bool FileBackedDemoScene::SetObjectProbeVolume(const SceneObjectMetadata& selection, const std::optional<ProbeVolumeComponent>& expected,
    const std::optional<ProbeVolumeComponent>& replacement, std::string* errorMessage)
{
    if(!m_initialized || m_objectMutationCandidate)
    {
        if(errorMessage)
        {
            *errorMessage = "The scene is not ready for Probe Volume edits.";
        }

        return false;
    }

    return m_runtime->SetObjectProbeVolume(selection, expected, replacement, errorMessage);
}

bool FileBackedDemoScene::ValidateObjectTransform(RenderObjectID index, const Transform& transform, std::string& error) const
{
    const auto identity = m_runtime->GetObjectMetadata(index, &error);

    if(!identity)
    {
        return false;
    }

    for(const auto& object : m_runtime->GetFileScene().GetDocument().objects)
    {
        if(object.id == identity->objectID)
        {
            return !object.characterController ||
                Physics::ValidateCharacterControllerTransform(*object.characterController, transform, &error);
        }
    }

    error = "Transform target is absent from the authored scene.";
    return false;
}

void FileBackedDemoScene::ReadEnvironmentWorld()
{
    m_environmentWorld.reset();
    m_environmentWorldProblem.clear();
    m_worldHasDirectionalLights = false;

    if(m_archiveScene || !m_runtime->GetFileScene().HasDocument())
    {
        return;
    }

    const auto& document = m_runtime->GetFileScene().GetDocument();

    if(document.kind != SceneKind::Local || document.owningWorld.empty())
    {
        return;
    }

    // The World is read as a document only: its entities are never loaded into this preview.
    const auto id = AssetID::Parse(document.owningWorld);
    const auto* record = id && m_registry ? m_registry->Find(*id) : nullptr;

    if(!record || record->type != AssetType::Scene)
    {
        m_environmentWorldProblem = "The owning World " + document.owningWorld + " is not a registered scene.";
        return;
    }

    std::ifstream file(m_projectRoot / record->relativePath, std::ios::binary);
    const std::string bytes = file ? std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()) : std::string{};

    if(!file.is_open() || file.bad())
    {
        m_environmentWorldProblem = "The owning World " + record->relativePath.generic_string() + " cannot be read.";
        return;
    }

    SceneDocumentCache::Parsed world;
    std::string error;

    if(!SceneDocumentCache::Parse(m_projectRoot / record->relativePath, bytes, world, &error, "Owning World environment"))
    {
        m_environmentWorldProblem = "The owning World " + record->relativePath.generic_string() + " is not a valid scene: " + error;
        return;
    }

    const auto& objects = world.document->objects;
    m_worldHasDirectionalLights = std::any_of(objects.begin(), objects.end(), [](const SceneObjectDocument& object)
    {
        return object.light && object.light->type == LightType::Directional;
    });
    m_environmentWorld = SceneDocumentCache::Copy(*world.document);
}

bool FileBackedDemoScene::RefreshEnvironmentPreview(bool rereadWorld)
{
    if(!m_runtime->GetFileScene().HasDocument())
    {
        return false;
    }

    if(rereadWorld)
    {
        ReadEnvironmentWorld();
    }

    const auto& document = m_runtime->GetFileScene().GetDocument();
    auto& scene = m_runtime->GetScene();

    // Resolution input: the environment fields only (never the objects), with the live sky and exposure of overridden groups
    // (they are edited in the runtime Scene until saved). This runs every Editor frame, so the member input is reassigned,
    // reusing its string capacity, rather than rebuilt.
    SceneDocument& input = m_environmentInput;
    input.kind = document.kind;
    input.id = document.id;
    input.name = document.name;
    input.owningWorld = document.owningWorld;
    input.environmentOverrides = document.environmentOverrides;
    input.environmentReference = document.environmentOverrides.sky ? scene.GetEnvironmentReference() : document.environmentReference;
    input.lightingSettings = document.lightingSettings;

    if(document.environmentOverrides.sky)
    {
        input.lightingSettings.environmentLuminanceScale = scene.GetLightingSettings().environmentLuminanceScale;
    }

    if(document.environmentOverrides.exposure)
    {
        input.lightingSettings.referenceEV100 = scene.GetLightingSettings().referenceEV100;
    }

    input.environmentalHaze = document.environmentalHaze;
    input.mistRendering = document.mistRendering;
    input.airEnvironment = document.airEnvironment;
    input.airSimulation = document.airSimulation;
    m_effectiveEnvironment = ResolveSceneEnvironment(input, m_environmentWorld ? &*m_environmentWorld : nullptr, m_environmentWorldProblem);

    // Show the effective sky and exposure; overridden groups already show their own live values.
    if(scene.GetEnvironmentReference() != m_effectiveEnvironment.environmentReference)
    {
        scene.SetEnvironmentReference(m_effectiveEnvironment.environmentReference);
    }

    if(!(scene.GetLightingSettings() == m_effectiveEnvironment.lightingSettings))
    {
        scene.SetLightingSettings(m_effectiveEnvironment.lightingSettings);
    }

    return scene.GetEnvironmentReference() != m_publishedEnvironmentReference;
}

bool FileBackedDemoScene::GetOverride(const SceneEnvironmentOverrides& overrides, EnvironmentGroup group)
{
    switch(group)
    {
    case EnvironmentGroup::Sky:
        return overrides.sky;
    case EnvironmentGroup::Exposure:
        return overrides.exposure;
    case EnvironmentGroup::Haze:
        return overrides.haze;
    case EnvironmentGroup::Mist:
        return overrides.mist;
    case EnvironmentGroup::AirEnvironment:
        return overrides.airEnvironment;
    case EnvironmentGroup::Wind:
        return overrides.wind;
    }

    return false;
}

bool FileBackedDemoScene::SetEnvironmentOverride(EnvironmentGroup group, bool enabled, std::string* errorMessage)
{
    if(!m_initialized || m_archiveScene || !m_runtime->GetFileScene().HasDocument() ||
        m_runtime->GetFileScene().GetDocument().kind != SceneKind::Local)
    {
        if(errorMessage)
        {
            *errorMessage = "Environment overrides belong to an open, editable Local Level.";
        }

        return false;
    }

    if(GetOverride(m_runtime->GetFileScene().GetDocument().environmentOverrides, group) == enabled)
    {
        return true;
    }

    auto& document = m_runtime->EditDocument();
    auto& scene = m_runtime->GetScene();
    auto lighting = scene.GetLightingSettings();

    // Sky and exposure live in the runtime Scene while edited: keep the Local Level's own values either way.
    switch(group)
    {
    case EnvironmentGroup::Sky:
        if(enabled)
        {
            scene.SetEnvironmentReference(document.environmentReference);
            lighting.environmentLuminanceScale = document.lightingSettings.environmentLuminanceScale;
            scene.SetLightingSettings(lighting);
        }
        else
        {
            document.environmentReference = scene.GetEnvironmentReference();
            document.lightingSettings.environmentLuminanceScale = lighting.environmentLuminanceScale;
        }

        document.environmentOverrides.sky = enabled;
        break;
    case EnvironmentGroup::Exposure:
        if(enabled)
        {
            lighting.referenceEV100 = document.lightingSettings.referenceEV100;
            scene.SetLightingSettings(lighting);
        }
        else
        {
            document.lightingSettings.referenceEV100 = lighting.referenceEV100;
        }

        document.environmentOverrides.exposure = enabled;
        break;
    case EnvironmentGroup::Haze:
        document.environmentOverrides.haze = enabled;
        break;
    case EnvironmentGroup::Mist:
        document.environmentOverrides.mist = enabled;
        break;
    case EnvironmentGroup::AirEnvironment:
        document.environmentOverrides.airEnvironment = enabled;
        break;
    case EnvironmentGroup::Wind:
        document.environmentOverrides.wind = enabled;
        break;
    }

    RefreshEnvironmentPreview(false);
    return true;
}
