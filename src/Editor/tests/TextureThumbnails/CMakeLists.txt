cmake_minimum_required(VERSION 3.25)
project(TextureThumbnailTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
add_executable(TextureThumbnailTests TextureThumbnailTests.cpp
    ${ENGINE_ROOT}/src/Editor/TextureThumbnailCache.cpp
    ${ENGINE_ROOT}/src/Editor/TextureThumbnailDiskCache.cpp
    ${ENGINE_ROOT}/src/Renderer/Texture.cpp
    ${ENGINE_ROOT}/src/Renderer/DiscriptorHeap.cpp
    ${ENGINE_ROOT}/src/Renderer/ImageLoader.cpp
    ${ENGINE_ROOT}/src/Renderer/DDSLoader.cpp
    ${ENGINE_ROOT}/src/Renderer/ResourceUploader.cpp
    ${ENGINE_ROOT}/src/Renderer/GraphicsDevice.cpp
    ${ENGINE_ROOT}/src/Core/Log.cpp
    ${ENGINE_ROOT}/src/assets/AssetID.cpp
    ${ENGINE_ROOT}/src/assets/AssetRecord.cpp
    ${ENGINE_ROOT}/src/assets/ProjectAssetRegistry.cpp
    ${ENGINE_ROOT}/src/assets/ProjectAssetResolver.cpp
    ${ENGINE_ROOT}/src/Project/ProjectPaths.cpp)
target_include_directories(TextureThumbnailTests PRIVATE ${ENGINE_ROOT}/src ${ENGINE_ROOT}/external/stb
    ${ENGINE_ROOT}/build/_deps/directxtex-src/DirectXTex)
target_compile_definitions(TextureThumbnailTests PRIVATE NOMINMAX DIRECTX_TEX_IMPORT)
target_compile_options(TextureThumbnailTests PRIVATE /EHsc /Gy)
target_link_options(TextureThumbnailTests PRIVATE /OPT:REF /INCREMENTAL:NO)
target_link_libraries(TextureThumbnailTests PRIVATE d3d12 dxgi ole32
    ${ENGINE_ROOT}/build/lib/Debug/DirectXTexd.lib)
add_custom_command(TARGET TextureThumbnailTests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${ENGINE_ROOT}/build/bin/Debug/DirectXTexd.dll $<TARGET_FILE_DIR:TextureThumbnailTests>)
enable_testing()
add_test(NAME TextureThumbnailTests COMMAND TextureThumbnailTests)
