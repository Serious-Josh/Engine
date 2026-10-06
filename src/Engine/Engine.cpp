#include "Engine.h"
#include "Core/InputCodes.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "Core/Time.h"
#include "Scene/Scene.h"

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

    m_demoVertexShader =m_renderer.GetShaderLibrary().Load(vertexShaderDescription);

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
        DemoSceneId::SphereGrid,
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

    m_window.SetEventCallback(
        [this](const Event& event)
        {
            switch(event.type){
                case EventType::WindowResized:
                    m_window.SetSize(event.width, event.height);

                    if(event.width > 0 && event.height > 0){
                        m_demoSceneManager.SetAspectRatio(static_cast<float>(event.width) / static_cast<float>(event.height));
                    }

                    m_renderer.Resize(event.width, event.height);
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

    if(m_demoSceneManager.HasPendingSceneChange())
    {
        m_renderer.WaitForGPU();
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
    }

    DebugUI& debugUI = m_renderer.GetDebugUI();

    debugUI.BeginFrame();
    debugUI.DrawPerformanceWindow();

    IDemoScene& activeDemo = m_demoSceneManager.GetActiveScene();

    m_editor.Draw(activeDemo, m_demoSceneManager, m_demoSceneManager.GetActiveSphereGridScene());

    activeDemo.Update(m_time.GetDeltaTime());

    m_renderer.Render(activeDemo.GetScene());

    activeDemo.GetScene().ClearChangedTransforms();
}

void Engine::Shutdown()
{
    ENGINE_ASSERT(m_state == EngineState::Running);
    Log::Info("Engine shutdown: starting.");

    m_state = EngineState::Stopped;

    m_renderer.WaitForGPU();

    m_renderer.ReleaseSceneResources();
    m_demoSceneManager.Shutdown();
    
    m_renderer.Shutdown();

    m_window.Shutdown();
    Log::Info("Engine shutdown: finished.");
}

Input& Engine::GetInput(){
    return m_input;
}

unsigned int Engine::GetWindowWidth() const
{
    return m_window.GetWidth();
}

unsigned int Engine::GetWindowHeight() const
{
    return m_window.GetHeight();
}