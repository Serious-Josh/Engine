#include "Engine.h"

#include "Core/Log.h"
#include "Core/Assert.h"
#include "Core/Time.h"
#include "Debug/CpuFrameTiming.h"

#include "Scene/Scene.h"

#include "DemoContent/SceneRegistry.h"
#include "DemoContent/MirrorTestScene.h"

#include <cstdlib>
#include <chrono>

bool Engine::Initialize()
{
    ENGINE_ASSERT(m_state == EngineState::Uninitialized);

    if(!m_window.Initialize(m_windowProperties)){
        Log::Error("Failed to initialize window.");
        return false;
    }

    m_time.Initialize();

    const std::filesystem::path executableDirectory = WindowsWindow::GetExecutableDirectory();
    const std::filesystem::path assetRoot = executableDirectory / "assets";

    if(!m_renderer.Initialize(m_window.GetNativeHandle(), m_window.GetWidth(), m_window.GetHeight(), assetRoot)){
        Log::Error("Failed to initialize renderer.");
        return false;
    }

    Log::Info("Initializing demo scene.");

    ShaderDescription vertexShaderDescription{};
    vertexShaderDescription.path = "Triangle.hlsl";
    vertexShaderDescription.entryPoint = "VSMain";
    vertexShaderDescription.stage = ShaderStage::Vertex;

    m_demoVertexShader = m_renderer.GetShaderLibrary().Load(vertexShaderDescription);

    if(!m_demoVertexShader){
        Log::Error("Failed to load demo vertex shader.");
        return false;
    }

    ShaderDescription pixelShaderDescription{};
    pixelShaderDescription.path = "Triangle.hlsl";
    pixelShaderDescription.entryPoint = "PSMain";
    pixelShaderDescription.stage = ShaderStage::Pixel;

    m_demoPixelShader = m_renderer.GetShaderLibrary().Load(pixelShaderDescription);

    if(!m_demoPixelShader){
        Log::Error("Failed to load demo pixel shader.");
        return false;
    }

    const float aspectRatio = static_cast<float>(m_window.GetWidth()) / static_cast<float>(m_window.GetHeight());

    if(!m_demoSceneManager.Initialize(
        "Sphere Grid",
        m_renderer.GetDevice(),
        m_renderer.GetResourceUploader(),
        m_renderer.GetResourceDescriptorHeap(),
        m_demoVertexShader,
        m_demoPixelShader,
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

    m_renderedSceneStructureRevision = activeDemo.GetScene().GetStructureRevision();

    m_window.SetEventCallback(
        [this](const Event& event)
        {
            if(m_window.ShouldClose()){
                return;
            }

            switch(event.type){
                case EventType::WindowResized:
                    m_window.SetSize(event.width, event.height);

                    if(event.width > 0 && event.height > 0){
                        m_demoSceneManager.SetAspectRatio(static_cast<float>(event.width) / static_cast<float>(event.height));
                    }

                    if(!m_renderer.Resize(event.width, event.height)){
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
        }
    );

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

    while(!m_window.ShouldClose()){
        const auto frameStart = CpuTiming::Clock::now();
        const double frameInterval = hasPreviousFrameStart
            ? std::chrono::duration<double, std::milli>(frameStart - previousFrameStart).count()
            : 0.0;
        const bool intervalValid = hasPreviousFrameStart;
        previousFrameStart = frameStart;
        hasPreviousFrameStart = true;

        Frame();

        const double frameExecution = CpuTiming::MillisecondsSince(frameStart);
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
    m_time.Update();

    m_input.Update();
    m_window.ProcessMessages();

    if(m_window.ShouldClose()){
        return;
    }

    if(m_window.GetWidth() == 0 || m_window.GetHeight() == 0){
        m_window.WaitForMessage();
        // Exclude the suspended interval from the next simulation update.
        m_time.Update();
        return;
    }

    DebugUI& debugUI = m_renderer.GetDebugUI();

    if(debugUI.HasEnvironmentChangeRequest())
    {
        const size_t environmentIndex =
            debugUI.ConsumeEnvironmentChangeRequest();

        if(!m_renderer.SetEnvironment(environmentIndex))
        {
            Log::Error("Failed to switch environment.");
            m_window.Close();
            return;
        }
    }

    if(m_demoSceneManager.HasPendingSceneChange())
    {
        if(!m_renderer.WaitForGPU()){
            Log::Error("Failed to confirm GPU completion before scene switch; requesting application shutdown.");
            m_window.Close();
            return;
        }
        m_renderer.ReleaseSceneResources();

        if(!m_demoSceneManager.ApplyPendingSceneChange(
            m_renderer.GetDevice(),
            m_renderer.GetResourceUploader(),
            m_renderer.GetResourceDescriptorHeap(),
            m_demoVertexShader,
            m_demoPixelShader))
        {
            Log::Error("Failed to switch demo scene.");
            m_window.Close();
            return;
        }

        IDemoScene& switchedDemo = m_demoSceneManager.GetActiveScene();

        if(!m_renderer.InitializeSceneResources(switchedDemo.GetScene(), switchedDemo.GetMaterials()))
        {
            Log::Error("Failed to initialize renderer resources for switched demo scene.");
            m_window.Close();
            return;
        }

        m_renderedSceneStructureRevision = switchedDemo.GetScene().GetStructureRevision();
    }

    const std::vector<EnvironmentAsset>& environments = m_renderer.GetAvailableEnvironments();

    std::vector<std::string> environmentNames;
    environmentNames.reserve(environments.size());

    for(const EnvironmentAsset& environment : environments)
    {
        environmentNames.push_back(environment.displayName);
    }

    debugUI.BeginFrame();
    debugUI.DrawPerformanceWindow(environmentNames, m_renderer.GetCurrentEnvironmentIndex(), m_renderer.GetStatistics());

    IDemoScene& activeDemo = m_demoSceneManager.GetActiveScene();

    m_editor.Draw(activeDemo, m_demoSceneManager, static_cast<float>(m_window.GetWidth()), static_cast<float>(m_window.GetHeight()), m_time.GetDeltaTime());

    activeDemo.Update(m_time.GetDeltaTime());

    Scene& activeScene = activeDemo.GetScene();

    if(activeScene.GetStructureRevision() != m_renderedSceneStructureRevision){
        if(!m_renderer.WaitForGPU()){
            Log::Error("Failed to confirm GPU completion before scene rebuild; requesting application shutdown.");
            m_window.Close();
            return;
        }
        m_renderer.ReleaseSceneResources();

        if(!m_renderer.InitializeSceneResources(activeScene, activeDemo.GetMaterials())){
            Log::Error("Failed to rebuild renderer resources after scene structure change.");
            m_window.Close();
            return;
        }

        m_renderedSceneStructureRevision = activeScene.GetStructureRevision();
    }

    // Apply camera-selected displacement geometry before rendering.
    if(auto* mirrorScene = dynamic_cast<MirrorTestScene*>(&activeDemo))
    {
        if(mirrorScene->HasPendingDisplacementGeometryUpdate())
        {
            DisplacementGeometry geometry{};

            const auto generationStart = CpuTiming::Clock::now();
            if(!mirrorScene->GenerateCameraDisplacementGeometry(geometry))
            {
                Log::Error("Failed to regenerate displacement geometry.");
                m_window.Close();
                return;
            }

            const auto generationEnd = CpuTiming::Clock::now();
            const auto replacementStart = CpuTiming::Clock::now();
            if(!m_renderer.ReplaceMeshGeometry(
                mirrorScene->GetDisplacementTestMesh(),
                geometry.vertices.data(),
                geometry.vertices.size(),
                geometry.indices.data(),
                geometry.indices.size()))
            {
                Log::Error("Failed to replace displacement mesh geometry.");
                m_window.Close();
                return;
            }

            const auto replacementEnd = CpuTiming::Clock::now();
            mirrorScene->MarkDisplacementGeometryUpdated();

            // Temporary CPU timing for the camera-selected geometry update.
            const double generationMs = std::chrono::duration<double, std::milli>(
                generationEnd - generationStart).count();
            const double replacementMs = std::chrono::duration<double, std::milli>(
                replacementEnd - replacementStart).count();
            const double totalMs = std::chrono::duration<double, std::milli>(
                replacementEnd - generationStart).count();

            Log::Info(
                "Displacement geometry replaced: vertices=" +
                std::to_string(geometry.vertices.size()) +
                ", indices=" +
                std::to_string(geometry.indices.size()) +
                ", CPU generation ms=" + std::to_string(generationMs) +
                ", mesh replacement ms=" + std::to_string(replacementMs) +
                ", total geometry update ms=" + std::to_string(totalMs));
        }
    }

    RendererOptions rendererOptions{};
    rendererOptions.enableRTGI = debugUI.IsRTGIEnabled();
    rendererOptions.rtgiContactAOStrength = debugUI.GetRTGIContactAOStrength();
    rendererOptions.rtgiContactAORadius = debugUI.GetRTGIContactAORadius();
    
    const size_t selectedObject = m_editor.GetSelectedObject();

    if(m_editor.GetSelectionStructureRevision() == activeScene.GetStructureRevision() && selectedObject < activeScene.GetRenderObjects().size()){
        rendererOptions.selectedObject = selectedObject;
    }

    if(!m_renderer.Render(activeScene, rendererOptions)){
        Log::Error("Frame rendering failed; requesting application shutdown.");
        m_window.Close();
        return;
    }

    activeScene.ClearChangedTransforms();
}

void Engine::Shutdown()
{
    ENGINE_ASSERT(m_state == EngineState::Running);
    Log::Info("Engine shutdown: starting.");

    m_state = EngineState::Stopped;

    if(!m_renderer.WaitForGPU()){
        Log::Error("Engine shutdown: GPU completion is unconfirmed; aborting without resource teardown.");
        std::abort();
    }

    m_renderer.ReleaseSceneResources();
    m_demoSceneManager.Shutdown();
    
    m_renderer.Shutdown();

    m_window.Shutdown();
    Log::Info("Engine shutdown: finished.");
}
