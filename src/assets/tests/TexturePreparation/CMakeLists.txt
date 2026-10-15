cmake_minimum_required(VERSION 3.25)
project(TexturePreparationTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
add_executable(TexturePreparationTests TexturePreparationTests.cpp
    ${ENGINE_ROOT}/src/assets/ProjectTexturePreparationService.cpp
    ${ENGINE_ROOT}/src/assets/CookedSurface.cpp
    ${ENGINE_ROOT}/src/assets/CookedEnvironment.cpp
    ${ENGINE_ROOT}/src/assets/CookedMesh.cpp
    ${ENGINE_ROOT}/src/assets/CookedMeshProvider.cpp
    ${ENGINE_ROOT}/src/assets/ArchiveCookedMeshProvider.cpp
    ${ENGINE_ROOT}/src/assets/EngineAssetPak.cpp
    ${ENGINE_ROOT}/src/Scene/SceneDocument.cpp
    ${ENGINE_ROOT}/src/assets/AssetID.cpp
    ${ENGINE_ROOT}/src/assets/AssetRecord.cpp
    ${ENGINE_ROOT}/src/assets/ProjectAssetRegistry.cpp
    ${ENGINE_ROOT}/src/assets/ProjectAssetResolver.cpp
    ${ENGINE_ROOT}/src/assets/MaterialAssetSerializer.cpp
    ${ENGINE_ROOT}/src/Project/ProjectPaths.cpp
    ${ENGINE_ROOT}/src/Renderer/Preview/MaterialThumbnailValidation.cpp
    ${ENGINE_ROOT}/src/Renderer/ProjectTextureLoader.cpp
    ${ENGINE_ROOT}/src/Renderer/Texture.cpp
    ${ENGINE_ROOT}/src/Renderer/ImageLoader.cpp
    ${ENGINE_ROOT}/src/Renderer/DDSLoader.cpp
    ${ENGINE_ROOT}/src/Renderer/ResourceUploader.cpp
    ${ENGINE_ROOT}/src/Renderer/GraphicsDevice.cpp
    ${ENGINE_ROOT}/src/Renderer/DiscriptorHeap.cpp
    ${ENGINE_ROOT}/src/Core/Log.cpp)
target_include_directories(TexturePreparationTests PRIVATE ${ENGINE_ROOT}/src ${ENGINE_ROOT}/external/stb
    ${ENGINE_ROOT}/build/_deps/directxtex-src/DirectXTex
    ${ENGINE_ROOT}/build/_deps/nlohmann_json-src/include)
target_compile_definitions(TexturePreparationTests PRIVATE NOMINMAX DIRECTX_TEX_IMPORT ENGINE_TEXTURE_PREPARATION_TESTS)
target_compile_options(TexturePreparationTests PRIVATE /EHsc /Gy)
target_link_options(TexturePreparationTests PRIVATE /OPT:REF /INCREMENTAL:NO)
target_link_libraries(TexturePreparationTests PRIVATE d3d12 dxgi ole32 bcrypt
    ${ENGINE_ROOT}/build/lib/Debug/DirectXTexd.lib)
add_custom_command(TARGET TexturePreparationTests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${ENGINE_ROOT}/build/bin/Debug/DirectXTexd.dll $<TARGET_FILE_DIR:TexturePreparationTests>)
enable_testing()
add_test(NAME TexturePreparationTests COMMAND TexturePreparationTests)

get_target_property(PREPARATION_SOURCES TexturePreparationTests SOURCES)
list(REMOVE_ITEM PREPARATION_SOURCES TexturePreparationTests.cpp)
add_executable(MaterialPreparationTests MaterialPreparationTests.cpp ${PREPARATION_SOURCES}
    ${ENGINE_ROOT}/src/assets/ProjectMaterialPreparationService.cpp)
get_target_property(PREPARATION_INCLUDES TexturePreparationTests INCLUDE_DIRECTORIES)
target_include_directories(MaterialPreparationTests PRIVATE ${PREPARATION_INCLUDES})
target_compile_definitions(MaterialPreparationTests PRIVATE NOMINMAX DIRECTX_TEX_IMPORT ENGINE_TEXTURE_PREPARATION_TESTS)
target_compile_options(MaterialPreparationTests PRIVATE /EHsc /Gy)
target_link_options(MaterialPreparationTests PRIVATE /OPT:REF /INCREMENTAL:NO)
target_link_libraries(MaterialPreparationTests PRIVATE d3d12 dxgi ole32 bcrypt
    ${ENGINE_ROOT}/build/lib/Debug/DirectXTexd.lib)
add_test(NAME MaterialPreparationTests COMMAND MaterialPreparationTests)
