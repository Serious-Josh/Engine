#include "Engine.h"

#include "Core/Log.h"
#include "Core/Assert.h"
#include "Core/Time.h"
#include "Debug/CpuFrameTiming.h"

#include "Scene/Scene.h"
#include "Scene/SceneFileCreation.h"

#include "Project/ProjectLocator.h"
#include "Project/ProjectPaths.h"

#include "DemoContent/SceneRegistry.h"
#include "DemoContent/MirrorTestScene.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "imgui.h"

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

void Engine::ProcessEditorSceneRequests(IDemoScene& activeScene)
{
    auto* fileScene = dynamic_cast<FileBackedDemoScene*>(&activeScene);

    if (const auto request = m_editor.TakeFileRequest())
    {
        if (m_pendingEditorSceneOperation || m_demoSceneManager.HasPendingSceneChange())
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
    const ProjectPaths projectPaths(
        ProjectLocator::GetDefaultProjectRoot());

    std::string projectError;

    if (!projectPaths.EnsureDirectories(&projectError))
    {
        Log::Error(
            "Failed to initialize default project directories: " +
            projectError);

        return false;
    }

    Log::Info(
        "Default project directory: " +
        projectPaths.GetRoot().string());

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

    if (!m_demoPixelShader)
    {
        Log::Error("Failed to load demo pixel shader.");

        return false;
    }

    const float aspectRatio = static_cast<float>(m_window.GetWidth()) / static_cast<float>(m_window.GetHeight());

    if (!m_demoSceneManager.Initialize("Sphere Grid", m_renderer.GetDevice(), m_renderer.GetResourceUploader(),
                                       m_renderer.GetResourceDescriptorHeap(), m_demoVertexShader, m_demoPixelShader,
                                       aspectRatio))
    {
        Log::Error("Failed to initialize demo scene manager.");
        return false;
    }

    Log::Info("Demo scene initialization complete.");

    IDemoScene& activeDemo = m_demoSceneManager.GetActiveScene();

    if (!m_renderer.InitializeSceneResources(activeDemo.GetScene(), activeDemo.GetMaterials()))
    {
        Log::Error("Failed to initialize demo renderer resources.");
        return false;
    }

    // Initial BLAS/TLAS registration is complete before generation rebuilds it.

    if (auto* mirrorScene = dynamic_cast<MirrorTestScene*>(&activeDemo))
    {
        if (!m_renderer.RegisterDisplacementHeightmap(*mirrorScene) ||

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
            if (m_window.ShouldClose())
            {
                return;
            }

            switch (event.type)
            {
            case EventType::WindowResized:
                m_window.SetSize(event.width, event.height);

                if (event.width > 0 && event.height > 0)
                {
                    m_demoSceneManager.SetAspectRatio(static_cast<float>(event.width) /

                                                      static_cast<float>(event.height));
                }

                if (!m_renderer.Resize(event.width, event.height))
                {
                    Log::Error("Renderer resize failed; requesting application shutdown.");

                    m_window.Close();
                }

                break;
            case EventType::WindowClose:
                m_window.Close();
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

    while (!m_window.ShouldClose())
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

        if (sample.submitted && (!hasPacingFrame || sample.frameSerial != lastPacingFrame))
        {
            sample.frameStart = frameStart;
            sample.frameExecution = frameExecution;

            m_renderer.GetDebugUI().RecordFramePacingCpuTiming(sample);
            lastPacingFrame = sample.frameSerial;
            hasPacingFrame = true;
        }

        // Skipped/minimized frames must not publish the previous Render sample again.

        if (sample.valid && (!hasRecordedFrame || sample.frameSerial != lastRecordedFrame))
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

    if (m_window.ShouldClose())
    {
        return;
    }

    if (m_window.GetWidth() == 0 || m_window.GetHeight() == 0)
    {
        m_window.WaitForMessage();
        // Exclude the suspended interval from the next simulation update.
        m_time.Update();
        return;
    }

    DebugUI& debugUI = m_renderer.GetDebugUI();

    bool displacementCompletionDelivered = false;

    if (Renderer::IsAsyncDisplacementEnabled())
    {
        Renderer::FrameHitchProbe completionProbe(
            m_renderer,
            Renderer::FrameHitchRegion::EngineCompletion);

        // Poll before scene switches/editor mutation and outside recording.
        // Render polls again at admission; a result produced there is preserved
        // until this owner-aware delivery on the next update.
        const auto completion = m_renderer.ProcessDisplacementCompletion();

        if (completion == Renderer::DisplacementCompletionStatus::Error)
        {
            Log::Error("Asynchronous displacement completion failed; requesting shutdown.");
            m_window.Close();
            return;
        }

        if (completion == Renderer::DisplacementCompletionStatus::Finalized)
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

    debugUI.BeginFrame();
    debugUI.DrawPerformanceWindow(environmentNames, m_renderer.GetCurrentEnvironmentIndex(),
                                  m_renderer.GetStatistics());

    IDemoScene& activeDemo = m_demoSceneManager.GetActiveScene();

    m_editor.SetSceneOperationPending(m_pendingEditorSceneOperation.has_value() ||
                                      m_demoSceneManager.HasPendingSceneChange());
    m_editor.Draw(activeDemo, m_demoSceneManager, debugUI, static_cast<float>(m_window.GetWidth()),
                  static_cast<float>(m_window.GetHeight()), m_time.GetDeltaTime());

    ProcessEditorSceneRequests(activeDemo);

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

    activeScene.ClearChangedTransforms();
}


void Engine::Shutdown()
{
    ENGINE_ASSERT(m_state == EngineState::Running);

    Log::Info("Engine shutdown: starting.");

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

    m_demoSceneManager.Shutdown();
    m_renderer.Shutdown();
    m_window.Shutdown();

    Log::Info("Engine shutdown: finished.");
}
