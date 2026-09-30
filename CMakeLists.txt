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

    COMMENT "Copying runtime assets and dependencies"
)

target_include_directories(Engine
    PRIVATE
        src
)

target_link_libraries(Engine
    PRIVATE
        dxgi
        d3d12
        dxcompiler
)