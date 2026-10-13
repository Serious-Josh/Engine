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
    error.clear();

    if(m_state != EngineState::Running || m_pendingMaterialAssignment || m_pendingEditorSceneOperation ||
       m_demoSceneManager.HasPendingSceneChange() || m_editor.HasBlockingMaterialOperation())
    {
        error = "Material assignment rejected: Engine is unavailable or an editor/scene operation is pending.";
        return false;
    }

    const auto* scene = dynamic_cast<const FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    const auto current = scene ? scene->GetObjectMetadata(selection.runtimeIndex, &error) : std::nullopt;

    if(!current || current->sceneID != selection.sceneID || current->objectID != selection.objectID ||
       current->runtimeIdentity != selection.runtimeIdentity || current->structureRevision != selection.structureRevision)
    {
        error = "Material assignment requires a current file-backed scene-object selection.";
        return false;
    }

    m_pendingMaterialAssignment = MaterialAssignmentRequest{selection, std::move(reference)};
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

    std::string error;
    feedback.message = "Selection or assignment validation failed. See the log for details.";
    const auto token = owner->PrepareMaterialAssignment(request.selection, request.reference, &error);

    if(!token)
    {
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

    feedback.message = "Saved material could not be loaded. See the log for details.";
    Material* replacement = nullptr;
    std::vector<Material*> materials;

    try
    {
        replacement = owner->ResolveAssignmentMaterial(request.reference, &error);

        if(replacement)
        {
            // Allocate the full table snapshot before releasing any Renderer resources.
            materials = owner->GetMaterials();
        }
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
    }

    if(!replacement || materials.empty())
    {
        Log::Warning("Material assignment load failed; previous assignment retained: " + error);
        return cancel();
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
        committed = owner->CommitMaterialAssignment(*token, *replacement, &error);
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
    }

    if(!committed)
    {
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
        if(!owner->FinishMaterialAssignment(*token, &error))
        {
            Log::Error("Material assignment finalization failed; retaining ownership and stopping: " + error);
            return false;
        }

        m_sceneResourcesReleasedForSwitch = false;
        m_renderedSceneStructureRevision = owner->GetScene().GetStructureRevision();
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

    if(!owner->RollbackMaterialAssignment(*token, &error))
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
    IDemoScene* previous = m_demoSceneManager.TryGetActiveScene();

    if (!previous)
    {
        Log::Error("File scene activation requires an active scene owner.");
        return false;
    }

    std::string error;

    if (!m_demoSceneManager.GetStagedFileScene() &&
        !m_demoSceneManager.StageRequestedFileScene(
            m_renderer.GetDevice(), m_renderer.GetResourceUploader(), m_renderer.GetResourceDescriptorHeap(),
            m_demoVertexShader, m_demoPixelShader, &error))
    {
        Log::Error("File scene staging failed; keeping the current scene: " + error);
        m_demoSceneManager.CancelFileSceneRequest();
        return true;
    }

    IDemoScene* candidate = m_demoSceneManager.GetStagedFileScene();

    if (!candidate)
    {
        Log::Error("File scene staging did not produce a candidate.");
        return false;
    }

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

    if (m_renderer.InitializeSceneResources(candidate->GetScene(), candidate->GetMaterials()))
    {
        if (!m_demoSceneManager.CommitStagedFileScene(&error))
        {
            Log::Error("File scene commit failed; retaining candidate Renderer ownership: " + error);
            return false;
        }

        m_sceneResourcesReleasedForSwitch = false;
        m_editor.OnSceneActivated();
        m_renderedSceneStructureRevision = m_demoSceneManager.GetActiveScene().GetScene().GetStructureRevision();
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
        if (m_pendingEditorSceneOperation || m_pendingMaterialAssignment || m_demoSceneManager.HasPendingSceneChange())
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

    m_editor.ConfigureThumbnails(m_renderer.GetDevice(), m_renderer.GetResourceUploader(),
        m_renderer.GetResourceDescriptorHeap());
    m_editor.ConfigureMaterialThumbnails(m_renderer);

    m_demoSceneManager.SetProjectAssets(ProjectLocator::GetDefaultProjectRoot(), m_projectAssetRegistry);

    if(!m_demoSceneManager.Initialize(
            "Sphere Grid", m_renderer.GetDevice(), m_renderer.GetResourceUploader(),
            m_renderer.GetResourceDescriptorHeap(), m_demoVertexShader, m_demoPixelShader,
            aspectRatio))
    {
        Log::Error("Failed to initialize demo scene manager.");
        return false;
    }

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
        materialAssignmentSafe = ExecuteMaterialAssignment();
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

            m_renderedSceneStructureRevision = switchedDemo.GetScene().GetStructureRevision();
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
        if(!m_renderer.WaitForGPU())
        {
            Log::Error("Thumbnail retirement GPU drain failed; retaining resources and stopping.");
            m_window.Close();
            return;
        }

        m_editor.ClearThumbnailsAfterGPUDrain();
    }

    ProcessWorkingMaterialNotifications();
    ProcessSavedMaterialRevisions();
    PublishWorkingMaterialParameters();

    m_editor.SetThumbnailCompletedFence(m_renderer.GetEditorCompletedFence());

    debugUI.BeginFrame();

    IDemoScene& activeDemo = m_demoSceneManager.GetActiveScene();

    m_editor.SetSceneOperationPending(m_pendingMaterialAssignment.has_value() || m_pendingEditorSceneOperation.has_value() ||
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

    if(auto request = m_editor.TakeMaterialAssignmentRequest())
    {
        std::string error;

        if(!RequestMaterialAssignment(request->selection, std::move(request->reference), error))
        {
            Log::Warning("Inspector material assignment rejected: " + error);
            m_editor.OnMaterialAssignmentCompleted(request->selection, false,
                "Request rejected because the scene or editor is busy or the selection changed. See the log for details.");
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
    rendererOptions.editorTextures = m_editor.GetThumbnailTextures();
    rendererOptions.materialPreview = m_editor.GetMaterialPreviewRequest();
    m_renderer.PrepareMaterialPreviewTextures(rendererOptions.materialPreview, m_projectAssetRegistry);

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

    m_editor.ConfirmThumbnailSubmission(m_renderer.GetEditorSubmissionFence());

    activeScene.ClearChangedTransforms();
}

void Engine::Shutdown()
{
    ENGINE_ASSERT(m_state == EngineState::Running);

    Log::Info("Engine shutdown: starting.");
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

    m_editor.ClearThumbnailsAfterGPUDrain();
    m_demoSceneManager.Shutdown();
    m_renderer.Shutdown();
    m_window.Shutdown();

    Log::Info("Engine shutdown: finished.");
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
