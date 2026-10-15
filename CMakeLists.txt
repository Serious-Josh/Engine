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

include(FetchContent)

# Assimp
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    assimp
    URL https://github.com/assimp/assimp/archive/refs/tags/v6.0.2.tar.gz
)

FetchContent_MakeAvailable(assimp)

# DirectXTex - DDS loading and texture processing
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    directxtex
    GIT_REPOSITORY https://github.com/microsoft/DirectXTex.git
    GIT_TAG main
)

FetchContent_MakeAvailable(directxtex)

# JSON serialization for persistent scene and material assets
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
)

FetchContent_MakeAvailable(nlohmann_json)

add_library(EngineCore OBJECT
    src/Engine/Engine.cpp
    src/Engine/BuildGame.cpp
    src/Engine/SceneEnvironment.cpp
    src/assets/CookedEnvironment.cpp
    $<$<CONFIG:Debug>:src/Engine/SceneEnvironmentVerification.cpp>
    src/Editor/BuildGameWindow.cpp
    $<$<CONFIG:Debug>:src/Engine/BuildGameVerification.cpp>
    src/Engine/CookedMeshGPUVerification.cpp
    $<$<CONFIG:Debug>:src/Engine/ArchiveCookedMeshVerification.cpp>
    $<$<CONFIG:Debug>:src/Engine/CookedSceneArchiveVerification.cpp>
    src/Engine/ArchiveSurfaceAdmission.cpp
    src/Engine/ArchiveSceneBootstrap.cpp
    src/Engine/PackagedBootstrap.cpp
    $<$<CONFIG:Debug>:src/Engine/PackagedSurfaceVerification.cpp>
    src/Engine/CookedMeshLifecycleVerification.cpp
    src/assets/ProjectTexturePreparationService.cpp
    src/assets/ProjectMaterialPreparationService.cpp
    src/Renderer/MaterialAdmission.cpp
    src/Engine/MaterialAdmissionVerification.cpp
    src/assets/ProjectMeshPreparationService.cpp
    src/assets/CookedSurface.cpp
    src/assets/CookedScene.cpp
    src/Scene/ArchiveSceneActivation.cpp
    src/assets/CookedMeshProvider.cpp
    src/assets/ArchiveCookedMeshProvider.cpp
    src/assets/EngineAssetPak.cpp
    src/assets/CookedMesh.cpp
    src/assets/DevelopmentCookedMeshProvider.cpp
    $<$<CONFIG:Debug>:src/assets/CookedMeshVerification.cpp>
    $<$<CONFIG:Debug>:src/assets/OfflineMeshCooker.cpp>
    $<$<CONFIG:Debug>:src/assets/DevelopmentCookedMeshProviderVerification.cpp>
    $<$<CONFIG:Debug>:src/assets/CookedMeshPreparationVerification.cpp>
    src/Scene/FileSceneTextureStreaming.cpp
    src/Platform/Windows/WindowsWindow.cpp
    src/Core/Time.cpp
    src/Core/Input.cpp
    src/Core/Log.cpp
    src/Renderer/Renderer.cpp
    src/Renderer/Displacement/DisplacementUpdateState.cpp
    src/Renderer/Displacement/DisplacementUpdateCoordinator.cpp
    src/Renderer/Displacement/DisplacementCommandRecorder.cpp
    src/Renderer/Preview/MaterialPreviewRenderer.cpp
    src/Renderer/Preview/MaterialThumbnailDiskCache.cpp
    src/Renderer/Preview/MaterialThumbnailValidation.cpp
    src/Renderer/ProjectTextureLoader.cpp
    src/Renderer/Displacement/DisplacementRequestValidation.cpp
    src/Renderer/Diagnostics/FrameHitchDiagnostics.cpp
    src/Renderer/Displacement/DisplacementGeometryBinding.cpp
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
    src/DemoContent/SphereGridScene.cpp
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
    external/ImGuizmo/ImGuizmo.cpp
    src/Debug/DebugUI.cpp
    src/Scene/Scene.cpp
    src/Renderer/Geometry/SphereGenerator.cpp
    src/Renderer/Geometry/DisplacementMeshGenerator.cpp
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
    src/Renderer/RayTracing/RayTracingMembershipCandidate.cpp
    src/Renderer/RendererMembershipCandidate.cpp
    src/Renderer/Passes/RayTracedShadowPass.cpp
    src/Renderer/Passes/ShadowDenoisePass.cpp
    src/Renderer/ComputePipeline.cpp
    src/Editor/Editor.cpp
    src/Editor/ViewportMaterialDrop.cpp
    src/Editor/EditorWorkspace.cpp
    src/Editor/SceneHierarchyPanel.cpp
    src/Editor/InspectorPanel.cpp
    src/assets/ProjectAssetDeletion.cpp
    src/assets/ProjectAssetRename.cpp
    src/assets/AssetReconciliationTransaction.cpp
    src/assets/ProjectAssetReconciliation.cpp
    src/Editor/TextureThumbnailCache.cpp
    src/Editor/TextureThumbnailDiskCache.cpp
    src/Editor/AssetBrowser.cpp
    src/Renderer/Passes/RayTracedReflectionPass.cpp
    src/Renderer/Passes/RayTracedReflectionDenoisePass.cpp
    src/Renderer/Passes/RayTracedReflectionTemporalPass.cpp
    src/Renderer/Passes/NrdDenoiser.cpp
    src/Renderer/Passes/NrdPreparePass.cpp
    src/Renderer/Passes/ReflectionContributionLimitPass.cpp
    src/Renderer/Passes/NrdReflectionResolvePass.cpp
    src/DemoContent/DemoSceneManager.cpp
    src/DemoContent/MirrorTestScene.cpp
    src/Renderer/GpuTimestampProfiler.cpp
    src/assets/ModelImporter.cpp
    src/Renderer/Preview/MeshThumbnailPreparation.cpp
    src/DemoContent/CornellBoxScene.cpp
    src/DemoContent/SceneRegistry.cpp
    src/DemoContent/StressTestScene.cpp
    src/Renderer/Passes/SelectionMaskPass.cpp
    src/Renderer/Passes/RayTracedGIPass.cpp
    src/Renderer/Passes/NrdDiffuseResolvePass.cpp
    src/Renderer/Passes/RayTracedAOPass.cpp
    src/Renderer/Passes/AOTemporalPass.cpp
    src/Renderer/Passes/AODenoisePass.cpp
    src/Renderer/Passes/GISpatialDenoisePass.cpp
    src/Renderer/Passes/GISignedCorrectionPass.cpp
    src/Renderer/Passes/AOResolvePass.cpp
    src/Renderer/DDSLoader.cpp
    src/Renderer/Passes/DisplacementVerticesPass.cpp
    src/Renderer/Passes/DisplacementIndicesPass.cpp
    src/Renderer//DisplacementGPUIndexLayout.cpp
    src/Renderer/DisplacementGPUPatchBuffer.cpp
    src/Scene/SceneDocument.cpp
    src/Scene/SceneSerializer.cpp
    src/Scene/SceneFileCreation.cpp
    src/Project/ProjectPaths.cpp
    src/Project/ProjectLocator.cpp
    src/Scene/FileScene.cpp
    src/Scene/SceneDocumentAdapter.cpp
    src/Scene/SceneObjectResolver.cpp
    src/Scene/PreparedScene.cpp
    src/Scene/SceneCommit.cpp
    src/Scene/FileSceneRuntime.cpp
    src/Scene/FileSceneAssetCatalog.cpp
    src/Scene/SceneMetadataPreparation.cpp
    src/Scene/MeshResidency.cpp
    src/Scene/CookedMeshCatalogPreparation.cpp
    src/Editor/MaterialLocate.cpp
    src/Engine/MaterialLocateVerification.cpp
    src/Engine/ViewportPanVerification.cpp
    src/Editor/SceneObjectDuplication.cpp
    src/Editor/SceneObjectDuplicationControls.cpp
    src/Engine/EditorMembership.cpp
    src/Engine/SceneDuplicationVerification.cpp
    src/Scene/SceneMembershipPublication.cpp
    src/Engine/SceneMembershipVerification.cpp
    src/Scene/MeshResidencyPublication.cpp
    src/Scene/SceneMaterialBindingPublication.cpp
    src/Engine/MaterialBindingVerification.cpp
    src/Engine/ViewportMaterialAssignment.cpp
    src/Engine/ViewportMaterialDropVerification.cpp
    src/DemoContent/FileBackedDemoScene.cpp
    src/assets/AssetID.cpp
    src/assets/AssetRecord.cpp
    src/assets/ProjectAssetRegistry.cpp
    src/assets/AssetRegistrySerializer.cpp
    src/assets/ProjectAssetResolver.cpp
    src/assets/MaterialAssetSerializer.cpp
    src/assets/ProjectTextureImportService.cpp
    src/assets/ProjectMeshImportService.cpp
    src/assets/ProjectMeshCPUCache.cpp
    src/assets/ProjectMaterialAssetService.cpp
)

target_compile_definitions(EngineCore PRIVATE
    ENGINE_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
)

# Object sharing also retains static scene-registration units for editor startup.
# Both executables use one implementation; only the entry point selects capabilities.
add_executable(Engine src/main.cpp)
target_link_libraries(Engine PRIVATE EngineCore)
add_executable(EngineRuntime WIN32 src/runtime_main.cpp)
target_link_libraries(EngineRuntime PRIVATE EngineCore shell32)
add_dependencies(Engine EngineRuntime)
target_compile_definitions(EngineCore PRIVATE ENGINE_RUNTIME_DEPLOY_DIR="$<TARGET_FILE_DIR:EngineRuntime>")
set_target_properties(EngineRuntime PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Game/$<CONFIG>"
)

# Runtime deployment contains binaries only. Archives are supplied by the offline cook.
add_custom_command(TARGET EngineRuntime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${DXC_RUNTIME_DIR}/dxcompiler.dll"
        "${DXC_RUNTIME_DIR}/dxil.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/external/NRD/Lib/$<CONFIG>/NRD.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/external/NRI/Lib/$<CONFIG>/NRI.dll"
        $<TARGET_FILE:assimp>
        $<TARGET_FILE:DirectXTex>
        $<TARGET_FILE_DIR:EngineRuntime>
    COMMENT "Copying packaged runtime DLL dependencies (no development assets)"
    VERBATIM
)

add_custom_command(
    TARGET Engine
    POST_BUILD

    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:Engine>/assets"

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

    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:assimp>
        $<TARGET_FILE_DIR:Engine>

    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:DirectXTex>
        $<TARGET_FILE_DIR:Engine>

    COMMENT "Copying runtime assets and dependencies"
)

target_include_directories(EngineCore
    PUBLIC
        src
        external/imgui
        external/ImGuizmo
        external/stb
        external/NRD/Include
        external/NRD/Integration
        external/NRI/Include
)

target_link_libraries(EngineCore
    PUBLIC
        shell32
        ole32
        dxgi
        d3d12
        dxcompiler

        "$<$<CONFIG:Debug>:${CMAKE_CURRENT_SOURCE_DIR}/external/NRD/Lib/Debug/NRD.lib>"
        "$<$<CONFIG:Release>:${CMAKE_CURRENT_SOURCE_DIR}/external/NRD/Lib/Release/NRD.lib>"

        "$<$<CONFIG:Debug>:${CMAKE_CURRENT_SOURCE_DIR}/external/NRI/Lib/Debug/NRI.lib>"
        "$<$<CONFIG:Release>:${CMAKE_CURRENT_SOURCE_DIR}/external/NRI/Lib/Release/NRI.lib>"

        assimp
        DirectXTex
        nlohmann_json
)
