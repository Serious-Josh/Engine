cmake_minimum_required(VERSION 3.25)

project(Engine
    VERSION 0.1.0
    LANGUAGES CXX
)

set(DXC_RUNTIME_DIR
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
    CACHE PATH "DXC DLL directory for normal development target deployment"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENGINE_BUILD_DIAGNOSTICS "Build local regression diagnostics (Debug only)" OFF)

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

include(cmake/Box3D.cmake)
add_subdirectory(src/Physics)
include(cmake/AngelScript.cmake)

add_library(EngineScripting STATIC src/Scripting/ScriptHost.cpp src/Scripting/ScriptSources.cpp)
target_include_directories(EngineScripting PUBLIC src)
target_link_libraries(EngineScripting PRIVATE EngineScriptString EngineScriptBuilder bcrypt)

add_library(EngineScriptAssets STATIC src/assets/ProjectScriptAssets.cpp)
target_include_directories(EngineScriptAssets PUBLIC src)
target_link_libraries(EngineScriptAssets PUBLIC EngineScripting PRIVATE nlohmann_json)

if(ENGINE_BUILD_DIAGNOSTICS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/Engine/tests/ScriptHostTests.cpp")
    add_executable(ScriptHostTests src/Engine/tests/ScriptHostTests.cpp src/Core/Log.cpp)
    target_link_libraries(ScriptHostTests PRIVATE EngineScripting EngineScriptString)
endif()

if(ENGINE_BUILD_DIAGNOSTICS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/tests/ScriptAssetTests.cpp")
    add_executable(ScriptAssetTests src/assets/tests/ScriptAssetTests.cpp
        src/Project/ProjectPaths.cpp src/assets/AssetID.cpp src/assets/AssetRecord.cpp
        src/assets/ProjectAssetRegistry.cpp src/assets/AssetRegistrySerializer.cpp
        src/assets/ProjectAssetResolver.cpp src/assets/ProjectAssetReconciliation.cpp
        src/assets/AssetReconciliationTransaction.cpp src/assets/ProjectAssetRename.cpp
        src/assets/ProjectAssetDeletion.cpp src/assets/MaterialAssetSerializer.cpp src/Core/Log.cpp)
    target_link_libraries(ScriptAssetTests PRIVATE EngineScriptAssets nlohmann_json ole32)
endif()

add_library(EngineCore OBJECT
    src/Engine/Engine.cpp
    src/Engine/BuildGame.cpp
    src/Engine/SceneEnvironment.cpp
    src/assets/CookedEnvironment.cpp
    src/Editor/BuildGameWindow.cpp
    src/Engine/ArchiveSurfaceAdmission.cpp
    src/Engine/ArchiveSceneBootstrap.cpp
    src/Engine/PackagedBootstrap.cpp
    src/assets/ProjectTexturePreparationService.cpp
    src/assets/ProjectMaterialPreparationService.cpp
    src/Renderer/MaterialAdmission.cpp
    src/assets/ProjectMeshPreparationService.cpp
    src/assets/CookedSurface.cpp
    src/assets/CookedScene.cpp
    src/Scene/ArchiveSceneActivation.cpp
    src/assets/CookedMeshProvider.cpp
    src/assets/ArchiveCookedMeshProvider.cpp
    src/assets/EngineAssetPak.cpp
    src/assets/CookedMesh.cpp
    src/assets/DevelopmentCookedMeshProvider.cpp
    $<$<CONFIG:Debug>:src/assets/OfflineMeshCooker.cpp>
    src/Scene/FileSceneTextureStreaming.cpp
    src/Platform/Windows/WindowsWindow.cpp
    src/Platform/Windows/EditorDistributionPaths.cpp
    src/Platform/Windows/CommandLineArguments.cpp
    src/Platform/Windows/AtomicFileWrite.cpp
    src/Platform/Windows/ProjectFileAssociation.cpp
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
    src/Scene/SceneHierarchyTransforms.cpp
    src/Scene/SceneHierarchyPublication.cpp
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
    src/Editor/EditorPreferences.cpp
    src/Editor/ProjectBrowser.cpp
    src/Editor/ProjectSettingsWindow.cpp
    src/Editor/TriggerColliderControls.cpp
    src/Editor/CharacterControllerControls.cpp
    src/Editor/CharacterControllerDiagnosticsControls.cpp
    src/Editor/ViewportMaterialDrop.cpp
    src/Editor/EditorWorkspace.cpp
    src/Editor/ViewportMarquee.cpp
    src/Editor/ViewportMarqueeControls.cpp
    src/Editor/SceneHierarchyPanel.cpp
    src/Editor/InspectorPanel.cpp
    src/Editor/InspectorNumericDrag.cpp
    src/assets/ProjectAssetDeletion.cpp
    src/assets/ProjectAssetRename.cpp
    src/assets/AssetReconciliationTransaction.cpp
    src/assets/ProjectAssetReconciliation.cpp
    src/Editor/TextureThumbnailCache.cpp
    src/Editor/TextureThumbnailDiskCache.cpp
    src/Editor/AssetBrowser.cpp
    src/Editor/ThumbnailAdmission.cpp
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
    src/Renderer/EditorColliderOverlay.cpp
    src/Renderer/Passes/EditorColliderPass.cpp
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
    src/Project/ProjectContext.cpp
    src/Project/ProjectManifest.cpp
    src/Project/ProjectCreation.cpp
    src/Project/ProjectLock.cpp
    src/Project/ProjectLoader.cpp
    src/Project/EditorCommandLine.cpp
    src/Project/PersonalSettingsFile.cpp
    src/Project/ProjectWorkspace.cpp
    src/Scene/FileScene.cpp
    src/Scene/SceneDocumentAdapter.cpp
    src/Scene/SceneObjectResolver.cpp
    src/Scene/PreparedScene.cpp
    src/Scene/SceneCommit.cpp
    src/Scene/FileSceneRuntime.cpp
    src/Scene/TriggerColliderEditing.cpp
    src/Scene/CharacterControllerEditing.cpp
    src/Scene/PhysicsPresentation.cpp
    src/Engine/PhysicsSimulationController.cpp
    src/Engine/EditorPhysicsSimulation.cpp
    src/Engine/PackagedPhysicsSimulation.cpp
    src/Engine/CharacterDriveScheduling.cpp
    src/Editor/PhysicsSimulationControls.cpp
    src/Scene/FileSceneAssetCatalog.cpp
    src/Scene/SceneMetadataPreparation.cpp
    src/Scene/MeshResidency.cpp
    src/Scene/CookedMeshCatalogPreparation.cpp
    src/Editor/MaterialLocate.cpp
    src/Editor/SceneObjectDuplication.cpp
    src/Editor/SceneObjectDuplicationControls.cpp
    src/Engine/EditorMembership.cpp
    src/Engine/EditorPersonalSettings.cpp
    src/Engine/EditorSceneLifecycle.cpp
    src/Engine/EditorProjectSettings.cpp
    src/Platform/Windows/EditorFileDialogs.cpp
    src/Scene/SceneMembershipPublication.cpp
    src/Scene/MeshResidencyPublication.cpp
    src/Scene/SceneMaterialBindingPublication.cpp
    src/Engine/ViewportMaterialAssignment.cpp
    src/DemoContent/FileBackedDemoScene.cpp
    src/assets/AssetID.cpp
    src/assets/AssetRecord.cpp
    src/assets/ProjectAssetRegistry.cpp
    src/assets/AssetRegistrySerializer.cpp
    src/assets/ProjectAssetResolver.cpp
    src/assets/MaterialAssetSerializer.cpp
    src/assets/ProjectTextureImportService.cpp
    src/assets/PrimitiveMeshAssets.cpp
    src/Editor/ScenePrimitiveCreation.cpp
    src/assets/ProjectMeshImportService.cpp
    src/assets/ProjectMeshCPUCache.cpp
    src/assets/ProjectMaterialAssetService.cpp
)

target_compile_definitions(EngineCore PRIVATE
    ENGINE_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
)
# Local regression code stays outside the production object library and is opt-in.
set(ENGINE_VERIFICATION_AVAILABLE OFF)
if(ENGINE_BUILD_DIAGNOSTICS)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/Engine/tests/PackagedBootstrapVerification.cpp" OR
       NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/tests/CookedMeshVerification.cpp")
        message(FATAL_ERROR "ENGINE_BUILD_DIAGNOSTICS requires the local Git-ignored test sources.")
    endif()

    set(ENGINE_VERIFICATION_AVAILABLE ON)
    # A consistent definition across core, tests and entry points guards test-only access.
    target_compile_definitions(EngineCore PUBLIC $<$<CONFIG:Debug>:ENGINE_HAS_VERIFICATION=1>)
    add_library(EngineVerification OBJECT)
    target_link_libraries(EngineVerification PRIVATE EngineCore)
    target_compile_definitions(EngineVerification PRIVATE
        ENGINE_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
        ENGINE_RUNTIME_DEPLOY_DIR="$<TARGET_FILE_DIR:EngineRuntime>"
    )
    target_sources(EngineVerification PRIVATE
        $<$<CONFIG:Debug>:src/Engine/tests/SceneEnvironmentVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/BuildGameVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/CookedMeshGPUVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/ArchiveCookedMeshVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/CookedSceneArchiveVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/PackagedSurfaceVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/CookedMeshLifecycleVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/MaterialAdmissionVerification.cpp>
        $<$<CONFIG:Debug>:src/assets/tests/CookedMeshVerification.cpp>
        $<$<CONFIG:Debug>:src/assets/tests/DevelopmentCookedMeshProviderVerification.cpp>
        $<$<CONFIG:Debug>:src/assets/tests/CookedMeshPreparationVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/MaterialLocateVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/ViewportPanVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/SceneDuplicationVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/ScenePhysicsDataVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/PrimitiveCreationVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/ColliderVisualizationVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/MarqueeSelectionVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/ScenePhysicsPersistenceVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/PresentationTransformVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/SceneMembershipVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/MaterialBindingVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/ViewportMaterialDropVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/PackagedBootstrapVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/PortableEditorVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/SceneHierarchyVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/DescriptorRecoveryVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/ThumbnailSchedulingVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/MeshImportUnitsVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/NormalMapVerification.cpp>
    )
endif()

# Object sharing also retains static scene-registration units for editor startup.
# Both executables use one implementation; only the entry point selects capabilities.
add_executable(Engine src/main.cpp)
target_link_libraries(Engine PRIVATE EngineCore)

if(ENGINE_VERIFICATION_AVAILABLE)
    target_link_libraries(Engine PRIVATE $<$<CONFIG:Debug>:EngineVerification>)
endif()
add_executable(EngineRuntime WIN32 src/runtime_main.cpp)
target_link_libraries(EngineRuntime PRIVATE EngineCore shell32)
if(ENGINE_VERIFICATION_AVAILABLE)
    target_link_libraries(EngineRuntime PRIVATE $<$<CONFIG:Debug>:EngineVerification>)
endif()
add_dependencies(Engine EngineRuntime)

if(ENGINE_BUILD_DIAGNOSTICS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/Engine/tests/AS2ApplicationSmokeTests.cpp")
    add_executable(AS2ApplicationSmokeTests src/Engine/tests/AS2ApplicationSmokeTests.cpp)
    target_link_libraries(AS2ApplicationSmokeTests PRIVATE EngineCore)
    if(ENGINE_VERIFICATION_AVAILABLE)
        target_link_libraries(AS2ApplicationSmokeTests PRIVATE $<$<CONFIG:Debug>:EngineVerification>)
    endif()
endif()
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
        EnginePhysics
        EngineScripting
        EngineScriptAssets
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

include(cmake/EnginePortable.cmake)

foreach(application IN ITEMS Engine EngineRuntime)
    add_custom_command(TARGET ${application} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/AngelScriptNotices.txt" "$<TARGET_FILE_DIR:${application}>/AngelScriptNotices.txt"
        VERBATIM
    )
endforeach()
