cmake_minimum_required(VERSION 3.25)

project(Engine
    VERSION 0.1.0
    LANGUAGES CXX
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
)

target_include_directories(Engine
    PRIVATE
        src
)

target_link_libraries(Engine
    PRIVATE
        dxgi
        d3d12
)