cmake_minimum_required(VERSION 3.25)

project(Engine
    VERSION 0.1.0
    LANGUAGES CXX
)

set(DXC
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x86/dxc.exe"
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
    src/Renderer/Shader.cpp
    src/Renderer/Texture.cpp
    src/Renderer/DiscriptorHeap.cpp
    src/Renderer/ResourceUploader.cpp
    src/Renderer/SwapChain.cpp
    src/Renderer/CommandContext.cpp
    src/Renderer/GraphicsDevice.cpp
)

target_include_directories(Engine
    PRIVATE
        src
)

set(SHADER_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Renderer/Shaders"
)

set(SHADER_OUTPUT_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/Shaders"
)

add_custom_command(
    OUTPUT
        "${SHADER_OUTPUT_DIR}/TriangleVS.cso"
        "${SHADER_OUTPUT_DIR}/TrianglePS.cso"

    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${SHADER_OUTPUT_DIR}"

    COMMAND ${DXC}
        "${SHADER_SOURCE_DIR}/Triangle.hlsl"
        /T vs_6_0
        /E VSMain
        /Fo "${SHADER_OUTPUT_DIR}/TriangleVS.cso"

    COMMAND ${DXC}
        "${SHADER_SOURCE_DIR}/Triangle.hlsl"
        /T ps_6_0
        /E PSMain
        /Fo "${SHADER_OUTPUT_DIR}/TrianglePS.cso"

    DEPENDS
        "${SHADER_SOURCE_DIR}/Triangle.hlsl"

    COMMENT "Compiling shaders"
)

add_custom_target(CompileShaders ALL
    DEPENDS
        "${SHADER_OUTPUT_DIR}/TriangleVS.cso"
        "${SHADER_OUTPUT_DIR}/TrianglePS.cso"
)

add_dependencies(Engine CompileShaders)

target_compile_definitions(Engine
    PRIVATE
        ENGINE_SHADER_DIR="$<TARGET_FILE_DIR:Engine>/Shaders"
)

target_link_libraries(Engine
    PRIVATE
        dxgi
        d3d12
)