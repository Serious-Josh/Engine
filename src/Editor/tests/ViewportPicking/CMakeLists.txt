cmake_minimum_required(VERSION 3.25)
project(ViewportPickingTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
add_executable(ViewportPickingTests ViewportPickingTests.cpp
    ${ENGINE_ROOT}/src/Renderer/Geometry/SphereGenerator.cpp
    ${ENGINE_ROOT}/src/Scene/Scene.cpp)
target_include_directories(ViewportPickingTests PRIVATE ${ENGINE_ROOT}/src)
target_compile_definitions(ViewportPickingTests PRIVATE NOMINMAX)
target_compile_options(ViewportPickingTests PRIVATE /EHsc)
enable_testing()
add_test(NAME ViewportPickingTests COMMAND ViewportPickingTests)
