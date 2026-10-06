cmake_minimum_required(VERSION 3.25)

project(Engine
    VERSION 0.1.0
    LANGUAGES CXX
)

set(DXC_RUNTIME_DIR
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(Engine
    src/main.cpp
    src/Engine/Engine.cpp
    src/Platform/Windows/WindowsWindow.cpp
    src/Core/Time.cpp
    src/Core/Input.cpp
    src/Core/Log.cpp
    src/Renderer/Renderer.cpp
    src/Renderer/Buffer.cpp
    src/Renderer/Mesh.cpp
    src/Renderer/Pipeline.cpp
    src/Renderer/VertexLayout.cpp
    src/Renderer/Shader/Shader.cpp
    src/Renderer/Texture.cpp
    src/Renderer/DiscriptorHeap.cpp
    src/Renderer/ResourceUploader.cpp
    src/Renderer/SwapChain.cpp
    src/Renderer/CommandContext.cpp
    src/Renderer/GraphicsDevice.cpp
    src/DemoContent/DemoScene.cpp
    src/Renderer/RenderTarget.cpp
    src/Renderer/GBuffer.cpp
    src/Renderer/DepthBuffer.cpp
    src/Renderer/UploadBuffer.cpp
    src/Renderer/Shader/ShaderCompiler.cpp
    src/Renderer/Shader/ShaderLibrary.cpp
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/imgui_impl_win32.cpp
    external/imgui/imgui_impl_dx12.cpp
    src/Debug/DebugUI.cpp
    src/Scene/Scene.cpp
    src/Renderer/Geometry/SphereGenerator.cpp
    src/Renderer/StructuredBuffer.cpp
    src/Renderer/ImageLoader.cpp
    src/Renderer/Passes/ToneMapPass.cpp
    src/Renderer/Passes/DeferrerLightingPass.cpp
    src/Renderer/EnvironmentMap.cpp
    src/Renderer/Passes/EquirectangularToCubemapPass.cpp
    src/Renderer/Passes/EnvironmentMipPass.cpp
    src/Renderer/Passes/BRDFIntegrationpass.cpp
    src/Renderer/Passes/DiffuseIrradiancePass.cpp
    src/Renderer/Passes/GGXAverageAlbedoPass.cpp
    src/Renderer/Passes/GGXMultiscatterIrradiancePass.cpp
    src/Renderer/RayTracing/BottomLevelAccelerationStructure.cpp
    src/Renderer/RayTracing/TopLevelAccelerationStructure.cpp
    src/Renderer/RayTracing/RayTracingPipeline.cpp
    src/Renderer/RayTracing/ShaderTable.cpp
    src/Renderer/RayTracing/RayTracingSceneResources.cpp
    src/Renderer/Passes/RayTracedShadowPass.cpp
    src/Renderer/Passes/ShadowDenoisePass.cpp
    src/Renderer/ComputePipeline.cpp
    src/Editor/Editor.cpp
    src/Renderer/Passes/RayTracedReflectionPass.cpp
    src/Renderer/Passes/RayTracedReflectionDenoisePass.cpp
    src/Renderer/Passes/RayTracedReflectionTemporalPass.cpp
    src/Renderer/Passes/NrdReflectionDenoiser.cpp
    src/Renderer/Passes/NrdReflectionPreparePass.cpp
    src/Renderer/Passes/ReflectionContributionLimitPass.cpp
    src/Renderer/Passes/NrdReflectionResolvePass.cpp
)

add_custom_command(
    TARGET Engine
    POST_BUILD

    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:Engine>/assets"

    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${DXC_RUNTIME_DIR}/dxcompiler.dll"
        "$<TARGET_FILE_DIR:Engine>/dxcompiler.dll"

    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${DXC_RUNTIME_DIR}/dxil.dll"
        "$<TARGET_FILE_DIR:Engine>/dxil.dll"

    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/external/NRD/Lib/$<CONFIG>/NRD.dll"
        "$<TARGET_FILE_DIR:Engine>/NRD.dll"

    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/external/NRI/Lib/$<CONFIG>/NRI.dll"
        "$<TARGET_FILE_DIR:Engine>/NRI.dll"

    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:Engine>/assets/Shaders/NRD"

    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/external/NRD/Shaders/NRD.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/external/NRD/Shaders/NRDConfig.hlsli"
        "$<TARGET_FILE_DIR:Engine>/assets/Shaders/NRD"

    COMMENT "Copying runtime assets and dependencies"
)

target_include_directories(Engine
    PRIVATE
        src
        external/imgui
        external/stb
        external/NRD/Include
        external/NRD/Integration
        external/NRI/Include
)

target_link_libraries(Engine
    PRIVATE
        dxgi
        d3d12
        dxcompiler

        "$<$<CONFIG:Debug>:${CMAKE_CURRENT_SOURCE_DIR}/external/NRD/Lib/Debug/NRD.lib>"
        "$<$<CONFIG:Release>:${CMAKE_CURRENT_SOURCE_DIR}/external/NRD/Lib/Release/NRD.lib>"

        "$<$<CONFIG:Debug>:${CMAKE_CURRENT_SOURCE_DIR}/external/NRI/Lib/Debug/NRI.lib>"
        "$<$<CONFIG:Release>:${CMAKE_CURRENT_SOURCE_DIR}/external/NRI/Lib/Release/NRI.lib>"
)