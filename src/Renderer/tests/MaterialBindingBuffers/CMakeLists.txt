cmake_minimum_required(VERSION 3.25)
project(MaterialBindingBufferTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
add_executable(MaterialBindingBufferTests MaterialBindingBufferTests.cpp
    ${ENGINE_ROOT}/src/Renderer/StructuredBuffer.cpp
    ${ENGINE_ROOT}/src/Renderer/UploadBuffer.cpp
    ${ENGINE_ROOT}/src/Renderer/DiscriptorHeap.cpp
    ${ENGINE_ROOT}/src/Renderer/ResourceUploader.cpp
    ${ENGINE_ROOT}/src/Renderer/GraphicsDevice.cpp
    ${ENGINE_ROOT}/src/Core/Log.cpp)
target_include_directories(MaterialBindingBufferTests PRIVATE ${ENGINE_ROOT}/src)
target_compile_definitions(MaterialBindingBufferTests PRIVATE NOMINMAX)
target_compile_options(MaterialBindingBufferTests PRIVATE /EHsc /Gy)
target_link_options(MaterialBindingBufferTests PRIVATE /OPT:REF /INCREMENTAL:NO)
target_link_libraries(MaterialBindingBufferTests PRIVATE d3d12 dxgi)
enable_testing()
add_test(NAME MaterialBindingBufferTests COMMAND MaterialBindingBufferTests)
