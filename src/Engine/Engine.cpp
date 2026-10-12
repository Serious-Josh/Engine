#include "Engine.h"

#include "Core/Log.h"
#include "Core/Assert.h"
#include "Core/Time.h"
#include "Debug/CpuFrameTiming.h"

#include "Scene/Scene.h"

#include "DemoContent/SceneRegistry.h"
#include "DemoContent/MirrorTestScene.h"
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

bool Engine::Initialize()
{
    ENGINE_ASSERT(m_state == EngineState::Uninitialized);

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

    m_editor.Draw(activeDemo, m_demoSceneManager, static_cast<float>(m_window.GetWidth()),
                  static_cast<float>(m_window.GetHeight()), m_time.GetDeltaTime());

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

    if (!DrainAndResolveSceneDisplacement(m_renderer, m_demoSceneManager.GetActiveScene()))
    {
        Log::Error("Engine shutdown: GPU drain or displacement resolution failed; aborting without teardown.");
        std::abort();
    }

    if (!m_renderer.ReleaseSceneResources())
    {
        Log::Error("Engine shutdown: scene resource release rejected; aborting without teardown.");

        std::abort();
    }

    m_demoSceneManager.Shutdown();

    m_renderer.Shutdown();

    m_window.Shutdown();
    Log::Info("Engine shutdown: finished.");
}
