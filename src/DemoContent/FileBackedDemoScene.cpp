
#include "DemoContent/FileBackedDemoScene.h"
#include "DemoContent/SceneRegistry.h"

#include "Scene/Camera.h"
#include "Scene/DirectionalLight.h"

#include "Core/Log.h"

#include "Project/ProjectLocator.h"
#include "Project/ProjectPaths.h"
#include "assets/ProjectAssetResolver.h"
#include "Renderer/Mesh.h"
#include "Renderer/Renderer.h"

#include <memory>
#include <string>
#include <utility>
#include <algorithm>

namespace
{
    std::unique_ptr<IDemoScene> CreateFileBackedTestScene()
    {
        const ProjectPaths paths(
            ProjectLocator::GetDefaultProjectRoot());

        const std::filesystem::path scenePath =
            paths.GetScenesDirectory() / "FirstFileBackedScene.scene";

        return std::make_unique<FileBackedDemoScene>(scenePath);
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

void FileBackedDemoScene::SetProjectAssets(
    const std::filesystem::path& root,
    const ProjectAssetRegistry& registry)
{
    m_projectRoot = root;
    m_registry = &registry;
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
    if (m_initialized)
    {
        return false;
    }

    // Initialization is performed while this scene is inactive.
    // Engine remains responsible for GPU synchronization when
    // replacing a previously active scene.
    Shutdown();

    Log::Info(std::string("Preparing file scene activation: mode=") +
        (metadataOnly ? "metadata-first" : "synchronous-full Debug diagnostic") + ", path=" + m_scenePath.string());

    std::string error;

    // Validate the document before allocating GPU resources.
    if (!m_runtime->Open(m_scenePath, &error))
    {
        Log::Error("Failed to open file-backed scene: " + error);
        return false;
    }

    if (!m_assets.Initialize(
            device,
            uploader,
            descriptorHeap,
            vertexShader,
            pixelShader))
    {
        Log::Error("Failed to initialize file-backed scene assets.");
        Shutdown();
        return false;
    }

    if(m_registry)
    {
        m_assets.SetProjectAssets(ProjectPaths(m_projectRoot), *m_registry);
    }

    m_assets.SetTextureStreamingEnabled(m_textureStreamingEnabled || metadataOnly);
    m_assets.SetMeshCPUCache(m_meshCPUCache);

    const bool prepared = metadataOnly ? m_runtime->PrepareMetadata(m_assets, &error) : m_runtime->Prepare(m_assets, &error);

    if(!prepared)
    {
        Log::Error("Failed to prepare file-backed scene: " + error + " " +
            m_assets.GetLastMeshError() + " " + m_assets.GetLastMaterialError());
        Shutdown();
        return false;
    }

    const bool committed = metadataOnly ? m_runtime->CommitMetadata(m_assets, &error) : m_runtime->Commit(&error);

    if(!committed)
    {
        Log::Error("Failed to commit file-backed scene: " + error);
        Shutdown();
        return false;
    }

    m_initialized = true;
    m_metadataOnlyActivation = metadataOnly;
    return true;
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

void FileBackedDemoScene::AdvanceTextureStreaming(ProjectTexturePreparationService& preparation, Renderer& renderer,
    const std::function<bool(const AssetID&, const MaterialAssetData&)>& mayPublish)
{
    if(!m_initialized || m_objectMutationCandidate || m_runtime->HasMaterialAssignment())
    {
        return;
    }

    m_assets.AdvanceTextureStreaming(preparation, renderer, GetRuntimeIdentity(), GetScene(), mayPublish);
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

    m_runtime->GetScene().GetDirectionalLight() =
        m_runtime->GetFileScene().GetDocument().directionalLight;
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

    return m_runtime->Save(errorMessage);
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
    return m_runtime->CheckUnsavedChanges(dirty, errorMessage);
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
