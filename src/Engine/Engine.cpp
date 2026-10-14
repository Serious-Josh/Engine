#include "Scene/SceneSwitchTiming.h"
#include "Engine/Engine.h"

#include "Core/Log.h"
#include "Core/Assert.h"
#include "Core/Time.h"

#include "Debug/CpuFrameTiming.h"

#include "Scene/Scene.h"
#include "Scene/SceneFileCreation.h"

#include "Project/ProjectLocator.h"
#include "Project/ProjectPaths.h"
#include "assets/ProjectMaterialAssetService.h"
#include "assets/MaterialAssetSerializer.h"

#include "DemoContent/SceneRegistry.h"
#include "DemoContent/MirrorTestScene.h"
#include "DemoContent/FileBackedDemoScene.h"

#include "assets/AssetRegistrySerializer.h"
#include "assets/AssetReconciliationTransaction.h"
#include "assets/ProjectAssetReconciliation.h"
#include "assets/ProjectAssetResolver.h"
#include "Scene/MaterialParameterPropagation.h"
#include "imgui.h"

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <system_error>
#include <cstdlib>
#include <chrono>
#include <string>

namespace
{
    void AcknowledgeDisplacementCompletion(
        Renderer& renderer,
        MirrorTestScene& owner,
        const Renderer::DisplacementCompletionResult& completed)
    {
        const auto revision = completed.requestRevision;
        owner.MarkDisplacementGeometryUpdated(revision);
        Renderer::FrameHitchProbe loggingProbe(
            renderer,
            Renderer::FrameHitchRegion::AcknowledgmentPhaseLogging,
            !Renderer::kSuppressDisplacementLifecycleLoggingForDiagnostics);

        if constexpr (!Renderer::kSuppressDisplacementLifecycleLoggingForDiagnostics)
        {
            const double acknowledgmentMilliseconds = CpuTiming::MillisecondsSince(completed.acceptedAt);
            Log::Info("Async displacement completion delivered for revision " + std::to_string(revision) +
                      ", CPU wall-clock ms since acceptance=" + std::to_string(acknowledgmentMilliseconds) +
                      (owner.HasPendingDisplacementGeometryUpdate() ? "; newer camera request remains pending."
                                                                    : "; scene revision acknowledged."));
        }
    }

    bool DrainAndResolveSceneDisplacement(
        Renderer& renderer,
        IDemoScene& activeDemo)
    {
        Renderer::DisplacementCompletionResult completed{};

        if (!renderer.DrainAndResolveSceneDisplacement(activeDemo.GetScene(), completed))
        {
            return false;
        }

        if (completed.requestRevision == 0)
        {
            return true; // No submission finalized, including canceled prepared requests.
        }

        auto* mirrorScene = dynamic_cast<MirrorTestScene*>(&activeDemo);

        if (!mirrorScene || completed.scene != &mirrorScene->GetScene())
        {
            return false;
        }

        // Renderer delivered only after validating this registered resource
        // lifetime. The scene ignores older revisions if a newer request exists.
        AcknowledgeDisplacementCompletion(renderer, *mirrorScene, completed);
        return true;
    }
}

bool Engine::RequestMaterialAssignment(
    const SceneObjectMetadata& selection, std::string reference, std::string& error)
{
    return QueueMaterialAssignment({selection, std::move(reference)}, error);
}

bool Engine::QueueMaterialAssignment(MaterialAssignmentRequest request, std::string& error)
{
    error.clear();
    const auto& selection = request.selection;

    if(m_state != EngineState::Running || m_pendingSceneObjectCreation || m_pendingSceneObjectRestoration ||
        m_pendingSceneObjectDeletion || m_pendingMaterialAssignment || m_pendingEditorSceneOperation ||
        m_demoSceneManager.HasPendingSceneChange() || m_editor.HasBlockingMaterialOperation() ||
        m_editor.HasPendingSceneHistoryOperation())
    {
        error = "Material assignment rejected: Engine is unavailable or an editor/scene operation is pending.";
        return false;
    }

    const auto* scene = dynamic_cast<const FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    const auto current = scene ? scene->GetObjectMetadata(selection.objectID, &error) : std::nullopt;

    if(!current || current->runtimeIndex != selection.runtimeIndex || current->sceneID != selection.sceneID ||
       current->objectID != selection.objectID ||
       current->runtimeIdentity != selection.runtimeIdentity || current->structureRevision != selection.structureRevision)
    {
        error = "Material assignment requires a current file-backed scene-object selection.";
        return false;
    }

    if(request.slotID)
    {
        if(current->meshReference != selection.meshReference || current->materialReference != selection.materialReference ||
            current->materialSlotBindings != selection.materialSlotBindings)
        {
            error = "Material slot assignment rejected: the object's mesh or bindings changed.";
            return false;
        }

        const auto* mesh = scene->GetScene().GetRenderObjects()[current->runtimeIndex].mesh;

        if(!mesh || std::none_of(mesh->GetMetadata().materialSlots.begin(), mesh->GetMetadata().materialSlots.end(),
            [&](const auto& slot) { return slot.id == *request.slotID; }))
        {
            error = "Material slot assignment rejected: slot does not exist on the current mesh.";
            return false;
        }

        if(request.slotMaterial)
        {
            const auto* material = m_projectAssetRegistry.Find(*request.slotMaterial);
            const ProjectPaths paths(ProjectLocator::GetDefaultProjectRoot());

            if(!material || material->type != AssetType::Material ||
                ProjectAssetResolver::Resolve(paths, m_projectAssetRegistry, *request.slotMaterial).status != AssetAvailability::Available)
            {
                error = "Material slot assignment requires an available registered material AssetID.";
                return false;
            }
        }
    }

    m_pendingMaterialAssignment = std::move(request);
    return true;
}

bool Engine::RequestMaterialAssignment(RenderObjectID index, std::string reference, std::string& error)
{
    const auto* scene = dynamic_cast<const FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    const auto selection = scene ? scene->GetObjectMetadata(index, &error) : std::nullopt;

    if(!selection)
    {
        error = "Material assignment requires a valid file-backed object.";
        return false;
    }

    return RequestMaterialAssignment(*selection, std::move(reference), error);
}

bool Engine::RequestMaterialSlotAssignment(std::string_view objectID, std::string slotID,
    std::optional<AssetID> materialID, std::string& error)
{
    const auto* scene = dynamic_cast<const FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    const auto selection = scene ? scene->GetObjectMetadata(objectID, &error) : std::nullopt;

    if(!selection)
    {
        error = "Material slot assignment requires a persistent object ID in the active file-backed scene.";
        return false;
    }

    return RequestMaterialSlotAssignment(*selection, std::move(slotID), std::move(materialID), error);
}

bool Engine::RequestMaterialSlotAssignment(const SceneObjectMetadata& selection, std::string slotID,
    std::optional<AssetID> materialID, std::string& error)
{
    MaterialAssignmentRequest request{selection, materialID ? materialID->ToReference() : std::string{}};
    request.slotID = std::move(slotID);
    request.slotMaterial = std::move(materialID);
    return QueueMaterialAssignment(std::move(request), error);
}

bool Engine::ExecuteMaterialAssignment()
{
    if(!m_pendingMaterialAssignment)
    {
        return true;
    }

    const auto request = std::move(*m_pendingMaterialAssignment);
    m_pendingMaterialAssignment.reset();
    // Report every exit, including exceptions, without changing execution/rollback semantics.
    struct AssignmentFeedback
    {
        Editor& editor;
        const SceneObjectMetadata& selection;
        bool succeeded = false;
        std::string message = "Scene changed or another operation is active. Try again.";
        ~AssignmentFeedback()
        {
            editor.OnMaterialAssignmentCompleted(selection, succeeded, std::move(message));
        }
    } feedback{m_editor, request.selection};
    auto* owner = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());

    if(!owner || m_pendingEditorSceneOperation || m_demoSceneManager.HasPendingSceneChange() ||
       m_editor.HasBlockingMaterialOperation())
    {
        Log::Warning("Material assignment cancelled: scene/editor operation conflicts with execution.");
        return true;
    }

    if(request.requireEditorSelection && m_editor.GetSelectedObject() != request.selection.runtimeIndex)
    {
        feedback.message = "Material slot assignment cancelled because the selected scene object changed.";
        Log::Warning(feedback.message);
        return true;
    }

    std::string error;
    feedback.message = "Selection or assignment validation failed. See the log for details.";
    std::optional<MaterialAssignmentToken> token;

    try
    {
        token = request.slotID ?
            owner->PrepareMaterialSlotAssignment(request.selection, *request.slotID, request.slotMaterial, &error) :
            owner->PrepareMaterialAssignment(request.selection, request.reference, &error);
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
    }
    catch(...)
    {
        error = "Unknown failure while preparing the material assignment; published scene retained.";
    }

    if(!token)
    {
        feedback.message = error;
        Log::Warning("Material assignment preparation rejected: " + error);
        return true;
    }

    const auto cancel = [&]()
    {
        if(!owner->CancelMaterialAssignment(*token, &error))
        {
            Log::Error("Material assignment cancellation failed: " + error);
            return false;
        }

        return true;
    };

    if(token->noOp)
    {
        Log::Info("Material assignment unchanged; no Renderer rebuild required.");
        feedback.succeeded = cancel();
        feedback.message = "Material is already assigned.";
        return feedback.succeeded;
    }

    const auto target = owner->GetObjectMetadata(request.selection.objectID, &error);
    const bool pendingObject = target && target->GetMeshResidency(
        owner->GetScene().GetRenderObjects()[target->runtimeIndex]) == SceneMeshResidency::Pending;

    feedback.message = "Saved material could not be loaded. See the log for details.";
    Material* replacement = nullptr;
    std::vector<Material*> materials;

    try
    {
        replacement = request.slotID ? owner->GetPreparedMaterialAssignmentMaterial(*token) :
            pendingObject ? owner->ResolvePendingAssignmentMaterial(request.reference, &error) :
                owner->ResolveAssignmentMaterial(request.reference, &error);

        if(replacement && !pendingObject)
        {
            // Allocate the full table snapshot before releasing any Renderer resources.
            materials = owner->GetMaterials();
        }
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
    }

    if(!replacement || (!pendingObject && materials.empty()))
    {
        if(!error.empty())
        {
            feedback.message = error;
        }

        Log::Warning("Material assignment load failed; previous assignment retained: " + error);
        return cancel();
    }

    if (pendingObject)
    {
        if (!m_renderer.CanEditPendingMaterialBindings(owner->GetScene()))
        {
            feedback.message = "Pending material assignment requires a valid pre-recording renderer boundary; retry later.";
            return cancel();
        }

        if (!m_renderer.HasPublishedMaterial(*replacement))
        {
            feedback.message = "Unsupported pending assignment: material requires deferred renderer material-table growth.";
            Log::Warning(feedback.message);
            return cancel();
        }

        bool committed = false;

        try
        {
            committed = owner->CommitMaterialAssignment(*token, *replacement, &error);

            if (committed && !owner->FinishMaterialAssignment(*token, &error))
            {
                if (!owner->RollbackMaterialAssignment(*token, &error))
                {
                    Log::Error("Pending material assignment rollback failed: " + error);
                    return false;
                }

                committed = false;
            }
        }
        catch (const std::exception& exception)
        {
            error = exception.what();

            if (committed)
            {
                if (!owner->RollbackMaterialAssignment(*token, &error))
                {
                    return false;
                }

                committed = false;
            }
        }

        if (!committed)
        {
            feedback.message = error;
            Log::Warning("Pending material assignment rejected: " + error);

            if (owner->m_runtime->HasMaterialAssignment())
            {
                return cancel();
            }

            return true;
        }

        m_editor.InvalidateSceneHistoryRedoAfterEdit();
        feedback.succeeded = true;
        feedback.message = "Authored pending material updated; save the scene to persist it.";
        Log::Info("Pending object material assignment completed without geometry residency or renderer table mutation.");
        return true;
    }

    feedback.message = "Renderer update failed. See the log for recovery details.";

    if(!DrainAndResolveSceneDisplacement(m_renderer, *owner))
    {
        Log::Error("Material assignment drain failed; retaining assets and stopping.");
        cancel();
        return false;
    }

    if(!m_renderer.ReleaseSceneResources())
    {
        Log::Error("Material assignment scene release rejected; retaining assets and stopping.");
        cancel();
        return false;
    }

    m_sceneResourcesReleasedForSwitch = true;
    const auto initialize = [&]()
    {
        try
        {
            return m_renderer.InitializeSceneResources(owner->GetScene(), materials);
        }
        catch(const std::exception& exception)
        {
            Log::Error(std::string("Material assignment Renderer initialization threw: ") + exception.what());
            return false;
        }
        catch(...)
        {
            Log::Error("Material assignment Renderer initialization threw an unknown exception.");
            return false;
        }
    };

    bool committed = false;

    try
    {
        bool materialAvailable = true;

        if(request.slotID && request.slotMaterial)
        {
            const auto* record = m_projectAssetRegistry.Find(*request.slotMaterial);
            const ProjectPaths paths(ProjectLocator::GetDefaultProjectRoot());
            materialAvailable = record && record->type == AssetType::Material &&
                ProjectAssetResolver::Resolve(paths, m_projectAssetRegistry, *request.slotMaterial).status == AssetAvailability::Available;

            if(!materialAvailable)
            {
                error = "Slot material disappeared or is no longer registered before commit.";
            }
        }

        committed = materialAvailable && owner->CommitMaterialAssignment(*token, *replacement, &error);
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
    }
    catch(...)
    {
        error = "Unknown material assignment commit failure; restoring the previous Renderer state.";
    }

    if(!committed)
    {
        feedback.message = error;
        Log::Warning("Material assignment CPU commit rejected after release: " + error);

        if(!cancel() || !initialize())
        {
            Log::Error("Material assignment pre-commit restoration failed; retaining ownership and stopping.");
            return false;
        }

        m_sceneResourcesReleasedForSwitch = false;
        Log::Warning("Material assignment cancelled; previous Renderer state successfully restored.");
        return true;
    }

    if(initialize())
    {
        bool finalized = false;

        try
        {
            finalized = owner->FinishMaterialAssignment(*token, &error);
        }
        catch(const std::exception& exception)
        {
            error = exception.what();
        }
        catch(...)
        {
            error = "Unknown material assignment finalization failure.";
        }

        if(!finalized)
        {
            Log::Error("Material assignment finalization failed; retaining ownership and stopping: " + error);
            return false;
        }

        m_sceneResourcesReleasedForSwitch = false;
        m_renderedSceneStructureRevision = owner->GetScene().GetStructureRevision();
        m_editor.InvalidateSceneHistoryRedoAfterEdit();
        feedback.succeeded = true;
        feedback.message = "Material updated; save the scene to persist it.";
        Log::Info("Scene object material assignment completed; scene remains unsaved.");
        return true;
    }

    if(!m_renderer.CleanupFailedSceneInitialization(owner->GetScene()))
    {
        Log::Error("Material assignment cleanup could not prove GPU safety; retaining CPU assignment and assets.");
        return false;
    }

    bool restored = false;

    try
    {
        restored = owner->RollbackMaterialAssignment(*token, &error);
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
    }
    catch(...)
    {
        error = "Unknown material assignment rollback failure.";
    }

    if(!restored)
    {
        Log::Error("Material assignment CPU rollback rejected; retaining assets and stopping: " + error);
        return false;
    }

    if(!initialize())
    {
        Log::Error("Material assignment Renderer restoration failed; retaining assets and stopping.");
        return false;
    }

    m_sceneResourcesReleasedForSwitch = false;
    feedback.message = "Renderer initialization failed; previous assignment restored. See the log for details.";
    Log::Warning("Material assignment failed; original CPU and Renderer state successfully restored.");
    return true;
}

bool Engine::ActivateRequestedFileScene()
{
    const auto started = std::chrono::steady_clock::now();
    SceneSwitchTiming::Session timing(m_meshCPUCache);
    IDemoScene* previous = m_demoSceneManager.TryGetActiveScene();

    if (!previous)
    {
        Log::Error("File scene activation requires an active scene owner.");
        return false;
    }

    std::string error;

    SceneSwitchTiming::Measure stagingTiming(SceneSwitchTiming::Stage::Staging);

    if (!m_demoSceneManager.GetStagedFileScene() &&
        !m_demoSceneManager.StageRequestedFileScene(
            m_renderer.GetDevice(), m_renderer.GetResourceUploader(), m_renderer.GetResourceDescriptorHeap(),
            m_demoVertexShader, m_demoPixelShader, &error))
    {
        Log::Error("File scene staging failed; keeping the current scene: " + error);
        m_demoSceneManager.CancelFileSceneRequest();
        return true;
    }

    stagingTiming.Stop();
    IDemoScene* candidate = m_demoSceneManager.GetStagedFileScene();

    if (!candidate)
    {
        Log::Error("File scene staging did not produce a candidate.");
        return false;
    }

    SceneSwitchTiming::Measure drainTiming(SceneSwitchTiming::Stage::DrainRelease);

    if (!DrainAndResolveSceneDisplacement(m_renderer, *previous))
    {
        Log::Error("Displacement drain failed before file scene activation; retaining both scene owners.");
        return false;
    }

    if (!m_renderer.ReleaseSceneResources())
    {
        Log::Error("Scene resource release rejected before file scene activation.");
        return false;
    }

    m_sceneResourcesReleasedForSwitch = true;

    drainTiming.Stop();
    const bool initialized = SceneSwitchTiming::MeasureCall(SceneSwitchTiming::Stage::Renderer, [&]()
    {
        return m_renderer.InitializeSceneResources(candidate->GetScene(), candidate->GetMaterials());
    });

    if (initialized)
    {
        const bool published = SceneSwitchTiming::MeasureCall(SceneSwitchTiming::Stage::Publication, [&]()
        {
            return m_demoSceneManager.CommitStagedFileScene(&error);
        });

        if (!published)
        {
            Log::Error("File scene commit failed; retaining candidate Renderer ownership: " + error);
            return false;
        }

        m_sceneResourcesReleasedForSwitch = false;

        OnFileSceneActivated(started);
        m_editor.OnSceneActivated();
        m_renderedSceneStructureRevision = m_demoSceneManager.GetActiveScene().GetScene().GetStructureRevision();
        timing.outcome = "published";
        return true;
    }

    Log::Error("Candidate Renderer initialization failed for " +
               m_demoSceneManager.GetRequestedFileScenePath().string() +
               "; file preserved; attempting checked cleanup.");

    if (!m_renderer.CleanupFailedSceneInitialization(candidate->GetScene()))
    {
        Log::Error("Candidate cleanup could not prove GPU safety; retaining both scene owners.");
        return false;
    }

    // The manager still owns the old scene. Only now may candidate assets die.
    m_demoSceneManager.CancelFileSceneRequest();

    if (!m_renderer.InitializeSceneResources(previous->GetScene(), previous->GetMaterials()))
    {
        Log::Error("Failed to restore previous scene Renderer resources; requesting fail-closed shutdown.");
        return false;
    }

    if (auto* mirror = dynamic_cast<MirrorTestScene*>(previous))
    {
        // The drain delivered completed async revisions before registration
        // release. Rebuild current camera geometry rather than reverting an
        // already acknowledged revision to seed LODs.
        const bool hasCameraSettings = mirror->HasCurrentDisplacementSettings();
        const auto revision = mirror->GetRequestedDisplacementRevision();
        const auto settings = hasCameraSettings
            ? mirror->GetCurrentDisplacementSettings()
            : mirror->GetSeedDisplacementSettings();

        if (!m_renderer.RegisterDisplacementHeightmap(*mirror) ||
            !m_renderer.GenerateDisplacementMeshGPU(*mirror, settings))
        {
            Log::Error("Failed to restore Mirror Test displacement; requesting fail-closed shutdown.");
            return false;
        }

        if (hasCameraSettings)
        {
            mirror->MarkDisplacementGeometryUpdated(revision);
        }
    }

    m_sceneResourcesReleasedForSwitch = false;
    Log::Info("Previous scene Renderer resources restored after failed file scene activation.");
    return true;
}

void Engine::QueueEditorSceneOperation()
{
    if (!m_pendingEditorSceneOperation)
    {
        return;
    }

    const auto request = std::move(*m_pendingEditorSceneOperation);
    m_pendingEditorSceneOperation.reset();
    m_sceneOperationError.clear();
    std::string error;

    if (request.type == Editor::FileRequestType::Demo)
    {
        if (!m_demoSceneManager.RequestScene(request.sceneName))
        {
            Log::Error("Failed to request demo scene: " + request.sceneName);
        }

        return;
    }

    std::filesystem::path path = request.path;

    if (request.type == Editor::FileRequestType::New)
    {
        if (!CreateNewSceneFile(path, &error))
        {
            Log::Error("Failed to create scene: " + error);
            return;
        }

        Log::Info("Created scene file: " + path.string());
    }

    if (!m_demoSceneManager.RequestFileScene(path, &error))
    {
        Log::Error("Failed to request file scene; any newly created file is preserved: " + error);
    }
}

void Engine::ProcessEditorMaterialRequests()
{
    if(const auto request = m_editor.TakeMaterialCreationRequest())
    {
        const ProjectPaths projectPaths(ProjectLocator::GetDefaultProjectRoot());
        auto createdID = AssetID::Generate();
        std::string error;

        if(!createdID)
        {
            error = "Failed to initialize material asset ID.";
        }
        else if(!ProjectMaterialAssetService::Create(projectPaths, m_projectAssetRegistry,
            request->relativePath, MaterialAssetData{}, *createdID, &error) && error.empty())
        {
            error = "Failed to create material.";
        }

        m_editor.OnMaterialCreationCompleted(request->relativePath, error);
    }
}

void Engine::ProcessEditorSceneRequests(IDemoScene& activeScene)
{
    auto* fileScene = dynamic_cast<FileBackedDemoScene*>(&activeScene);

    if (const auto request = m_editor.TakeFileRequest())
    {
        if(m_pendingSceneObjectCreation || m_pendingSceneObjectRestoration || m_pendingSceneObjectDeletion ||
            m_pendingEditorSceneOperation || m_pendingMaterialAssignment || m_demoSceneManager.HasPendingSceneChange() ||
            m_editor.HasPendingSceneHistoryOperation())
        {
            Log::Error("Scene operation rejected: another operation is pending.");
        }
        else if (request->type == Editor::FileRequestType::Save)
        {
            std::string error;

            if (!fileScene)
            {
                Log::Error("Save rejected: the active scene is not file-backed.");
            }
            else if (!fileScene->Save(&error))
            {
                Log::Error("Failed to save scene: " + error);
            }
            else
            {
                Log::Info("Saved file-backed scene: " + fileScene->GetDisplayName());
            }
        }
        else
        {
            auto operation = *request;
            bool valid = true;

            if (operation.type == Editor::FileRequestType::Load)
            {
                // Resolve symlinks/junctions before checking component containment.
                // Save the resolved path so staging does not reuse the menu's alias.
                std::error_code error;
                const ProjectPaths project(ProjectLocator::GetDefaultProjectRoot());
                const auto directory = std::filesystem::canonical(project.GetScenesDirectory(), error);

                if (error || !operation.path.is_absolute() || operation.path.extension() != ".scene")
                {
                    valid = false;
                }
                else
                {
                    const auto path = std::filesystem::canonical(operation.path, error);

                    if (error || path.extension() != ".scene" ||
                        !std::filesystem::is_regular_file(path, error) || error)
                    {
                        valid = false;
                    }
                    else
                    {
                        const auto relative = path.lexically_relative(directory);
                        valid = !relative.empty() && !relative.is_absolute();

                        for (const auto& component : relative)
                        {
                            if (component == "..")
                            {
                                valid = false;
                            }
                        }

                        operation.path = path;
                    }
                }

                if (!valid)
                {
                    Log::Error("Load rejected: select a regular .scene file inside the project's Scenes directory.");
                }
            }

            bool dirty = false;
            std::string error;

            if (valid && fileScene && !fileScene->CheckUnsavedChanges(dirty, &error))
            {
                Log::Error("Cannot safely check unsaved scene changes: " + error);
                valid = false;
            }

            if (valid)
            {
                m_pendingEditorSceneOperation = std::move(operation);
                m_sceneOperationError.clear();

                if (!dirty)
                {
                    QueueEditorSceneOperation();
                }
            }
        }
    }

    if (!m_pendingEditorSceneOperation)
    {
        return;
    }

    const auto decision = m_editor.DrawSceneChangeConfirmation(m_sceneOperationError);

    if (decision == Editor::SceneChangeDecision::Cancel)
    {
        m_pendingEditorSceneOperation.reset();
        m_sceneOperationError.clear();
    }
    else if (decision == Editor::SceneChangeDecision::Discard)
    {
        QueueEditorSceneOperation();
    }
    else if (decision == Editor::SceneChangeDecision::Save)
    {
        if (!fileScene || !fileScene->Save(&m_sceneOperationError))
        {
            if (!fileScene)
            {
                m_sceneOperationError = "Active scene is no longer file-backed.";
            }

            Log::Error("Scene switch paused: Save failed: " + m_sceneOperationError);
        }
        else
        {
            Log::Info("Saved file-backed scene: " + fileScene->GetDisplayName());
            QueueEditorSceneOperation();
        }
    }
}

bool Engine::Initialize()
{
    const auto initializationStarted = std::chrono::steady_clock::now();
    ENGINE_ASSERT(m_state == EngineState::Uninitialized);

    // Initialize persistent project storage before GPU resources.
    const ProjectPaths projectPaths( ProjectLocator::GetDefaultProjectRoot());

    std::string projectError;

    if(!projectPaths.EnsureDirectories(&projectError))
    {
        Log::Error("Failed to initialize default project directories: " + projectError);

        return false;
    }

    Log::Info("Default project directory: " + projectPaths.GetRoot().string());

    // Initialize the project asset registry before runtime resources.
    if(!AssetReconciliationTransaction::Recover(projectPaths, projectError))
    {
        Log::Error("Project reconciliation recovery failed: " + projectError);
        return false;
    }

    const std::filesystem::path registryPath = projectPaths.GetAssetRegistryPath();

    std::error_code registryError;
    const bool registryExists = std::filesystem::exists(registryPath, registryError);

    if(registryError)
    {
        Log::Error("Failed to inspect project asset registry: " + registryError.message());
        return false;
    }

    if(registryExists)
    {
        // Never overwrite an existing malformed or unreadable manifest.
        if(!AssetRegistrySerializer::LoadFromFile(
                registryPath,
                m_projectAssetRegistry,
                &projectError))
        {
            Log::Error("Failed to load project asset registry: " + projectError);
            return false;
        }

        Log::Info("Loaded project asset registry: " + registryPath.string());
    }
    else
    {
        ProjectAssetRegistry emptyRegistry;

        if(!AssetRegistrySerializer::SaveToFile(
                emptyRegistry,
                registryPath,
                &projectError))
        {
            Log::Error("Failed to create project asset registry: " + projectError);
            return false;
        }

        m_projectAssetRegistry = std::move(emptyRegistry);

        Log::Info("Created empty project asset registry: " + registryPath.string());
    }

    ProjectAssetReconciliation::Report reconciliation;

    if(!ProjectAssetReconciliation::Run(projectPaths, m_projectAssetRegistry, reconciliation, projectError))
    {
        Log::Error("Project asset reconciliation failed: " + projectError);
        return false;
    }

    for(const auto& skipped : reconciliation.skipped)
    {
        Log::Warning("Asset reconciliation skipped " + skipped);
    }

    if(reconciliation.registrationsRemoved)
    {
        Log::Warning("Project asset reconciliation: " + std::to_string(reconciliation.missingDetected) +
            " missing registrations detected, " + std::to_string(reconciliation.registrationsRemoved) +
            " removed; " + std::to_string(reconciliation.materialReferencesCleared) +
            " material and " + std::to_string(reconciliation.sceneReferencesCleared) +
            " scene references cleared; " + std::to_string(reconciliation.filesModified.size()) + " files modified.");

        for(const auto& file : reconciliation.filesModified)
        {
            Log::Info("Reconciled project file: " + file);
        }
    }

    // Inspect the reconciled registry without modifying it further.
    std::size_t availableAssets = 0;
    std::size_t missingAssets = 0;
    std::size_t invalidAssets = 0;
    std::size_t unreadableAssets = 0;

    for(const AssetRecord& record : m_projectAssetRegistry.GetAssets())
    {
        const AssetResolution resolution =
            ProjectAssetResolver::Resolve(
                projectPaths,
                m_projectAssetRegistry,
                record.id);

        const std::string assetDescription =
            record.id.ToReference() + " (" +
            record.relativePath.generic_string() + ")";

        switch (resolution.status){
            case AssetAvailability::Available:
                ++availableAssets;
                break;

            case AssetAvailability::Missing:
                ++missingAssets;
                Log::Error("Project asset missing: " + assetDescription);
                break;

            case AssetAvailability::InvalidPath:
                ++invalidAssets;
                Log::Error("Project asset has an invalid path: " + assetDescription);
                break;

            case AssetAvailability::Unreadable:
                ++unreadableAssets;
                Log::Error("Project asset cannot be inspected: " + assetDescription);
                break;

            case AssetAvailability::NotRegistered:
                ++invalidAssets;
                Log::Error("Registered asset lookup failed: " + assetDescription);
                break;
        }
    }

    Log::Info(
        "Project asset scan: " +
        std::to_string(availableAssets) + " available, " +
        std::to_string(missingAssets) + " missing, " +
        std::to_string(invalidAssets) + " invalid, " +
        std::to_string(unreadableAssets) + " unreadable.");

    if (!m_window.Initialize(m_windowProperties))
    {
        Log::Error("Failed to initialize window.");
        return false;
    }

    m_time.Initialize();

    const std::filesystem::path executableDirectory = WindowsWindow::GetExecutableDirectory();

    const std::filesystem::path assetRoot = executableDirectory / "assets";

    if (!m_renderer.Initialize(m_window.GetNativeHandle(), m_window.GetWidth(), m_window.GetHeight(), assetRoot))
    {
        Log::Error("Failed to initialize renderer.");
        return false;
    }

    Log::Info("Initializing demo scene.");

    ShaderDescription vertexShaderDescription{};

    vertexShaderDescription.path = "Triangle.hlsl";
    vertexShaderDescription.entryPoint = "VSMain";
    vertexShaderDescription.stage = ShaderStage::Vertex;

    m_demoVertexShader = m_renderer.GetShaderLibrary().Load(vertexShaderDescription);

    if (!m_demoVertexShader)
    {
        Log::Error("Failed to load demo vertex shader.");

        return false;
    }

    ShaderDescription pixelShaderDescription{};
    pixelShaderDescription.path = "Triangle.hlsl";

    pixelShaderDescription.entryPoint = "PSMain";
    pixelShaderDescription.stage = ShaderStage::Pixel;

    m_demoPixelShader = m_renderer.GetShaderLibrary().Load(pixelShaderDescription);

    if(!m_demoPixelShader)
    {
        Log::Error("Failed to load demo pixel shader.");

        return false;
    }

    const float aspectRatio = static_cast<float>(m_window.GetWidth()) / static_cast<float>(m_window.GetHeight());

    m_editor.ConfigureMaterialThumbnails(m_renderer);

    m_demoSceneManager.SetProjectAssets(ProjectLocator::GetDefaultProjectRoot(), m_projectAssetRegistry);
    m_demoSceneManager.SetTextureStreamingEnabled(true);
    m_demoSceneManager.SetMeshCPUCache(&m_meshCPUCache);

    if(!m_demoSceneManager.Initialize(
            "Sphere Grid", m_renderer.GetDevice(), m_renderer.GetResourceUploader(),
            m_renderer.GetResourceDescriptorHeap(), m_demoVertexShader, m_demoPixelShader,
            aspectRatio))
    {
        Log::Error("Failed to initialize demo scene manager.");
        return false;
    }

    std::string texturePreparationError;

    if(!m_texturePreparation.Start({}, texturePreparationError))
    {
        Log::Error("Cannot start texture preparation service: " + texturePreparationError);
        return false;
    }

    std::string meshPreparationError;

    if (!m_meshPreparation.Start({}, meshPreparationError))
    {
        Log::Error("Cannot start mesh preparation service: " + meshPreparationError);
        return false;
    }

    Log::Info("Texture preparation limits: 16 requests, 4 completed jobs, 64 MiB source, "
        "256 MiB active reservation, 512 MiB total retained reservations.");

    Log::Info("Demo scene initialization complete.");

    IDemoScene& activeDemo = m_demoSceneManager.GetActiveScene();

    if(!m_renderer.InitializeSceneResources(activeDemo.GetScene(), activeDemo.GetMaterials()))
    {
        Log::Error("Failed to initialize demo renderer resources.");
        return false;
    }

    // Initial BLAS/TLAS registration is complete before generation rebuilds it.

    if(auto* mirrorScene = dynamic_cast<MirrorTestScene*>(&activeDemo))
    {
        if(!m_renderer.RegisterDisplacementHeightmap(*mirrorScene) ||

            !m_renderer.GenerateDisplacementMeshGPU(*mirrorScene, mirrorScene->GetSeedDisplacementSettings()))
        {
            Log::Error("Failed to generate initial Mirror Test GPU displacement geometry.");
            return false;
        }
    }

    m_renderedSceneStructureRevision = activeDemo.GetScene().GetStructureRevision();

    m_window.SetEventCallback(
        [this](const Event& event)
        {
            if(m_window.ShouldClose())
            {
                return;
            }

            switch(event.type)
            {
            case EventType::WindowResized:
                m_window.SetSize(event.width, event.height);

                if(event.width > 0 && event.height > 0)
                {
                    m_demoSceneManager.SetAspectRatio(static_cast<float>(event.width) /

                                                      static_cast<float>(event.height));
                }

                if(!m_renderer.Resize(event.width, event.height))
                {
                    Log::Error("Renderer resize failed; requesting application shutdown.");

                    m_window.Close();
                }

                break;
            case EventType::WindowClose:

                if(m_editor.RequestApplicationClose())
                {
                    m_window.Close();
                }

                break;
            case EventType::KeyPressed:
                m_input.SetKeyState(event.code, true);

                break;
            case EventType::KeyReleased:
                m_input.SetKeyState(event.code, false);
                break;
            case EventType::MouseButtonPressed:
                m_input.SetMouseButtonState(event.code, true);
                break;
            case EventType::MouseButtonReleased:
                m_input.SetMouseButtonState(event.code, false);
                break;
            case EventType::MouseMoved:
                m_input.SetMousePosition(event.mouseX, event.mouseY);
                break;
            case EventType::MouseScrolled:
                m_input.SetScroll(event.scrollX, event.scrollY);
                break;
            }
        });

    m_state = EngineState::Running;
    OnFileSceneActivated(initializationStarted);
    Log::Info("Engine initialized.");
    return true;
}

void Engine::Run()
{
    ENGINE_ASSERT(m_state == EngineState::Running);

    CpuTiming::Clock::time_point previousFrameStart;

    bool hasPreviousFrameStart = false;

    uint64_t lastRecordedFrame = 0;

    bool hasRecordedFrame = false;

    uint64_t lastPacingFrame = 0;

    bool hasPacingFrame = false;

    std::uint64_t diagnosticEngineSerial = 0;

    while(!m_window.ShouldClose())
    {
        const auto frameStart = CpuTiming::Clock::now();
        const double frameInterval =
            hasPreviousFrameStart ? std::chrono::duration<double, std::milli>(frameStart - previousFrameStart).count()
                                  : 0.0;
        const bool intervalValid = hasPreviousFrameStart;
        previousFrameStart = frameStart;
        hasPreviousFrameStart = true;

        if constexpr (Renderer::IsFrameHitchDiagnosticsEnabled())
        {
            m_diagnosticRenderOutcome.reset();
            m_renderer.BeginFrameHitchDiagnostics(++diagnosticEngineSerial);
        }

        Frame();

        const double frameExecution = CpuTiming::MillisecondsSince(frameStart);

        if constexpr (Renderer::IsFrameHitchDiagnosticsEnabled())
        {
            m_renderer.RecordFrameHitchDiagnostics(frameStart, frameExecution, m_diagnosticRenderOutcome);
        }

        CpuTiming::FrameSample sample = m_renderer.GetCpuFrameTiming();

        if(sample.submitted && (!hasPacingFrame || sample.frameSerial != lastPacingFrame))
        {
            sample.frameStart = frameStart;
            sample.frameExecution = frameExecution;

            m_renderer.GetDebugUI().RecordFramePacingCpuTiming(sample);
            lastPacingFrame = sample.frameSerial;
            hasPacingFrame = true;
        }

        // Skipped/minimized frames must not publish the previous Render sample again.

        if(sample.valid && (!hasRecordedFrame || sample.frameSerial != lastRecordedFrame))
        {
            sample.frameInterval = frameInterval;
            sample.frameIntervalValid = intervalValid;

            sample.frameExecution = frameExecution;
            m_renderer.GetDebugUI().RecordCpuFrameTiming(sample);
            lastRecordedFrame = sample.frameSerial;
            hasRecordedFrame = true;
        }
        else
        {
            m_renderer.ResetCpuSubmissionCadence();
        }
    }

    Log::Info("Engine loop ended.");
}

void Engine::Frame()
{
    std::optional<Renderer::FrameHitchProbe> inputProbe;

    if constexpr (Renderer::IsFrameHitchDiagnosticsEnabled())
    {
        inputProbe.emplace(m_renderer, Renderer::FrameHitchRegion::EngineInput);
    }

    m_time.Update();

    m_input.Update();
    m_window.ProcessMessages();
    inputProbe.reset();

    if(m_window.ShouldClose())
    {
        return;
    }

    if(m_window.GetWidth() == 0 || m_window.GetHeight() == 0)
    {
        m_editor.SuspendViewportInteraction();
        m_window.WaitForMessage();
        // Exclude the suspended interval from the next simulation update.
        m_time.Update();
        return;
    }

    DebugUI& debugUI = m_renderer.GetDebugUI();

    bool displacementCompletionDelivered = false;

    if(Renderer::IsAsyncDisplacementEnabled())
    {
        Renderer::FrameHitchProbe completionProbe(
            m_renderer,
            Renderer::FrameHitchRegion::EngineCompletion);

        // Poll before scene switches/editor mutation and outside recording.
        // Render polls again at admission; a result produced there is preserved
        // until this owner-aware delivery on the next update.
        const auto completion = m_renderer.ProcessDisplacementCompletion();

        if(completion == Renderer::DisplacementCompletionStatus::Error)
        {
            Log::Error("Asynchronous displacement completion failed; requesting shutdown.");
            m_window.Close();
            return;
        }

        if(completion == Renderer::DisplacementCompletionStatus::Finalized)
        {
            auto* mirrorScene = dynamic_cast<MirrorTestScene*>(&m_demoSceneManager.GetActiveScene());

            Renderer::DisplacementCompletionResult completed{};

            if (!mirrorScene || !m_renderer.TakeDisplacementCompletion(*mirrorScene, completed) ||

                completed.scene != &mirrorScene->GetScene())
            {
                Log::Error("Displacement completion does not match the active scene lifetime; requesting shutdown.");
                m_window.Close();

                return;
            }

            // Renderer validated the resource lifetime before taking the payload.
            // The scene only clears pending when this exact revision is current.
            AcknowledgeDisplacementCompletion(m_renderer, *mirrorScene, completed);
            displacementCompletionDelivered = true;
        }
    }

    if (debugUI.HasEnvironmentChangeRequest())
    {
        Renderer::FrameHitchProbe environmentProbe(
            m_renderer,
            Renderer::FrameHitchRegion::EngineEnvironment);
        const size_t environmentIndex = debugUI.ConsumeEnvironmentChangeRequest();

        if (!m_renderer.SetEnvironment(environmentIndex))
        {
            Log::Error("Failed to switch environment.");
            m_window.Close();
            return;
        }
    }

    bool materialAssignmentSafe = false;

    try
    {
        const bool meshDropPending = m_pendingSceneObjectCreation.has_value();
        const bool restorationReplayPending = m_pendingSceneObjectRestoration &&
            m_pendingSceneObjectRestoration->historyReplay.has_value();
        const auto deletionSelection = m_pendingSceneObjectDeletion ?
            std::optional<SceneObjectMetadata>{m_pendingSceneObjectDeletion->selection} : std::nullopt;
        materialAssignmentSafe = ExecuteSceneObjectRestoration() && ExecuteSceneObjectDeletion() &&
            ExecuteSceneObjectCreation() && ExecuteMaterialAssignment();

        if(materialAssignmentSafe && restorationReplayPending && m_lastSceneObjectRestoration &&
            m_lastSceneObjectRestoration->historyReplay)
        {
            const auto& result = *m_lastSceneObjectRestoration;
            m_editor.CompleteSceneHistoryReplay(*result.historyReplay, result.succeeded, result.error);
        }

        if(materialAssignmentSafe && deletionSelection && GetLastSceneObjectDeletionResult())
        {
            const auto& completed = *GetLastSceneObjectDeletionResult();
            m_editor.CompleteObjectDeletionHistory(completed.operationID, completed.historyEpoch,
                completed.succeeded, completed.error);
        }

        if(deletionSelection && GetLastSceneObjectDeletionResult())
        {
            const auto& result = *GetLastSceneObjectDeletionResult();
            m_editor.OnSceneObjectDeletionCompleted(m_demoSceneManager.GetActiveScene(),
                *deletionSelection, result.succeeded, result.error);
        }

        if(meshDropPending && m_lastSceneObjectCreation)
        {
            m_editor.OnMeshCreationCompleted(m_lastSceneObjectCreation->succeeded, m_lastSceneObjectCreation->error);
        }
    }
    catch(const std::exception& exception)
    {
        Log::Error(std::string("Material assignment execution failed; retaining ownership: ") + exception.what());
    }
    catch(...)
    {
        Log::Error("Material assignment execution failed unexpectedly; retaining ownership.");
    }

    if(!materialAssignmentSafe)
    {
        m_window.Close();
        return;
    }

    if (m_demoSceneManager.HasPendingSceneChange())
    {
        const auto switchStarted = std::chrono::steady_clock::now();
        if (m_diagnosticMeshToken || m_diagnosticCPUPreparation)
        {
            Log::Info("Mesh residency request canceled before scene switch.");
        }

        CancelMeshResidencyAcceptance();
        m_texturePreparation.Invalidate();
        m_meshPreparation.Invalidate();
        Renderer::FrameHitchProbe switchProbe(
            m_renderer,
            Renderer::FrameHitchRegion::EngineSwitch);

        if (!m_demoSceneManager.GetRequestedFileScenePath().empty())
        {
            if (!ActivateRequestedFileScene())
            {
                m_window.Close();
                return;
            }
        }
        else
        {
            m_automaticMeshResidency = {};
            m_automaticResidencyIntent = 0;

            if (!DrainAndResolveSceneDisplacement(m_renderer, m_demoSceneManager.GetActiveScene()))
            {
                Log::Error("GPU drain or displacement resolution failed before scene switch; requesting shutdown.");

                m_window.Close();
                return;
            }

            if (!m_renderer.ReleaseSceneResources())
            {
                Log::Error("Scene resource release rejected before scene switch; requesting shutdown.");
                m_window.Close();
                return;
            }

            m_sceneResourcesReleasedForSwitch = true;

            if (!m_demoSceneManager.ApplyPendingSceneChange(m_renderer.GetDevice(), m_renderer.GetResourceUploader(),
                                                            m_renderer.GetResourceDescriptorHeap(), m_demoVertexShader,
                                                            m_demoPixelShader))
            {
                Log::Error("Failed to switch demo scene.");

                m_window.Close();
                return;
            }

            IDemoScene& switchedDemo = m_demoSceneManager.GetActiveScene();

            if (!m_renderer.InitializeSceneResources(switchedDemo.GetScene(), switchedDemo.GetMaterials()))
            {
                Log::Error("Failed to initialize renderer resources for switched demo scene.");
                m_window.Close();
                return;
            }

            m_sceneResourcesReleasedForSwitch = false;

            if (auto* mirrorScene = dynamic_cast<MirrorTestScene*>(&switchedDemo))
            {
                if (!m_renderer.RegisterDisplacementHeightmap(*mirrorScene) ||
                    !m_renderer.GenerateDisplacementMeshGPU(*mirrorScene, mirrorScene->GetSeedDisplacementSettings()))
                {
                    Log::Error("Failed to generate Mirror Test GPU displacement geometry after scene switch.");

                    m_window.Close();
                    return;
                }
            }

            m_editor.OnSceneActivated();
            m_renderedSceneStructureRevision = switchedDemo.GetScene().GetStructureRevision();

            OnFileSceneActivated(switchStarted);
        }
    }

    std::optional<Renderer::FrameHitchProbe> uiProbe;

    if constexpr (Renderer::IsFrameHitchDiagnosticsEnabled())
    {
        uiProbe.emplace(m_renderer, Renderer::FrameHitchRegion::EngineUI);
    }

    const std::vector<EnvironmentAsset>& environments = m_renderer.GetAvailableEnvironments();

    std::vector<std::string> environmentNames;
    environmentNames.reserve(environments.size());

    for (const EnvironmentAsset& environment : environments)
    {
        environmentNames.push_back(environment.displayName);
    }

    if(m_editor.NeedsThumbnailReset())
    {
        m_editor.ResetThumbnailRequests();
    }

    ProcessWorkingMaterialNotifications();
    ProcessSavedMaterialRevisions();
    PublishWorkingMaterialParameters();
    ProcessSceneTextureStreaming();

    m_editor.SetThumbnailCompletedFence(m_renderer.GetEditorCompletedFence());

    debugUI.BeginFrame();

    IDemoScene& activeDemo = m_demoSceneManager.GetActiveScene();

    m_editor.SetSceneOperationPending(m_pendingSceneObjectCreation.has_value() ||
                                      m_pendingSceneObjectRestoration.has_value() || m_pendingSceneObjectDeletion.has_value() ||
                                      m_pendingMaterialAssignment.has_value() || m_pendingEditorSceneOperation.has_value() ||
                                      m_demoSceneManager.HasPendingSceneChange());
    const auto& viewportRectangle = m_editor.PrepareWorkspace(activeDemo, m_demoSceneManager, debugUI);
    const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;

    if(viewportRectangle.size.x <= 0.0f || viewportRectangle.size.y <= 0.0f)
    {
        m_editor.SuspendViewportInteraction();
        ImGui::EndFrame();
        return;
    }

    const auto renderPixels = [](float logical, float scale) -> unsigned int
    {
        const double pixels = static_cast<double>(logical) * static_cast<double>(scale);

        if(!std::isfinite(pixels) || pixels <= 0.0)
        {
            return 0;
        }

        return static_cast<unsigned int>((std::clamp)(std::floor(pixels), 1.0, 16384.0));
    };
    const unsigned int sceneWidth = renderPixels(viewportRectangle.size.x, framebufferScale.x);
    const unsigned int sceneHeight = renderPixels(viewportRectangle.size.y, framebufferScale.y);

    // Coalesce splitter movement without storing a potentially stale pending extent.
    // The release frame always uses the latest finalized layout.
    const bool resizeScene = !m_editor.IsWorkspaceSplitterDragging();

    if(sceneWidth == 0 || sceneHeight == 0 || (resizeScene && !m_renderer.ResizeScene(sceneWidth, sceneHeight)))
    {
        Log::Error("Scene viewport resize failed; requesting application shutdown.");
        ImGui::EndFrame();
        m_window.Close();
        return;
    }

    Renderer::FinalSceneImage finalSceneImage{};

    if(!m_renderer.TryGetFinalSceneImage(finalSceneImage))
    {
        Log::Error("Final scene image is unavailable; requesting application shutdown.");
        m_window.Close();
        return;
    }

    // During dragging the camera follows the retained texture extent, not the requested layout extent.
    m_demoSceneManager.SetAspectRatio(static_cast<float>(finalSceneImage.width) /
                                    static_cast<float>(finalSceneImage.height));

    const ProjectPaths inspectorPaths(ProjectLocator::GetDefaultProjectRoot());
    m_editor.Draw(activeDemo, m_demoSceneManager, debugUI, finalSceneImage.srv.ptr,
                  finalSceneImage.width, finalSceneImage.height, m_time.GetDeltaTime(),
                  inspectorPaths, m_projectAssetRegistry, m_renderer.GetMaterialPreviewImage());

    debugUI.DrawPerformanceWindow(environmentNames, m_renderer.GetCurrentEnvironmentIndex(),
                                  m_renderer.GetStatistics());

    const auto* activeFileScene = dynamic_cast<const FileBackedDemoScene*>(&activeDemo);
    m_editor.ProcessAssetRename(inspectorPaths, m_projectAssetRegistry,
        activeFileScene ? activeFileScene->GetScenePath() : std::filesystem::path{});
    m_editor.ProcessAssetDeletion(inspectorPaths, m_projectAssetRegistry);

    for(const auto& drop : m_window.TakeFileDrops())
    {
        // Win32 drops are client-relative. ImGui's main viewport uses client
        // coordinates in this non-platform-viewport backend, not desktop pixels.
        const ImVec2 origin = ImGui::GetMainViewport()->Pos;
        m_editor.ImportFileDrop(drop.files, origin.x + static_cast<float>(drop.x),
            origin.y + static_cast<float>(drop.y), inspectorPaths, m_projectAssetRegistry);
    }

    if(m_editor.TakeApplicationCloseApproval())
    {
        ImGui::EndFrame();
        m_window.Close();
        return;
    }

    if(auto request = m_editor.TakeSceneHistoryReplayRequest())
    {
        std::string error;
        bool accepted = false;

        if(request->identity.redo)
        {
            const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&activeDemo);
            const auto target = owner ? owner->GetObjectMetadata(request->object.id, &error) : std::nullopt;

            if(target)
            {
                accepted = RequestSceneObjectDeletion(*target, error, request->identity);
            }
            else
            {
                error = "Redo deletion target no longer exists in the active scene.";
            }
        }
        else
        {
            const auto identity = GetSceneObjectRestorationIdentity();

            if(identity)
            {
                accepted = RequestSceneObjectRestoration(request->object, request->insertionIndex,
                    *identity, error, false, request->identity);
            }
            else
            {
                error = "Undo restoration cannot establish the active scene/session identity.";
            }
        }

        if(!accepted)
        {
            m_editor.CompleteSceneHistoryReplay(request->identity, false, error);
        }
    }

    if(auto request = m_editor.TakeSceneObjectDeletionRequest())
    {
        std::string error;

        if(!RequestSceneObjectDeletion(*request, error))
        {
            m_editor.OnSceneObjectDeletionCompleted(activeDemo, *request, false, error);
        }
    }

    if(auto request = m_editor.TakeMeshCreationRequest())
    {
        const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&activeDemo);
        std::string error;

        if(!owner || owner->GetRuntimeIdentity() != request->runtimeIdentity)
        {
            m_editor.OnMeshCreationCompleted(false, "Scene changed before the drop could be queued.");
        }
        else if(!RequestSceneObjectCreation(std::move(request->meshReference), request->transform, error,
            "builtin:default", std::move(request->name)))
        {
            m_editor.OnMeshCreationCompleted(false, error);
        }
    }

    if(auto request = m_editor.TakeMaterialAssignmentRequest())
    {
        std::string error;

        bool queued = false;

        if(request->slotID && m_editor.GetSelectedObject() != request->selection.runtimeIndex)
        {
            error = "Material slot assignment cancelled because the selected scene object changed.";
        }
        else
        {
            queued = request->slotID ?
                RequestMaterialSlotAssignment(request->selection, *request->slotID, request->slotMaterial, error) :
                RequestMaterialAssignment(request->selection, std::move(request->reference), error);
        }

        if(queued && request->slotID)
        {
            m_pendingMaterialAssignment->requireEditorSelection = true;
        }

        if(!queued)
        {
            Log::Warning("Inspector material assignment rejected: " + error);
            m_editor.OnMaterialAssignmentCompleted(request->selection, false,
                error);
        }
    }

    ProcessEditorSceneRequests(activeDemo);
    ProcessEditorMaterialRequests();
    m_pendingSavedMaterialRevisions = m_editor.TakeSavedMaterialRevisions();
    m_pendingWorkingMaterialRevisions = m_editor.TakeWorkingMaterialRevisions();

    activeDemo.Update(m_time.GetDeltaTime());

    if constexpr (Renderer::IsFrameHitchDiagnosticsEnabled())
    {
        if (auto* mirrorScene = dynamic_cast<MirrorTestScene*>(&activeDemo))
        {
            if (const auto logging = mirrorScene->GetCameraPhaseLoggingMilliseconds())
            {
                m_renderer.RecordFrameHitchRegion(Renderer::FrameHitchRegion::CameraPhaseLogging, *logging,
                                                  mirrorScene->GetRequestedDisplacementRevision());
            }
        }
    }

    uiProbe.reset();

    Scene& activeScene = activeDemo.GetScene();

    if (activeScene.GetStructureRevision() != m_renderedSceneStructureRevision)
    {
        Renderer::FrameHitchProbe rebuildProbe(
            m_renderer,
            Renderer::FrameHitchRegion::EngineRebuild);

        if (!DrainAndResolveSceneDisplacement(m_renderer, activeDemo))
        {
            Log::Error("GPU drain or displacement resolution failed before scene rebuild; requesting shutdown.");

            m_window.Close();
            return;
        }

        if (!m_renderer.ReleaseSceneResources())
        {
            Log::Error("Scene resource release rejected before scene rebuild; requesting shutdown.");

            m_window.Close();
            return;
        }

        if (!m_renderer.InitializeSceneResources(activeScene, activeDemo.GetMaterials()))
        {
            Log::Error("Failed to rebuild renderer resources after scene structure change.");
            m_window.Close();

            return;
        }

        if (auto* mirrorScene = dynamic_cast<MirrorTestScene*>(&activeDemo))
        {
            if (!m_renderer.RegisterDisplacementHeightmap(*mirrorScene) ||

                !m_renderer.GenerateDisplacementMeshGPU(*mirrorScene, mirrorScene->GetSeedDisplacementSettings()))
            {
                Log::Error("Failed to generate Mirror Test GPU displacement geometry after scene rebuild.");
                m_window.Close();
                return;
            }
        }

        m_renderedSceneStructureRevision = activeScene.GetStructureRevision();
    }

    // Apply camera-selected displacement geometry before rendering.

    if (auto* mirrorScene = dynamic_cast<MirrorTestScene*>(&activeDemo))
    {
        Renderer::FrameHitchProbe requestProbe(
            m_renderer,
            Renderer::FrameHitchRegion::EngineRequest);

        if (mirrorScene->HasPendingDisplacementGeometryUpdate() && mirrorScene->HasCurrentDisplacementSettings())
        {
            const std::uint64_t requestedRevision = mirrorScene->GetRequestedDisplacementRevision();

            if (Renderer::IsAsyncDisplacementEnabled())
            {
                // Acknowledge first, then consider the latest dirty camera map
                // on a subsequent frame. Intermediate revisions are never queued.

                if (!displacementCompletionDelivered)
                {
                    const auto request = m_renderer.RequestDisplacementUpdate(*mirrorScene, requestedRevision);

                    if (request == Renderer::DisplacementRequestResult::Error)
                    {
                        Log::Error("Failed to request Mirror Test displacement update; requesting shutdown.");
                        m_window.Close();
                        return;
                    }

                    // Accepted and Deferred remain unacknowledged here. Async
                    // ownership never falls back to synchronous generation.
                }
            }

            else
            {
                // Borrowed LOD view remains stable during synchronous generation.
                const DisplacementMeshSettings settings = mirrorScene->GetCurrentDisplacementSettings();

                if (!m_renderer.GenerateDisplacementMeshGPU(*mirrorScene, settings))
                {
                    Log::Error(
                        "Failed to update Mirror Test GPU displacement geometry; requesting application shutdown.");
                    m_window.Close();

                    return;
                }

                mirrorScene->MarkDisplacementGeometryUpdated(requestedRevision);
            }
        }
    }

    RendererOptions rendererOptions{};
    rendererOptions.materialPreview = m_editor.GetMaterialPreviewRequest();
    m_renderer.PrepareMaterialPreviewTextures(rendererOptions.materialPreview, m_projectAssetRegistry);
    // Poll even with the browser hidden, so cancelled CPU work and completed GPU resources retire.
    // This follows asset mutations and precedes frame recording.
    m_renderer.PollMeshThumbnails(inspectorPaths, m_projectAssetRegistry);

    rendererOptions.enableRTGI = debugUI.IsRTGIEnabled();
    rendererOptions.rtgiContactAOStrength = debugUI.GetRTGIContactAOStrength();
    rendererOptions.rtgiContactAORadius = debugUI.GetRTGIContactAORadius();

    const size_t selectedObject = m_editor.GetSelectedObject();

    if (m_editor.GetSelectionStructureRevision() == activeScene.GetStructureRevision() &&

        selectedObject < activeScene.GetRenderObjects().size())
    {
        rendererOptions.selectedObject = selectedObject;
    }

    std::optional<Renderer::FrameHitchProbe> renderProbe;

    if constexpr (Renderer::IsFrameHitchDiagnosticsEnabled())
    {
        renderProbe.emplace(m_renderer, Renderer::FrameHitchRegion::EngineRender);
    }

    ProcessMeshResidencyDiagnostics();

    // Coordinated publication boundary: all editor/assets, scene update/rebuild
    // and thumbnail/preview preparation above are complete. Final validation
    // and the noexcept ownership/binding transfer finish before Render records.
    m_meshResidencyAcceptance.reset();

    if (m_meshResidencyPublication.HasCandidate())
    {
        if (auto* fileScene = dynamic_cast<FileBackedDemoScene*>(&activeDemo))
        {
            m_meshResidencyAcceptance = m_meshResidencyPublication.PublishAtBoundary(
                m_renderer, *fileScene->m_runtime, fileScene->m_assets);
        }
        else
        {
            m_meshResidencyPublication.Cancel();
            m_meshResidencyAcceptance = MeshResidencyPublication::AcceptanceResult{
                MeshResidencyPublication::Status::Stale, "Active scene is no longer the prepared file scene."};
        }
    }

    if (m_diagnosticMeshToken && m_meshResidencyAcceptance)
    {
        LogMeshResidencyDiagnostic(*m_meshResidencyAcceptance);

        if (m_meshResidencyAcceptance->published)
        {
            FinishAutomaticMeshResidency(MeshResidencyPublication::Status::Ready);
            m_residencyRequestSelection.reset();
            m_residencyRequestSourceIdentity.clear();
            m_diagnosticMeshToken.reset();
            m_diagnosticMeshReadiness.reset();
            m_diagnosticRendererLifetime = 0;
        }
        else if (m_meshResidencyAcceptance->status != MeshResidencyPublication::Status::Retry)
        {
            FinishAutomaticMeshResidency(m_meshResidencyAcceptance->status);
            CancelMeshResidencyAcceptance();
        }
    }

    const Renderer::FrameRenderResult renderResult = m_renderer.Render(activeScene, rendererOptions);
    renderProbe.reset();

    if constexpr (Renderer::IsFrameHitchDiagnosticsEnabled())
    {
        m_diagnosticRenderOutcome = renderResult;
    }

    if (renderResult == Renderer::FrameRenderResult::Error)
    {

        Log::Error("Frame rendering failed; requesting application shutdown.");
        m_window.Close();
        return;
    }

    if (renderResult == Renderer::FrameRenderResult::Deferred)
    {
        // DebugUI::BeginFrame already started this CPU UI frame. Finish it
        // without recording GPU UI commands or presenting a new frame.
        ImGui::EndFrame();
        // Preserve transform dirtiness until a frame actually records it.
        // The next update can poll completion without submitting another frame.
        return;
    }


    activeScene.ClearChangedTransforms();
}

void Engine::Shutdown()
{
    if (m_diagnosticMeshToken || m_diagnosticCPUPreparation)
    {
        Log::Info("Mesh residency request canceled for Engine shutdown.");
    }

    CancelMeshResidencyAcceptance();
    m_automaticMeshResidency = {};
    m_automaticResidencyIntent = 0;

    ENGINE_ASSERT(m_state == EngineState::Running);

    Log::Info("Engine shutdown: starting.");
    m_texturePreparation.Shutdown();
    m_meshPreparation.Shutdown();
    m_pendingWorkingMaterialRevisions.clear();
    m_workingMaterialRestores.clear();
    m_workingMaterialOverride.reset();
    m_pendingSavedMaterialRevisions.clear();
    m_savedMaterialRevisionQueue.clear();
    m_savedMaterialObservations.clear();

    m_state = EngineState::Stopped;

    IDemoScene* activeDemo = m_demoSceneManager.TryGetActiveScene();

    if (!activeDemo)
    {
        // Only the successfully released scene-switch path may
        // reach shutdown without an active scene owner.
        if (!m_sceneResourcesReleasedForSwitch)
        {
            Log::Error(
                "Engine shutdown: no active scene without confirmed "
                "scene-resource release; aborting without teardown.");
            std::abort();
        }

        Log::Info(
            "Engine shutdown: scene switch failed after prior "
            "scene resources were released.");
    }
    else
    {
        if (!DrainAndResolveSceneDisplacement(m_renderer, *activeDemo))
        {
            Log::Error(
                "Engine shutdown: GPU drain or displacement "
                "resolution failed; aborting without teardown.");
            std::abort();
        }

        if (!m_renderer.ReleaseSceneResources())
        {
            Log::Error(
                "Engine shutdown: scene resource release rejected; "
                "aborting without teardown.");
            std::abort();
        }
    }

    m_editor.ResetThumbnailRequests();
    m_demoSceneManager.Shutdown();
    m_meshCPUCache.Clear();
    m_renderer.Shutdown();
    m_window.Shutdown();

    Log::Info("Engine shutdown: finished.");
}

void Engine::ProcessSceneTextureStreaming()
{
    auto* scene = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());

    if(!scene || m_demoSceneManager.HasPendingSceneChange())
    {
        return;
    }

    const auto mayPublish = [&](const AssetID& material, const MaterialAssetData& baseline)
    {
        const auto runtime = scene->GetRuntimeIdentity();

        for(const auto& queued : m_savedMaterialRevisionQueue)
        {
            if(queued.runtimeIdentity == runtime && queued.saved.assetID == material)
            {
                return false;
            }
        }

        if(m_workingMaterialOverride && m_workingMaterialOverride->parameters &&
            m_workingMaterialOverride->selection.runtime.runtimeIdentity == runtime &&
            m_workingMaterialOverride->selection.runtime.assetID == material)
        {
            return MaterialParameterPropagation::SameTextures(baseline, *m_workingMaterialOverride->parameters);
        }

        return true;
    };

    scene->AdvanceTextureStreaming(m_texturePreparation, m_renderer, mayPublish);
}

void Engine::ProcessSavedMaterialRevisions()
{
    auto pending = std::move(m_pendingSavedMaterialRevisions);
    m_pendingSavedMaterialRevisions.clear();
    auto* scene = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    std::error_code pathError;
    const auto project = std::filesystem::canonical(ProjectLocator::GetDefaultProjectRoot(), pathError);

    if(!scene || pathError)
    {
        if(scene)
        {
            scene->CancelSavedMaterialRevision();
        }

        m_savedMaterialRevisionQueue.clear();
        m_savedMaterialObservations.clear();
        return;
    }

    const auto runtimeIdentity = scene->GetRuntimeIdentity();
    const auto structureRevision = scene->GetScene().GetStructureRevision();
    std::erase_if(m_savedMaterialObservations, [&](const SavedMaterialObservation& observation)
    {
        const auto current = scene->GetMaterialVersions(project, observation.saved.assetID);
        const auto* record = m_projectAssetRegistry.Find(observation.saved.assetID);
        return !current || !observation.runtime.Matches(*current) || observation.saved.projectIdentity != project ||
            !record || record->relativePath != observation.saved.relativePath;
    });
    // Keep only the newest receipt per asset, in stable FIFO order, with bounded admission.
    for(auto& notification : pending)
    {
        auto found = std::find_if(m_savedMaterialRevisionQueue.begin(), m_savedMaterialRevisionQueue.end(),
            [&](const auto& queued) { return queued.saved.projectIdentity == notification.saved.projectIdentity &&
                queued.saved.assetID == notification.saved.assetID; });

        if(found != m_savedMaterialRevisionQueue.end())
        {
            if(notification.saved.sequence > found->saved.sequence)
            {
                *found = std::move(notification);
            }
        }
        else if(m_savedMaterialRevisionQueue.size() < 8)
        {
            m_savedMaterialRevisionQueue.push_back(std::move(notification));
        }
        else
        {
            Log::Warning("Saved material revision queue is full; previous live appearance retained. Save again to retry.");
        }
    }

    if(m_savedMaterialRevisionQueue.empty())
    {
        return;
    }

    const auto abandon = [&]()
    {
        scene->CancelSavedMaterialRevision();
        m_savedMaterialRevisionQueue.erase(m_savedMaterialRevisionQueue.begin());
    };

    try
    {
        const auto& notification = m_savedMaterialRevisionQueue.front();
        const auto& saved = notification.saved;
        const auto* record = m_projectAssetRegistry.Find(saved.assetID);

        if(notification.runtimeIdentity != runtimeIdentity || notification.structureRevision != structureRevision ||
           saved.projectIdentity != project || !record || record->type != AssetType::Material || record->relativePath != saved.relativePath)
        {
            Log::Warning("Saved material revision rejected: project, scene, or registration changed.");
            abandon();
            return;
        }

        const auto versions = scene->GetMaterialVersions(project, saved.assetID);

        if(!versions || versions->catalogIdentity != notification.catalogIdentity || versions->versions.empty())
        {
            abandon();
            return;
        }

        const auto existing = std::find_if(m_savedMaterialObservations.begin(), m_savedMaterialObservations.end(),
            [&](const auto& observation) { return observation.saved.assetID == saved.assetID; });

        if(existing != m_savedMaterialObservations.end() && existing->saved.sequence >= saved.sequence)
        {
            abandon();
            return;
        }

        const auto sourceStillCurrent = [&]()
        {
            const auto* current = m_projectAssetRegistry.Find(saved.assetID);
            const auto resolved = ProjectAssetResolver::Resolve(ProjectPaths(project), m_projectAssetRegistry, saved.assetID);
            std::error_code error;

            if(!current || current->type != AssetType::Material || current->relativePath != saved.relativePath ||
               resolved.status != AssetAvailability::Available ||
               std::filesystem::file_size(resolved.resolvedPath, error) != saved.savedBytes.size() || error)
            {
                return false;
            }

            std::ifstream file(resolved.resolvedPath, std::ios::binary);
            std::string bytes(saved.savedBytes.size(), '\0');
            file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            return file && bytes == saved.savedBytes && file.peek() == std::char_traits<char>::eof();
        };

        if(!sourceStillCurrent())
        {
            Log::Warning("Saved material revision rejected: saved source changed or became unavailable.");
            abandon();
            return;
        }

        MaterialAssetData data;
        std::string error;

        if(!MaterialAssetSerializer::Deserialize(nlohmann::json::parse(saved.savedBytes), data, &error))
        {
            Log::Warning("Saved material revision rejected: " + error);
            abandon();
            return;
        }

        // All fallible bookkeeping is prepared before the catalog can accept publication.
        SavedMaterialObservation observation{saved, *versions};
        const bool hadObservation = existing != m_savedMaterialObservations.end();
        const auto index = hadObservation ? static_cast<size_t>(existing - m_savedMaterialObservations.begin()) : 0;

        if(!hadObservation)
        {
            m_savedMaterialObservations.reserve(m_savedMaterialObservations.size() + 1);
        }

        const auto result = scene->AdvanceSavedMaterialRevision(*versions, data, saved.savedBytes, m_renderer, sourceStillCurrent, error);

        if(result == SavedMaterialRevisionResult::Pending)
        {
            return;
        }

        if(result == SavedMaterialRevisionResult::Rejected)
        {
            Log::Warning("Saved material revision retained the previous live appearance: " + error);
            abandon();
            return;
        }

        if(hadObservation)
        {
            m_savedMaterialObservations[index] = std::move(observation);
        }
        else
        {
            m_savedMaterialObservations.push_back(std::move(observation));
        }

        if(m_workingMaterialOverride && m_workingMaterialOverride->selection.working.assetID == saved.assetID &&
           m_workingMaterialOverride->selection.runtime.runtimeIdentity == versions->runtimeIdentity &&
           m_workingMaterialOverride->promotionSequence == saved.sequence)
        {
            m_workingMaterialOverride->parameters.reset();
            m_workingMaterialOverride->lastPublished.reset();
            m_workingMaterialOverride->promotionSequence = 0;
            m_workingMaterialOverride->lastTableRevision = m_renderer.GetMaterialTableRevision();
        }

        m_savedMaterialRevisionQueue.erase(m_savedMaterialRevisionQueue.begin());
        Log::Info("Saved material revision propagated to all matching runtime versions.");
    }
    catch(const std::exception& error)
    {
        Log::Warning(std::string("Saved material revision rejected: ") + error.what());
        abandon();
    }
}

void Engine::ProcessWorkingMaterialNotifications()
{
    auto notifications = std::move(m_pendingWorkingMaterialRevisions);
    m_pendingWorkingMaterialRevisions.clear();
    auto* scene = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    std::error_code error;
    const auto root = std::filesystem::canonical(ProjectLocator::GetDefaultProjectRoot(), error);

    if(!scene || error)
    {
        m_workingMaterialOverride.reset();
        m_workingMaterialRestores.clear();
        return;
    }

    for(auto& notification : notifications)
    {
        const auto& revision = notification.working;
        const auto& identity = notification.runtime;

        if(revision.sequence <= m_lastWorkingMaterialSequence || revision.projectIdentity != root ||
           identity.runtimeIdentity != scene->GetRuntimeIdentity() || identity.structureRevision != scene->GetScene().GetStructureRevision())
        {
            continue;
        }

        const bool leaving = revision.action == MaterialWorkingRevision::Action::Leave;
        const auto* record = m_projectAssetRegistry.Find(revision.assetID);

        if(!leaving && (!record || record->type != AssetType::Material || record->relativePath != revision.relativePath))
        {
            continue;
        }

        m_lastWorkingMaterialSequence = revision.sequence;
        const bool same = m_workingMaterialOverride &&
            m_workingMaterialOverride->selection.runtime.assetID == identity.assetID &&
            m_workingMaterialOverride->selection.runtime.runtimeIdentity == identity.runtimeIdentity &&
            m_workingMaterialOverride->selection.runtime.catalogIdentity == identity.catalogIdentity;

        if(leaving)
        {
            m_workingMaterialRestores.push_back(identity);

            if(same)
            {
                m_workingMaterialOverride.reset();
            }

            continue;
        }

        if(!same)
        {
            if(m_workingMaterialOverride)
            {
                m_workingMaterialRestores.push_back(m_workingMaterialOverride->selection.runtime);
            }

            m_workingMaterialOverride = WorkingMaterialOverride{notification};
        }

        auto& active = *m_workingMaterialOverride;
        active.selection = std::move(notification);
        active.promotionSequence = 0;
        active.lastTableRevision = 0;

        switch(active.selection.working.action)
        {
        case MaterialWorkingRevision::Action::Draft:
        case MaterialWorkingRevision::Action::Saved:
            active.parameters = active.selection.working.parameters;

            if(active.selection.working.action == MaterialWorkingRevision::Action::Saved)
            {
                active.promotionSequence = active.selection.working.saveSequence;
            }

            break;
        case MaterialWorkingRevision::Action::Restore:
            active.parameters.reset();
            active.lastPublished.reset();
            break;
        case MaterialWorkingRevision::Action::Invalid:
            active.parameters = active.lastPublished;
            break;
        default:
            break;
        }
    }
}

void Engine::PublishWorkingMaterialParameters()
{
    auto* scene = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());

    if(!scene)
    {
        m_workingMaterialOverride.reset();
        m_workingMaterialRestores.clear();
        return;
    }

    for(const auto& identity : m_workingMaterialRestores)
    {
        std::string error;

        if(!scene->PublishWorkingMaterialParameters(identity, std::nullopt, m_renderer, error))
        {
            Log::Warning("Working material restoration rejected: " + error);
        }
    }

    m_workingMaterialRestores.clear();

    if(!m_workingMaterialOverride)
    {
        return;
    }

    auto& active = *m_workingMaterialOverride;
    auto& identity = active.selection.runtime;
    const auto current = scene->GetMaterialVersions(identity.projectIdentity, identity.assetID);

    if(current && identity.projectIdentity == current->projectIdentity && identity.assetID == current->assetID &&
       identity.catalogIdentity == current->catalogIdentity && identity.runtimeIdentity == current->runtimeIdentity &&
       identity.structureRevision == current->structureRevision && identity.versions != current->versions)
    {
        // An approved assignment may add a fresh version to the same live catalog.
        // Requery ownership before including it in the active parameter override.
        identity = *current;
        active.lastTableRevision = 0;
    }

    if(!current || !identity.Matches(*current))
    {
        // Restoration is restricted to the exact retained catalog/scene identities.
        std::string error;
        scene->PublishWorkingMaterialParameters(identity, std::nullopt, m_renderer, error);
        m_workingMaterialOverride.reset();
        return;
    }

    if(active.lastTableRevision == m_renderer.GetMaterialTableRevision())
    {
        return;
    }

    std::string error;

    if(scene->PublishWorkingMaterialParameters(identity, active.parameters, m_renderer, error))
    {
        active.lastPublished = active.parameters;
        active.lastTableRevision = m_renderer.GetMaterialTableRevision();
    }
    else
    {
        Log::Warning("Working material parameters retained their previous live appearance: " + error);
        active.parameters = active.lastPublished;
        active.lastTableRevision = 0; // Retry the last valid appearance on the next safe boundary.
    }
}

bool Engine::RequestSceneObjectCreation(
    std::string meshReference, const Transform& transform, std::string& error,
    std::string materialReference, std::string name)
{
    error.clear();
    auto* owner = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());

    if(m_state != EngineState::Running || !owner || m_pendingSceneObjectCreation ||
        m_pendingSceneObjectRestoration || m_pendingSceneObjectDeletion ||
        m_pendingMaterialAssignment || m_pendingEditorSceneOperation ||
        m_demoSceneManager.HasPendingSceneChange() || m_editor.HasBlockingObjectCreation() || m_editor.HasPendingSceneHistoryOperation())
    {
        error = "Object creation requires an active file-backed scene without pending editor/scene operations.";
        return false;
    }

    if(!AssetID::ParseReference(meshReference) ||
        (materialReference != "builtin:default" && !AssetID::ParseReference(materialReference)))
    {
        error = "Object creation requires canonical mesh and material asset references (or builtin:default material).";
        return false;
    }

    std::error_code pathError;
    const auto project = std::filesystem::canonical(owner->m_projectRoot, pathError);

    if(pathError)
    {
        error = "Object creation project identity is unavailable: " + pathError.message();
        return false;
    }

    m_pendingSceneObjectCreation = SceneObjectCreationRequest{project, std::move(meshReference),
        std::move(materialReference), std::move(name), transform, owner->GetRuntimeIdentity(),
        owner->GetScene().GetStructureRevision()};
    m_lastSceneObjectCreation.reset();
    return true;
}

bool Engine::ExecuteSceneObjectCreation()
{
    if(!m_pendingSceneObjectCreation)
    {
        return true;
    }

    auto request = std::move(*m_pendingSceneObjectCreation);
    m_pendingSceneObjectCreation.reset();
    m_lastSceneObjectCreation.emplace();
    auto& result = *m_lastSceneObjectCreation;
    auto* owner = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    std::error_code pathError;
    const auto project = owner ? std::filesystem::canonical(owner->m_projectRoot, pathError) :
        std::filesystem::path{};

    if(!owner || pathError || project != request.projectIdentity ||
        owner->GetRuntimeIdentity() != request.runtimeIdentity ||
        owner->GetScene().GetStructureRevision() != request.structureRevision ||
        m_pendingMaterialAssignment || m_pendingEditorSceneOperation || m_demoSceneManager.HasPendingSceneChange() ||
        m_editor.HasBlockingObjectCreation() || owner->m_objectMutationCandidate)
    {
        result.error = "Object creation cancelled: scene/project identity changed or another operation is active.";
        Log::Warning(result.error);
        return true;
    }

    std::vector<Material*> materials;
    std::optional<SceneObjectMetadata> selection;
    std::string activeName(m_demoSceneManager.GetActiveSceneName());

    try
    {
        owner->m_objectMutationCandidate = owner->m_runtime->PrepareObjectCreation(owner->m_assets,
            std::move(request.meshReference), request.transform, std::move(request.materialReference),
            std::move(request.name), result.objectID, &result.error);

        if(owner->m_objectMutationCandidate)
        {
            const auto& mapping = owner->m_objectMutationCandidate->GetObjectMapping();
            const auto index = mapping.idToRuntimeIndex.at(result.objectID);
            selection = owner->m_objectMutationCandidate->GetObjectMetadata(index, &result.error);
            materials = owner->GetMaterials();
        }
    }
    catch(const std::exception& error)
    {
        result.error = error.what();
    }

    if(!owner->m_objectMutationCandidate || !selection || materials.empty())
    {
        result.error += " " + owner->m_assets.GetLastMeshError() + " " + owner->m_assets.GetLastMaterialError();
        owner->m_objectMutationCandidate.reset();
        result.objectID.clear();
        Log::Warning("Object creation preparation rejected: " + result.error);
        return true;
    }

    // Both runtime addresses and all catalog assets remain owned throughout
    // initialization, cleanup, and restoration. No active CPU state changes yet.
    // Capture intent identity before cancellation. Never transfer old request
    // handles/epochs; restart discovery only after the checked runtime swap.
    const auto previousRuntimeIdentity = owner->GetRuntimeIdentity();

    // Invalidate old-session CPU subscribers and residency candidates before the
    // checked renderer replacement. Manager pins survive until GPU-safe release.
    CancelMeshResidencyAcceptance();
    m_meshPreparation.Invalidate();

    if(!DrainAndResolveSceneDisplacement(m_renderer, *owner) || !m_renderer.ReleaseSceneResources())
    {
        result.error = "Object creation could not prove renderer release safety; retaining both runtimes and stopping.";
        Log::Error(result.error);
        return false;
    }

    m_sceneResourcesReleasedForSwitch = true;
    const auto initialize = [&](Scene& scene)
    {
        try
        {
            return m_renderer.InitializeSceneResources(scene, materials);
        }
        catch(const std::exception& error)
        {
            result.error = error.what();
            return false;
        }
        catch(...)
        {
            result.error = "Unknown renderer initialization exception.";
            return false;
        }
    };

    Scene& candidateScene = owner->m_objectMutationCandidate->GetScene();

    if(initialize(candidateScene))
    {
        // Swapping owners preserves the exact Scene address initialized by Renderer.
        // The prior runtime contains only asset borrows and can now be discarded.
        owner->m_runtime.swap(owner->m_objectMutationCandidate);
        owner->m_objectMutationCandidate.reset();
        owner->CancelSavedMaterialRevision();

        m_renderedSceneStructureRevision = owner->GetScene().GetStructureRevision();
        m_sceneResourcesReleasedForSwitch = false;
        result.succeeded = true;
        result.error.clear();

        m_editor.PublishCreatedObjectSelection(owner->GetScene(), std::move(*selection), std::move(activeName));
        Log::Info("Created scene object " + result.objectID + "; save the scene to persist it.");
        ContinueAutomaticMeshResidencyAfterReplacement(previousRuntimeIdentity);
        return true;
    }

    if(!m_renderer.CleanupFailedSceneInitialization(candidateScene))
    {
        result.error = "Object creation cleanup could not prove GPU safety; retaining both runtimes and stopping.";
        Log::Error(result.error);
        return false;
    }

    // Original runtime/document/selection/history have never been changed.
    owner->m_objectMutationCandidate.reset();
    result.objectID.clear();

    if(!initialize(owner->GetScene()))
    {
        // Retain the original runtime and all assets for checked shutdown.
        result.error = "Object creation renderer restoration failed; retaining ownership and stopping.";
        Log::Error(result.error);
        return false;
    }

    m_sceneResourcesReleasedForSwitch = false;
    result.error = "Candidate renderer initialization failed; previous scene restored.";
    Log::Warning(result.error);
    ContinueAutomaticMeshResidencyAfterReplacement(previousRuntimeIdentity);
    return true;
}

bool Engine::RequestSceneObjectDeletion(std::string_view objectID, std::string& error)
{
    error.clear();
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    const auto selection = owner ? owner->GetObjectMetadata(objectID, &error) : std::nullopt;

    if(!selection)
    {
        error = "Object deletion requires an existing persistent object ID in the active file-backed scene.";
        return false;
    }

    return RequestSceneObjectDeletion(*selection, error);
}

bool Engine::RequestSceneObjectDeletion(const SceneObjectMetadata& selection, std::string& error,
    std::optional<Editor::HistoryReplayIdentity> replay)
{
    error.clear();
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());

    if(m_state != EngineState::Running || !owner || m_pendingSceneObjectDeletion ||
        m_pendingSceneObjectCreation || m_pendingSceneObjectRestoration || m_pendingMaterialAssignment || m_pendingEditorSceneOperation ||
        m_demoSceneManager.HasPendingSceneChange() || m_editor.HasBlockingObjectCreation())
    {
        error = "Object deletion requires an active file-backed scene without pending editor/scene operations.";
        return false;
    }

    if((replay && (!replay->redo || !m_editor.MatchesPendingHistoryReplay(*replay))) ||
        (!replay && m_editor.HasPendingSceneHistoryOperation()))
    {
        error = "Deletion rejected: a different history operation is pending.";
        return false;
    }

    const auto current = owner->GetObjectMetadata(selection.objectID, &error);

    if(!current || current->runtimeIdentity != selection.runtimeIdentity || current->sceneID != selection.sceneID ||
        current->structureRevision != selection.structureRevision || current->runtimeIndex != selection.runtimeIndex)
    {
        error = "Object deletion rejected: the persistent object selection is missing or stale.";
        return false;
    }

    std::error_code pathError;
    const auto project = std::filesystem::canonical(owner->m_projectRoot, pathError);

    if(pathError)
    {
        error = "Object deletion project identity is unavailable: " + pathError.message();
        return false;
    }

    if(!m_editor.CanPreserveSceneHistory(*owner, project, m_editor.GetSceneHistoryEpoch()))
    {
        error = "Deletion rejected: scene history session bindings are stale.";
        return false;
    }

    const auto operationID = replay ? replay->operationID : m_editor.AllocateSceneHistoryOperationID();
    m_pendingSceneObjectDeletion = SceneObjectDeletionRequest{project, *current, operationID,
        m_editor.GetSceneHistoryEpoch(), std::move(replay)};
    m_lastSceneObjectDeletion.reset();
    return true;
}

bool Engine::ExecuteSceneObjectDeletion()
{
    if(!m_pendingSceneObjectDeletion)
    {
        return true;
    }

    auto request = std::move(*m_pendingSceneObjectDeletion);
    m_pendingSceneObjectDeletion.reset();
    m_lastSceneObjectDeletion.emplace();
    auto& result = *m_lastSceneObjectDeletion;
    result.operationID = request.operationID;
    result.historyEpoch = request.historyEpoch;
    result.objectID = request.selection.objectID;
    auto* owner = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    std::error_code pathError;
    const auto project = owner ? std::filesystem::canonical(owner->m_projectRoot, pathError) :
        std::filesystem::path{};
    const auto current = owner ? owner->GetObjectMetadata(request.selection.objectID, &result.error) : std::nullopt;

    if(!owner || pathError || project != request.projectIdentity || !current ||
        current->runtimeIdentity != request.selection.runtimeIdentity || current->sceneID != request.selection.sceneID ||
        current->structureRevision != request.selection.structureRevision ||
        m_pendingSceneObjectCreation || m_pendingSceneObjectRestoration || m_pendingMaterialAssignment || m_pendingEditorSceneOperation ||
        m_demoSceneManager.HasPendingSceneChange() || m_editor.HasBlockingObjectCreation() || owner->m_objectMutationCandidate)
    {
        result.error = "Object deletion cancelled: scene/project/object identity changed or another operation is active.";
        Log::Warning(result.error);
        return true;
    }

    if(request.historyEpoch != m_editor.GetSceneHistoryEpoch() ||
        !m_editor.CanPreserveSceneHistory(*owner, project, request.historyEpoch) ||
        (request.historyReplay && !m_editor.MatchesPendingHistoryReplay(*request.historyReplay)))
    {
        result.error = "Deletion cancelled: history operation/session identity changed.";
        return true;
    }

    SceneObjectDocument deletedSnapshot;
    size_t deletedDocumentIndex = 0;
    std::optional<std::string> selectionBefore;

    std::vector<Material*> materials;
    std::optional<SceneObjectMetadata> survivingSelection;
    std::string activeName(m_demoSceneManager.GetActiveSceneName());

    try
    {
        if(!owner->m_runtime->CaptureObjectDeletionSnapshot(current->objectID,
            deletedSnapshot, deletedDocumentIndex, &result.error))
        {
            return true;
        }

        owner->m_objectMutationCandidate = owner->m_runtime->PrepareObjectDeletion(current->objectID, &result.error, &owner->m_assets);

        if(owner->m_objectMutationCandidate)
        {
            const auto selectedIndex = m_editor.GetSelectedObject();

            if(selectedIndex < owner->GetScene().GetRenderObjects().size() &&
                m_editor.GetSelectionStructureRevision() == owner->GetScene().GetStructureRevision())
            {
                const auto selected = owner->GetObjectMetadata(selectedIndex, &result.error);

                if(!selected)
                {
                    owner->m_objectMutationCandidate.reset();
                    Log::Warning("Object deletion could not validate the editor selection: " + result.error);
                    return true;
                }

                selectionBefore = selected->objectID;

                if(selected->objectID != current->objectID)
                {
                    survivingSelection = owner->m_objectMutationCandidate->GetObjectMetadata(selected->objectID, &result.error);

                    if(!survivingSelection)
                    {
                        owner->m_objectMutationCandidate.reset();
                        Log::Warning("Object deletion could not remap the surviving selection: " + result.error);
                        return true;
                    }
                }
            }

            materials = owner->GetMaterials();
        }
    }
    catch(const std::exception& error)
    {
        result.error = error.what();
    }

    if(!owner->m_objectMutationCandidate || materials.empty())
    {
        owner->m_objectMutationCandidate.reset();
        Log::Warning("Object deletion preparation rejected: " + result.error);
        return true;
    }

    std::optional<std::string> selectionAfter;

    if(survivingSelection)
    {
        selectionAfter = survivingSelection->objectID;
    }

    if(!m_editor.StageObjectDeletionHistory(std::move(deletedSnapshot), deletedDocumentIndex,
        std::move(selectionBefore), std::move(selectionAfter), request.operationID,
        request.historyEpoch, request.historyReplay.has_value(), result.error))
    {
        owner->m_objectMutationCandidate.reset();
        Log::Warning(result.error);
        return true;
    }

    // Original document, runtime mappings, selection and undo history remain untouched.
    // The catalog owns shared meshes/materials through candidate cleanup and restoration.
    // Capture intent identity before cancellation. Never transfer old request
    // handles/epochs; restart discovery only after the checked runtime swap.
    const auto previousRuntimeIdentity = owner->GetRuntimeIdentity();

    // Invalidate old-session CPU subscribers and residency candidates before the
    // checked renderer replacement. Manager pins survive until GPU-safe release.
    CancelMeshResidencyAcceptance();
    m_meshPreparation.Invalidate();

    if(!DrainAndResolveSceneDisplacement(m_renderer, *owner) || !m_renderer.ReleaseSceneResources())
    {
        result.error = "Object deletion could not prove renderer release safety; retaining both runtimes and stopping.";
        Log::Error(result.error);
        return false;
    }

    m_sceneResourcesReleasedForSwitch = true;
    const auto initialize = [&](Scene& scene)
    {
        try
        {
            return m_renderer.InitializeSceneResources(scene, materials);
        }
        catch(const std::exception& error)
        {
            result.error = error.what();
            return false;
        }
        catch(...)
        {
            result.error = "Unknown renderer initialization exception.";
            return false;
        }
    };
    Scene& candidateScene = owner->m_objectMutationCandidate->GetScene();

    if(initialize(candidateScene))
    {
        owner->m_runtime.swap(owner->m_objectMutationCandidate);
        owner->m_objectMutationCandidate.reset();
        owner->CancelSavedMaterialRevision();

        m_renderedSceneStructureRevision = owner->GetScene().GetStructureRevision();
        m_sceneResourcesReleasedForSwitch = false;
        result.succeeded = true;
        result.error.clear();

        m_editor.PublishRestoredObjectSelection(*owner, std::move(survivingSelection), std::move(activeName));
        Log::Info("Deleted scene object " + result.objectID + "; save the scene to persist it.");
        ContinueAutomaticMeshResidencyAfterReplacement(previousRuntimeIdentity);
        return true;
    }

    if(!m_renderer.CleanupFailedSceneInitialization(candidateScene))
    {
        result.error = "Object deletion cleanup could not prove GPU safety; retaining both runtimes and stopping.";
        Log::Error(result.error);
        return false;
    }

    owner->m_objectMutationCandidate.reset();

    if(!initialize(owner->GetScene()))
    {
        result.error = "Object deletion renderer restoration failed; retaining ownership and stopping.";
        Log::Error(result.error);
        return false;
    }

    m_sceneResourcesReleasedForSwitch = false;
    result.error = "Deletion renderer initialization failed; previous scene restored.";
    Log::Warning(result.error);
    ContinueAutomaticMeshResidencyAfterReplacement(previousRuntimeIdentity);
    return true;
}

std::optional<Engine::SceneObjectRestorationIdentity> Engine::GetSceneObjectRestorationIdentity() const
{
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());

    if(m_state != EngineState::Running || !owner || owner->GetPersistentSceneID().empty() ||
        m_editor.GetSceneHistoryEpoch() == 0)
    {
        return std::nullopt;
    }

    std::error_code error;
    const auto project = std::filesystem::canonical(owner->GetProjectRoot(), error);

    if(error)
    {
        return std::nullopt;
    }

    return SceneObjectRestorationIdentity{project, owner->GetScenePath().lexically_normal(),
        std::string(owner->GetPersistentSceneID()), owner->GetRuntimeIdentity(),
        owner->GetScene().GetStructureRevision(), m_editor.GetSceneHistoryEpoch()};
}

bool Engine::RequestSceneObjectRestoration(const SceneObjectDocument& object, size_t insertionIndex,
    const SceneObjectRestorationIdentity& identity, std::string& error, bool selectRestored,
    std::optional<Editor::HistoryReplayIdentity> replay)
{
    error.clear();
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    const auto current = GetSceneObjectRestorationIdentity();

    if(!owner || !current || *current != identity || m_pendingSceneObjectRestoration ||
        m_pendingSceneObjectCreation || m_pendingSceneObjectDeletion || m_pendingMaterialAssignment ||
        m_pendingEditorSceneOperation || m_demoSceneManager.HasPendingSceneChange() ||
        m_editor.HasBlockingObjectCreation() || owner->m_objectMutationCandidate)
    {
        error = "Object restoration rejected: scene/session identity is stale or another operation is active.";
        return false;
    }

    if((replay && (replay->redo || !m_editor.MatchesPendingHistoryReplay(*replay))) ||
        (!replay && m_editor.HasPendingSceneHistoryOperation()))
    {
        error = "Restoration rejected: a different history operation is pending.";
        return false;
    }

    if(!m_editor.CanPreserveSceneHistory(*owner, identity.projectIdentity, identity.historyEpoch))
    {
        error = "Object restoration cannot preserve the current editing session history.";
        return false;
    }

    if(insertionIndex > owner->GetScene().GetRenderObjects().size() ||
        owner->m_runtime->GetObjectMapping().idToRuntimeIndex.contains(object.id))
    {
        error = "Object restoration requires an unused persistent ID and a valid document insertion index.";
        return false;
    }

    SceneDocument validation = owner->m_runtime->GetFileScene().GetDocument();
    validation.objects = {object};

    if(!ValidateSceneDocument(validation, &error))
    {
        return false;
    }

    m_pendingSceneObjectRestoration = SceneObjectRestorationRequest{object, identity, insertionIndex, selectRestored, std::move(replay)};
    m_lastSceneObjectRestoration.reset();
    return true;
}

bool Engine::ExecuteSceneObjectRestoration()
{
    if(!m_pendingSceneObjectRestoration)
    {
        return true;
    }

    auto request = std::move(*m_pendingSceneObjectRestoration);
    m_pendingSceneObjectRestoration.reset();
    m_lastSceneObjectRestoration.emplace();
    auto& result = *m_lastSceneObjectRestoration;
    result.historyReplay = request.historyReplay;
    result.objectID = request.object.id;
    result.documentIndex = request.insertionIndex;

    auto* owner = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    const auto identity = GetSceneObjectRestorationIdentity();

    if(!owner || !identity || *identity != request.identity || m_pendingSceneObjectCreation ||
        m_pendingSceneObjectDeletion || m_pendingMaterialAssignment || m_pendingEditorSceneOperation ||
        m_demoSceneManager.HasPendingSceneChange() || m_editor.HasBlockingObjectCreation() ||
        owner->m_objectMutationCandidate ||
        !m_editor.CanPreserveSceneHistory(*owner, identity->projectIdentity, identity->historyEpoch))
    {
        result.error = "Object restoration cancelled: scene/session identity changed or another operation is active.";
        Log::Warning(result.error);
        return true;
    }

    if(request.historyReplay && !m_editor.MatchesPendingHistoryReplay(*request.historyReplay))
    {
        result.error = "Restoration cancelled: history replay identity changed.";
        return true;
    }

    std::vector<Material*> materials;
    std::optional<SceneObjectMetadata> restored;
    std::optional<SceneObjectMetadata> selection;
    auto publishedIdentity = request.identity;
    std::string activeName(m_demoSceneManager.GetActiveSceneName());

    const auto prepareSelection = [&]() -> bool
    {
        if(request.historyReplay)
        {
            if(!request.historyReplay->selectionID)
            {
                return true;
            }

            selection = owner->m_objectMutationCandidate->GetObjectMetadata(
                *request.historyReplay->selectionID, &result.error);
            return selection.has_value();
        }

        if(request.selectRestored)
        {
            selection = restored;
            return true;
        }

        const auto index = m_editor.GetSelectedObject();

        if(index >= owner->GetScene().GetRenderObjects().size())
        {
            return true;
        }

        if(m_editor.GetSelectionStructureRevision() != owner->GetScene().GetStructureRevision())
        {
            result.error = "Object restoration cannot validate the surviving editor selection.";
            return false;
        }

        const auto selected = owner->GetObjectMetadata(index, &result.error);

        if(!selected)
        {
            return false;
        }

        selection = owner->m_objectMutationCandidate->GetObjectMetadata(selected->objectID, &result.error);
        return selection.has_value();
    };

    try
    {
        owner->m_objectMutationCandidate = owner->PrepareObjectRestoration(request.object,
            request.insertionIndex, &result.error);

        if(owner->m_objectMutationCandidate)
        {
            restored = owner->m_objectMutationCandidate->GetObjectMetadata(request.object.id, &result.error);

            if(restored && prepareSelection())
            {
                materials = owner->GetMaterials();
                publishedIdentity.runtimeIdentity = owner->m_objectMutationCandidate->GetRuntimeIdentity();
                publishedIdentity.structureRevision = owner->m_objectMutationCandidate->GetScene().GetStructureRevision();
            }
        }
    }
    catch(const std::exception& error)
    {
        result.error = error.what();
    }

    if(!owner->m_objectMutationCandidate || !restored || materials.empty())
    {
        owner->m_objectMutationCandidate.reset();

        if(result.error.empty())
        {
            result.error = "Restoration preparation failed or the scene material catalog is unavailable.";
        }

        result.error += " " + owner->m_assets.GetLastMeshError() + " " + owner->m_assets.GetLastMaterialError();
        Log::Warning("Object restoration preparation rejected: " + result.error);
        return true;
    }

    // Catalog assets and both Scene addresses remain owned until checked cleanup.
    // No document, selection, or history publication has occurred at this point.
    // Capture intent identity before cancellation. Never transfer old request
    // handles/epochs; restart discovery only after the checked runtime swap.
    const auto previousRuntimeIdentity = owner->GetRuntimeIdentity();

    // Invalidate old-session CPU subscribers and residency candidates before the
    // checked renderer replacement. Manager pins survive until GPU-safe release.
    CancelMeshResidencyAcceptance();
    m_meshPreparation.Invalidate();

    if(!DrainAndResolveSceneDisplacement(m_renderer, *owner) || !m_renderer.ReleaseSceneResources())
    {
        result.error = "Object restoration could not prove renderer release safety; retaining both runtimes and stopping.";
        Log::Error(result.error);
        return false;
    }

    m_sceneResourcesReleasedForSwitch = true;
    const auto initialize = [&](Scene& scene)
    {
        try
        {
            return m_renderer.InitializeSceneResources(scene, materials);
        }
        catch(const std::exception& error)
        {
            result.error = error.what();
            return false;
        }
        catch(...)
        {
            result.error = "Unknown renderer initialization exception.";
            return false;
        }
    };
    Scene& candidateScene = owner->m_objectMutationCandidate->GetScene();

    if(initialize(candidateScene))
    {
        // All selection/result payloads were prepared before the Renderer transition.
        // Publication preserves the initialized Scene address and performs no fallible lookup.
        owner->m_runtime.swap(owner->m_objectMutationCandidate);
        owner->m_objectMutationCandidate.reset();
        owner->CancelSavedMaterialRevision();

        m_renderedSceneStructureRevision = owner->GetScene().GetStructureRevision();
        m_sceneResourcesReleasedForSwitch = false;
        m_editor.PublishRestoredObjectSelection(*owner, std::move(selection), std::move(activeName));

        if(!request.historyReplay)
        {
            m_editor.InvalidateSceneHistoryRedoAfterEdit();
        }

        result.restoredObject = std::move(restored);
        result.publishedIdentity = std::move(publishedIdentity);
        result.succeeded = true;
        result.error.clear();

        Log::Info("Restored scene object " + result.objectID + "; save the scene to persist it.");
        ContinueAutomaticMeshResidencyAfterReplacement(previousRuntimeIdentity);
        return true;
    }

    if(!m_renderer.CleanupFailedSceneInitialization(candidateScene))
    {
        result.error = "Object restoration cleanup could not prove GPU safety; retaining both runtimes and stopping.";
        Log::Error(result.error);
        return false;
    }

    owner->m_objectMutationCandidate.reset();

    if(!initialize(owner->GetScene()))
    {
        result.error = "Object restoration renderer rollback failed; retaining ownership and stopping.";
        Log::Error(result.error);
        return false;
    }

    m_sceneResourcesReleasedForSwitch = false;
    result.error = "Restored candidate renderer initialization failed; previous scene restored.";
    Log::Warning(result.error);
    ContinueAutomaticMeshResidencyAfterReplacement(previousRuntimeIdentity);
    return true;
}

bool Engine::PrepareMeshResidencyAcceptance(const MeshResidencyToken& token, std::string& error)
{
    auto* scene = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());

    if (!scene || m_demoSceneManager.HasPendingSceneChange() || m_pendingEditorSceneOperation ||
        m_pendingMaterialAssignment)
    {
        error = "Mesh acceptance preparation requires an active file scene without a pending scene/material operation.";
        return false;
    }

    if (!m_meshResidencyPublication.Prepare(m_renderer, *scene->m_runtime, scene->m_assets, token, error))
    {
        return false;
    }

    m_meshResidencyAcceptance.reset();
    return true;
}

void Engine::CancelMeshResidencyAcceptance() noexcept
{
    FinishAutomaticMeshResidency(MeshResidencyPublication::Status::Stale);
    m_residencyRequestSelection.reset();
    m_residencyRequestSourceIdentity.clear();
    if (m_diagnosticCPUPreparation)
    {
        m_meshPreparation.Cancel(m_diagnosticCPUPreparation->ticket);
        m_diagnosticCPUPreparation.reset();
    }

    m_meshResidencyPublication.Cancel();
    m_meshResidencyAcceptance.reset();

    auto* scene = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());

    if (m_diagnosticMeshToken && scene && scene->GetRuntimeIdentity() == m_diagnosticMeshToken->runtimeIdentity)
    {
        scene->m_runtime->CancelMeshResidency();
    }

    m_diagnosticMeshToken.reset();
    m_diagnosticMeshReadiness.reset();
    m_diagnosticRendererLifetime = 0;
}

void Engine::RequestDiagnosticMeshResidency()
{
    auto* scene = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    std::string error;

    if (!scene || !scene->IsMetadataOnlyActivation())
    {
        Log::Warning("Mesh residency diagnostic blocked: active scene is not a metadata-only file scene.");
        return;
    }

    if (m_diagnosticCPUPreparation || m_diagnosticMeshToken || m_meshResidencyPublication.HasCandidate())
    {
        if (m_residencyRequestAutomatic && m_residencyRequestSelection &&
            m_residencyRequestSelection->runtimeIndex == m_editor.GetSelectedObject())
        {
            Log::Info("F10: selected object is already pending automatic residency; reusing the existing request.");
        }
        else
        {
            Log::Warning("F10 busy/retry: another manual or automatic residency request owns the single publication slot.");
        }
        return;
    }

    if (m_demoSceneManager.HasPendingSceneChange() || m_pendingEditorSceneOperation || m_pendingMaterialAssignment ||
        m_editor.HasBlockingMaterialOperation() || m_editor.HasBlockingObjectCreation() ||
        m_editor.HasPendingSceneHistoryOperation())
    {
        Log::Warning("Mesh residency diagnostic blocked: a scene, material, or editor operation is pending.");
        return;
    }

    if (!MeshResidencyPublication::CanPrepareGeometry(m_renderer, *scene->m_runtime))
    {
        Log::Warning("Mesh residency diagnostic blocked: renderer scene/session or recording state is not ready.");
        return;
    }

    try
    {
        const auto selectedIndex = m_editor.GetSelectedObject();
        const auto& objects = scene->GetScene().GetRenderObjects();

        if (selectedIndex >= objects.size())
        {
            Log::Warning("Mesh residency diagnostic rejected: no selected hierarchy object.");
            return;
        }

        if (m_editor.GetSelectionStructureRevision() != scene->GetScene().GetStructureRevision())
        {
            Log::Warning("Mesh residency diagnostic rejected: hierarchy selection belongs to an obsolete scene revision.");
            return;
        }

        const auto selection = scene->GetObjectMetadata(selectedIndex, &error);

        if (!selection || selection->runtimeIdentity != scene->GetRuntimeIdentity())
        {
            Log::Warning("Mesh residency diagnostic selection validation failed: " + error);
            return;
        }

        const auto& object = objects[selection->runtimeIndex];
        const auto residency = selection->GetMeshResidency(object);

        if (residency == SceneMeshResidency::Empty)
        {
            Log::Warning("Mesh residency diagnostic rejected: selected object has no authored mesh.");
            return;
        }

        if (residency == SceneMeshResidency::Resident)
        {
            Log::Warning("Mesh residency diagnostic rejected: selected object is already resident.");
            return;
        }

        if (residency != SceneMeshResidency::Pending)
        {
            Log::Warning("Mesh residency diagnostic rejected: selected object's authored/runtime bindings are inconsistent.");
            return;
        }

        BeginMeshResidencyRequest(*selection, false);
    }
    catch (const std::exception& exception)
    {
        CancelMeshResidencyAcceptance();
        Log::Warning("Mesh residency diagnostic preparation failed: " + std::string(exception.what()));
    }
}

void Engine::ProcessMeshResidencyDiagnostics()
{
#ifdef _DEBUG
    // Deliberate developer chords, ignored while typing into an editor field.
    if (!ImGui::GetIO().WantTextInput && m_input.IsKeyDown(VK_CONTROL) && m_input.IsKeyDown(VK_SHIFT))
    {
        if (m_input.WasKeyPressed(VK_F11))
        {
            if (m_residencyRequestAutomatic)
            {
                Log::Info("F11: automatic mesh residency remains active; no manual request to cancel.");
            }
            else
            {
                Log::Info("Mesh residency manual diagnostic cancellation requested.");
                CancelMeshResidencyAcceptance();
            }
        }
        else if (m_input.WasKeyPressed(VK_F9) || m_input.WasKeyPressed(VK_F8))
        {
            const bool synchronous = m_input.WasKeyPressed(VK_F8);
            auto* scene = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
            bool dirty = true;
            std::string error;

            if (!scene || m_demoSceneManager.HasPendingSceneChange() || m_pendingEditorSceneOperation ||
                m_pendingMaterialAssignment || m_editor.HasBlockingMaterialOperation() ||
                m_editor.HasBlockingObjectCreation() || m_editor.HasPendingSceneHistoryOperation() ||
                !scene->CheckUnsavedChanges(dirty, &error) || dirty)
            {
                Log::Warning("Diagnostic scene reload requires a saved file scene and no pending edits: " + error);
            }
            else
            {
                const auto path = scene->GetScenePath();
                CancelMeshResidencyAcceptance();

                const bool requested = synchronous ?
                    m_demoSceneManager.RequestSynchronousFileSceneDiagnostic(path, &error) :
                    m_demoSceneManager.RequestMetadataOnlyFileScene(path, &error);

                if (!requested)
                {
                    Log::Warning("Diagnostic scene reload rejected: " + error);
                }
                else
                {
                    Log::Info(std::string(synchronous ? "Synchronous full-residency Debug reload requested: " :
                        "Metadata-first diagnostic reload requested with automatic residency: ") + path.string());
                }
            }
        }
        else if (m_input.WasKeyPressed(VK_F10))
        {
            Log::Info("Mesh residency diagnostic: Ctrl+Shift+F10 detected.");
            RequestDiagnosticMeshResidency();
        }
    }
#endif

    AdvanceAutomaticMeshResidency();
    AdvanceDiagnosticMeshPreparation();

    if (!m_diagnosticMeshToken || m_meshResidencyPublication.HasCandidate())
    {
        return;
    }

    auto* scene = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());

    if (!scene || scene->GetRuntimeIdentity() != m_diagnosticMeshToken->runtimeIdentity ||
        m_renderer.GetMaterialTableIdentity() != m_diagnosticRendererLifetime || m_demoSceneManager.HasPendingSceneChange())
    {
        Log::Warning("Mesh residency request canceled: scene session or renderer lifetime changed.");
        CancelMeshResidencyAcceptance();
        return;
    }

    std::string error;

    if (PrepareMeshResidencyAcceptance(*m_diagnosticMeshToken, error))
    {
        Log::Info("Mesh residency: renderer membership candidate created; awaiting final publication boundary.");
        return;
    }

    if (error.starts_with("Deferred:"))
    {
        LogMeshResidencyDiagnostic({MeshResidencyPublication::Status::Retry, std::move(error)});
        return;
    }

    RejectMeshResidencyRequest("Mesh residency renderer preparation rejected: " + error);
}

void Engine::LogMeshResidencyDiagnostic(const MeshResidencyPublication::AcceptanceResult& result)
{
    if (m_diagnosticMeshReadiness && m_diagnosticMeshReadiness->status == result.status &&
        m_diagnosticMeshReadiness->message == result.message && m_diagnosticMeshReadiness->published == result.published)
    {
        return;
    }

    m_diagnosticMeshReadiness = result;
    const char* status = "Failed";

    switch (result.status)
    {
    case MeshResidencyPublication::Status::Ready: status = "Ready"; break;
    case MeshResidencyPublication::Status::Retry: status = "Retry"; break;
    case MeshResidencyPublication::Status::Stale: status = "Stale"; break;
    case MeshResidencyPublication::Status::Unsupported: status = "Unsupported"; break;
    case MeshResidencyPublication::Status::Failed: break;
    }

    if (result.published)
    {
        Log::Info("Mesh residency: residency published successfully.");
        return;
    }

    Log::Info("Mesh residency readiness " + std::string(status) + ": " + result.message);
}

void Engine::AdvanceDiagnosticMeshPreparation()
{
    if (!m_diagnosticCPUPreparation)
    {
        return;
    }

    try
    {
        auto* scene = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
        const auto& request = *m_diagnosticCPUPreparation;
        const auto& saved = request.selection;
        std::string error;

        if (!scene || scene->GetRuntimeIdentity() != saved.runtimeIdentity ||
            m_renderer.GetMaterialTableIdentity() != m_diagnosticRendererLifetime ||
            m_demoSceneManager.HasPendingSceneChange() || request.generation != m_meshPreparation.Generation())
        {
            RejectMeshResidencyRequest("Asynchronous mesh preparation canceled: scene/subscriber or renderer lifetime is stale.",
                MeshResidencyPublication::Status::Stale);
            return;
        }

        const auto current = scene->GetObjectMetadata(saved.objectID, &error);

        if (!current || current->runtimeIdentity != saved.runtimeIdentity || current->sceneID != saved.sceneID ||
            current->runtimeIndex != saved.runtimeIndex || current->structureRevision != saved.structureRevision ||
            current->meshReference != saved.meshReference || current->bindingRevision != saved.bindingRevision ||
            current->materialReference != saved.materialReference || current->materialSlotBindings != saved.materialSlotBindings ||
            current->GetMeshResidency(scene->GetScene().GetRenderObjects()[current->runtimeIndex]) != SceneMeshResidency::Pending)
        {
            RejectMeshResidencyRequest("Asynchronous mesh preparation canceled: object identity or authored bindings changed. " + error,
                MeshResidencyPublication::Status::Stale);
            return;
        }

        if (!request.completion)
        {
            auto completion = m_meshPreparation.Poll(request.ticket, ProjectPaths(scene->GetProjectRoot()),
                m_projectAssetRegistry, m_meshCPUCache);

            if (completion.status == ProjectMeshPreparationService::Status::Pending ||
                completion.status == ProjectMeshPreparationService::Status::Retry)
            {
                if (!completion.error.empty())
                {
                    LogMeshResidencyDiagnostic({MeshResidencyPublication::Status::Retry, completion.error});
                }

                return;
            }

            if (completion.status != ProjectMeshPreparationService::Status::Available ||
                !m_meshPreparation.IsCurrent(completion))
            {
                const char* outcome = completion.status == ProjectMeshPreparationService::Status::Canceled ? "canceled" :
                    completion.status == ProjectMeshPreparationService::Status::Stale || !m_meshPreparation.IsCurrent(completion) ?
                        "stale" : "failed";
                RejectMeshResidencyRequest("Asynchronous mesh preparation " + std::string(outcome) + ": " + completion.error,
                    completion.status == ProjectMeshPreparationService::Status::Stale ||
                    completion.status == ProjectMeshPreparationService::Status::Canceled ?
                        MeshResidencyPublication::Status::Stale : MeshResidencyPublication::Status::Failed);
                return;
            }

            Log::Info(completion.cacheHit ? "Asynchronous mesh preparation: verified CPU cache hit." :
                "Asynchronous mesh preparation: background FBX import completed.");
            m_diagnosticCPUPreparation->completion.emplace(std::move(completion));
        }

        // Keep one immutable completion while the renderer boundary is busy;
        // never poll an already-delivered ticket or upload the same result twice.
        if (!MeshResidencyPublication::CanPrepareGeometry(m_renderer, *scene->m_runtime) ||
            m_pendingMaterialAssignment || m_pendingEditorSceneOperation)
        {
            LogMeshResidencyDiagnostic({MeshResidencyPublication::Status::Retry,
                "Prepared CPU geometry awaits a safe main-thread GPU preparation boundary."});
            return;
        }

        auto& completion = *m_diagnosticCPUPreparation->completion;

        if (!m_meshPreparation.IsCurrent(completion) ||
            completion.identity.assetID != AssetID::ParseReference(saved.meshReference)->ToString())
        {
            RejectMeshResidencyRequest("Asynchronous mesh preparation completion became stale before consumption.",
                MeshResidencyPublication::Status::Stale);
            return;
        }

        if (!m_meshPreparation.IntegrateCache(completion, m_meshCPUCache, error))
        {
            RejectMeshResidencyRequest("Asynchronous mesh preparation cache integration failed: " + error);
            return;
        }

        Log::Info("Active scene bindings match CPU completion; starting synchronous source validation/GPU geometry preparation.");
        const auto uploadStart = std::chrono::steady_clock::now();
        Mesh* mesh = scene->m_assets.ResolvePreparedMesh(saved.meshReference, completion, &error);

        if (!mesh)
        {
            RejectMeshResidencyRequest("Prepared geometry source validation/GPU preparation failed: " + error);
            return;
        }

        const auto milliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - uploadStart).count();
        Log::Info("Synchronous GPU geometry preparation/source validation completed in " +
            std::to_string(milliseconds) + " ms.");
        const auto candidate = scene->m_assets.DescribeMeshResidencyCandidate(saved.meshReference, *mesh, &error);

        if (!candidate)
        {
            RejectMeshResidencyRequest("Asynchronous residency geometry validation failed: " + error);
            return;
        }

        m_diagnosticMeshToken = scene->m_runtime->PrepareMeshResidency(*current, scene->m_assets, *candidate, &error);

        if (!m_diagnosticMeshToken)
        {
            RejectMeshResidencyRequest("Asynchronous residency scene preparation rejected: " + error);
            return;
        }

        // Catalog/cache now retain geometry. CPU subscription no longer owns
        // publication authority; the existing scene token/coordinator takes over.
        m_meshPreparation.Cancel(request.ticket);
        m_diagnosticCPUPreparation.reset();
        m_diagnosticMeshReadiness.reset();
        Log::Info("Prepared geometry accepted by active scene: residency token ready for existing renderer coordinator.");
    }
    catch (const std::exception& exception)
    {
        RejectMeshResidencyRequest("Asynchronous residency completion processing failed: " + std::string(exception.what()),
            MeshResidencyPublication::Status::Retry);
    }
}

bool Engine::BeginMeshResidencyRequest(const SceneObjectMetadata& selection, bool automatic)
{
    auto* scene = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());

    if (!scene || m_diagnosticCPUPreparation || m_diagnosticMeshToken || m_meshResidencyPublication.HasCandidate())
    {
        return false;
    }

    try
    {
        m_residencyRequestSelection = selection;
        m_residencyRequestAutomatic = automatic;
        m_residencyRequestSourceIdentity = GetMeshSourceIdentity(selection);

        const auto kind = FileSceneAssetCatalog::ClassifyMeshReference(selection.meshReference);

        if (kind == FileSceneAssetCatalog::MeshReferenceKind::UnknownBuiltin ||
            kind == FileSceneAssetCatalog::MeshReferenceKind::Invalid)
        {
            RejectMeshResidencyRequest((kind == FileSceneAssetCatalog::MeshReferenceKind::UnknownBuiltin ?
                "Unsupported primitive reference: " : "Invalid authored mesh reference: ") + selection.meshReference,
                MeshResidencyPublication::Status::Unsupported);
            return false;
        }

        if (kind == FileSceneAssetCatalog::MeshReferenceKind::Builtin)
        {
            if (!MeshResidencyPublication::CanPrepareGeometry(m_renderer, *scene->m_runtime))
            {
                RejectMeshResidencyRequest("Built-in geometry preparation requires an idle renderer boundary.",
                    MeshResidencyPublication::Status::Retry);
                return false;
            }

            Log::Info("Built-in primitive residency requested: " + selection.meshReference);
            Mesh* mesh = scene->m_assets.ResolveMesh(selection.meshReference);
            std::string error;
            const auto candidate = mesh ? scene->m_assets.DescribeMeshResidencyCandidate(
                selection.meshReference, *mesh, &error) : std::nullopt;

            if (!candidate)
            {
                RejectMeshResidencyRequest("Built-in geometry validation failed: " + error);
                return false;
            }

            Log::Info("Built-in geometry prepared: existing catalog primitive GPU geometry reused.");
            m_diagnosticMeshToken = scene->m_runtime->PrepareMeshResidency(selection, scene->m_assets, *candidate, &error);

            if (!m_diagnosticMeshToken)
            {
                RejectMeshResidencyRequest("Built-in scene residency preparation failed: " + error);
                return false;
            }

            m_diagnosticRendererLifetime = m_renderer.GetMaterialTableIdentity();
            m_diagnosticMeshReadiness.reset();
            Log::Info("Built-in residency token accepted; handing off to existing renderer coordinator.");
            return true;
        }

        const auto asset = AssetID::ParseReference(selection.meshReference);
        m_diagnosticCPUPreparation.emplace();
        m_diagnosticCPUPreparation->selection = selection;
        m_diagnosticCPUPreparation->generation = m_meshPreparation.Generation();
        m_diagnosticRendererLifetime = m_renderer.GetMaterialTableIdentity();
        m_diagnosticMeshReadiness.reset();

        const auto request = m_meshPreparation.Request(ProjectPaths(scene->GetProjectRoot()), m_projectAssetRegistry,
            *asset, m_diagnosticCPUPreparation->generation);
        m_diagnosticCPUPreparation->ticket = request.ticket;

        if (request.status != ProjectMeshPreparationService::Status::Accepted &&
            request.status != ProjectMeshPreparationService::Status::Available)
        {
            const auto status = request.status == ProjectMeshPreparationService::Status::Retry ||
                request.error.find("Cannot pin") != std::string::npos ? MeshResidencyPublication::Status::Retry :
                    MeshResidencyPublication::Status::Unsupported;
            RejectMeshResidencyRequest("Asynchronous mesh preparation request rejected: " + request.error, status);
            return false;
        }

        Log::Info("Asynchronous mesh preparation requested: object " + selection.objectID + ", mesh " +
            selection.meshReference + "; worker source validation/cache lookup scheduled.");
        return true;
    }
    catch (const std::exception& exception)
    {
        RejectMeshResidencyRequest("Mesh residency request allocation/preparation failed: " + std::string(exception.what()),
            MeshResidencyPublication::Status::Retry);
        return false;
    }
}

std::string Engine::GetMeshSourceIdentity(const SceneObjectMetadata& selection) const
{
    const auto* scene = dynamic_cast<const FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());

    if (!scene)
    {
        return {};
    }

    try
    {
        const auto asset = AssetID::ParseReference(selection.meshReference);

        if (!asset)
        {
            return {};
        }

        const auto resolved = ProjectAssetResolver::Resolve(ProjectPaths(scene->GetProjectRoot()), m_projectAssetRegistry, *asset);

        if (resolved.status != AssetAvailability::Available)
        {
            return {};
        }

        return ProjectMeshPreparationService::InspectSourceIdentity(resolved.resolvedPath);
    }
    catch (const std::exception&)
    {
        return {};
    }
}

void Engine::OnFileSceneActivated(std::chrono::steady_clock::time_point started)
{
    auto* scene = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());

    if (!scene)
    {
        return;
    }

    if (m_automaticResidencyIntent)
    {
        Log::Info("Automatic mesh residency session " + std::to_string(m_automaticResidencyIntent) +
            " superseded by scene activation; old requests canceled, session counters discarded.");
    }

    m_automaticMeshResidency = {};
    m_automaticResidencyIntent = 0;
    scene->m_assets.BeginTextureStreaming();

    size_t authored = 0;

    for (const auto& object : scene->m_runtime->GetFileScene().GetDocument().objects)
    {
        if (!object.meshReference.empty())
        {
            ++authored;
        }
    }

    const auto& objects = scene->GetScene().GetRenderObjects();
    const auto resident = static_cast<size_t>(std::count_if(objects.begin(), objects.end(), [](const auto& object)
    {
        return object.mesh != nullptr;
    }));

    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    Log::Info(std::string("File scene activation: mode=") +
        (scene->IsMetadataOnlyActivation() ? "metadata-first" : "synchronous-full Debug diagnostic") +
        ", session=" + std::to_string(scene->GetRuntimeIdentity()) + ", activation elapsed=" +
        std::to_string(elapsed) + " ms, authored mesh objects=" + std::to_string(authored) +
        ", initially resident=" + std::to_string(resident) + ", pending=" + std::to_string(authored - resident) +
        "; mesh and texture settlement reported separately.");

    if (scene->IsMetadataOnlyActivation())
    {
        StartAutomaticMeshResidency();
    }
}

void Engine::StartAutomaticMeshResidency()
{
    const auto* scene = dynamic_cast<const FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());

    if (!scene || !scene->IsMetadataOnlyActivation())
    {
        return;
    }

    m_automaticMeshResidency = {};
    auto& scheduler = m_automaticMeshResidency;
    scheduler.enabled = true;
    scheduler.runtimeIdentity = scene->GetRuntimeIdentity();
    scheduler.structureRevision = scene->GetScene().GetStructureRevision();

    scheduler.activated = std::chrono::steady_clock::now();
    scheduler.retryAfter = scheduler.activated;
    const auto& objects = scene->m_runtime->GetFileScene().GetDocument().objects;
    const auto& mapping = scene->m_runtime->GetObjectMapping();
    const auto& runtime = scene->GetScene().GetRenderObjects();

    for (const auto& object : objects)
    {
        if (object.meshReference.empty())
        {
            continue;
        }

        const auto found = mapping.idToRuntimeIndex.find(object.id);

        if (found == mapping.idToRuntimeIndex.end() || found->second >= runtime.size())
        {
            scheduler.enabled = false;
            Log::Warning("Automatic mesh residency discovery rejected an inconsistent committed object mapping.");
            return;
        }

        if (runtime[found->second].mesh)
        {
            ++scheduler.initiallyResident;
        }
        else
        {
            ++scheduler.eligible;
        }
    }

    // Set intent only for this explicitly enabled and committed session.
    m_automaticResidencyIntent = scheduler.runtimeIdentity;
    Log::Info("Automatic mesh residency enabled: session=" + std::to_string(scheduler.runtimeIdentity) +
        ", eligible pending=" + std::to_string(scheduler.eligible) + ", already resident=" +
        std::to_string(scheduler.initiallyResident) + "; one outstanding CPU/publication request.");
}

void Engine::ContinueAutomaticMeshResidencyAfterReplacement(std::uint64_t previousRuntimeIdentity) noexcept
{
    if (!previousRuntimeIdentity || m_automaticResidencyIntent != previousRuntimeIdentity)
    {
        return; // A synchronous/manual scene never gains automatic intent from pending objects.
    }

    auto* scene = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());

    if (!scene || m_state != EngineState::Running || m_demoSceneManager.HasPendingSceneChange() ||
        !scene->IsMetadataOnlyActivation() || !m_renderer.CanEditPendingMaterialBindings(scene->GetScene()))
    {
        return; // Never schedule against an uncommitted/failed renderer or during shutdown.
    }

    try
    {
        if (scene->GetRuntimeIdentity() == previousRuntimeIdentity)
        {
            Log::Info("Automatic mesh residency: editor replacement failed; old session " +
                std::to_string(previousRuntimeIdentity) + " restored with fresh-request retry state retained.");
            return;
        }

        Log::Info("Automatic mesh residency session " + std::to_string(previousRuntimeIdentity) +
            " superseded by editor replacement; old requests canceled, counters/failure generations discarded.");
        StartAutomaticMeshResidency();
        // Rebuild bounded failure state rather than reconciling old epochs. A
        // conservative cooldown prevents immediate repeated imports after edits.
        m_automaticMeshResidency.retryAfter = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    }
    catch (const std::exception&)
    {
        // Scheduler bookkeeping must not turn a committed editor transaction
        // into a reported scene-replacement failure. The active scene stays valid.
        m_automaticMeshResidency.enabled = false;
        m_automaticResidencyIntent = scene->GetRuntimeIdentity();
    }
}

void Engine::FinishAutomaticMeshResidency(MeshResidencyPublication::Status status)
{
    auto& scheduler = m_automaticMeshResidency;

    if (status == MeshResidencyPublication::Status::Ready && scheduler.enabled && m_residencyRequestSelection &&
        m_residencyRequestSelection->runtimeIdentity == scheduler.runtimeIdentity)
    {
        ++scheduler.published;
        std::erase_if(scheduler.failures, [&](const auto& failure)
        {
            return failure.selection.objectID == m_residencyRequestSelection->objectID;
        });
        scheduler.backpressure.clear();
        m_residencyRequestAutomatic = false;
        return;
    }

    if (!m_residencyRequestAutomatic)
    {
        return;
    }

    m_residencyRequestAutomatic = false;

    if (status == MeshResidencyPublication::Status::Ready)
    {
        return;
    }

    if (status == MeshResidencyPublication::Status::Failed || status == MeshResidencyPublication::Status::Unsupported)
    {
        if (m_residencyRequestSelection)
        {
            try
            {
                std::erase_if(scheduler.failures, [&](const auto& failure)
                {
                    return failure.selection.objectID == m_residencyRequestSelection->objectID;
                });

                if (scheduler.failures.size() < 4096)
                {
                    scheduler.failures.push_back({*m_residencyRequestSelection, m_residencyRequestSourceIdentity});
                }
                else
                {
                    scheduler.enabled = false;
                }
            }
            catch (const std::exception&)
            {
                scheduler.enabled = false; // Cannot track failures safely; never spin/reimport.
            }
        }

        return;
    }

    if (status == MeshResidencyPublication::Status::Stale)
    {
        ++scheduler.stale;
    }

    if (m_residencyRequestSelection)
    {
        scheduler.cursor = (std::min)(scheduler.cursor, m_residencyRequestSelection->runtimeIndex);
    }

    scheduler.retryAfter = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
}

void Engine::RejectMeshResidencyRequest(const std::string& message, MeshResidencyPublication::Status status)
{
    // Existing low-level APIs report certain temporary failures through strings.
    // Keep these retryable rather than poisoning this object's binding generation.
    if (message.find("Cannot pin") != std::string::npos || message.find("Cannot lock") != std::string::npos ||
        message.find("cannot lock") != std::string::npos || message.find("allocation") != std::string::npos ||
        message.find("capacity") != std::string::npos || message.find("Deferred:") != std::string::npos ||
        message.find("outstanding candidate") != std::string::npos)
    {
        status = MeshResidencyPublication::Status::Retry;
    }

    auto& scheduler = m_automaticMeshResidency;

    if (!m_residencyRequestAutomatic || status != MeshResidencyPublication::Status::Retry || scheduler.backpressure != message)
    {
        Log::Warning((status == MeshResidencyPublication::Status::Retry ? "Mesh residency retryable backpressure: " :
            status == MeshResidencyPublication::Status::Stale ? "Mesh residency stale invalidation: " :
                "Mesh residency terminal failure: ") + message);
    }

    if (m_residencyRequestAutomatic && status == MeshResidencyPublication::Status::Retry)
    {
        scheduler.backpressure = message;
    }

    FinishAutomaticMeshResidency(status);
    CancelMeshResidencyAcceptance();
}

void Engine::AdvanceAutomaticMeshResidency()
{
    auto& scheduler = m_automaticMeshResidency;

    if (!scheduler.enabled)
    {
        return;
    }

    auto* scene = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());

    if (!scene || !scene->IsMetadataOnlyActivation() || scene->GetRuntimeIdentity() != scheduler.runtimeIdentity)
    {
        scheduler = {};
        return;
    }

    if (m_diagnosticCPUPreparation || m_diagnosticMeshToken || m_meshResidencyPublication.HasCandidate() ||
        m_pendingEditorSceneOperation || m_pendingMaterialAssignment || m_demoSceneManager.HasPendingSceneChange() ||
        !MeshResidencyPublication::CanPrepareGeometry(m_renderer, *scene->m_runtime))
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    if (now < scheduler.retryAfter)
    {
        return;
    }

    const auto& objects = scene->GetScene().GetRenderObjects();
    const auto revision = scene->GetScene().GetStructureRevision();

    if (revision != scheduler.structureRevision)
    {
        scheduler.cursor = 0;
        scheduler.settled = false;
        scheduler.structureRevision = revision;
        scheduler.failures.clear();
        const auto& authored = scene->m_runtime->GetFileScene().GetDocument().objects;
        scheduler.eligible = std::count_if(authored.begin(), authored.end(), [](const auto& object)
        {
            return !object.meshReference.empty();
        });
    }

    if (objects.size() > 4096)
    {
        Log::Warning("Automatic mesh residency stopped: scene exceeds the 4096-object candidate/scheduler bound.");
        scheduler.enabled = false;
        return;
    }

    // Scan cheap resident indices in bounded batches. At most one validated
    // pending-object lookup/submission per frame; no repeated all-scene snapshot.
    for (size_t scanned = 0; scanned < 32 && scheduler.cursor < objects.size(); ++scanned)
    {
        const auto index = scheduler.cursor++;

        if (objects[index].mesh)
        {
            continue;
        }

        std::string error;
        const auto selection = scene->GetObjectMetadata(index, &error);

        if (!selection)
        {
            const auto message = "Automatic discovery metadata is unavailable: " + error;

            if (scheduler.backpressure != message)
            {
                Log::Warning(message);
                scheduler.backpressure = message;
            }

            scheduler.cursor = index;
            scheduler.retryAfter = now + std::chrono::seconds(1);
            return;
        }

        if (selection->meshReference.empty())
        {
            return; // Bound metadata validation to one object per frame.
        }

        const auto failure = std::find_if(scheduler.failures.begin(), scheduler.failures.end(), [&](const auto& saved)
        {
            const auto& binding = saved.selection;
            return binding.objectID == selection->objectID && binding.runtimeIndex == selection->runtimeIndex &&
                binding.bindingRevision == selection->bindingRevision && binding.meshReference == selection->meshReference &&
                binding.materialReference == selection->materialReference && binding.materialSlotBindings == selection->materialSlotBindings;
        });

        if (failure != scheduler.failures.end() && failure->sourceIdentity == GetMeshSourceIdentity(*selection))
        {
            return; // Terminal for this source/binding; next frame advances the cursor.
        }

        std::erase_if(scheduler.failures, [&](const auto& saved)
        {
            return saved.selection.objectID == selection->objectID;
        });
        scheduler.settled = false;
        BeginMeshResidencyRequest(*selection, true);
        return;
    }

    if (scheduler.cursor < objects.size())
    {
        return;
    }

    if (!scheduler.settled)
    {
        const auto elapsed = std::chrono::duration<double, std::milli>(now - scheduler.activated).count();
        Log::Info("Automatic mesh residency settled: session=" + std::to_string(scheduler.runtimeIdentity) +
            ", eligible=" + std::to_string(scheduler.eligible) +
            ", published=" + std::to_string(scheduler.published) + ", already-resident=" + std::to_string(scheduler.initiallyResident) +
            ", terminal failures=" + std::to_string(scheduler.failures.size()) + ", canceled/stale=" +
            std::to_string(scheduler.stale) + ", outstanding=0, elapsed=" + std::to_string(elapsed) + " ms.");
        scheduler.settled = true;
    }

    // Low-frequency discovery resumes to notice binding/source changes after
    // settlement. No import retry occurs for an unchanged recorded failure.
    scheduler.cursor = 0;
    scheduler.retryAfter = now + std::chrono::seconds(1);
}
