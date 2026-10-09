#include "Engine.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "Core/Time.h"
#include "Scene/Scene.h"
#include "DemoContent/SceneRegistry.h"

#include <cstdlib>

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

    while(!m_window.ShouldClose()){
        Frame();
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

    RendererOptions rendererOptions{};
    rendererOptions.enableRTGI = debugUI.IsRTGIEnabled();
    
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
