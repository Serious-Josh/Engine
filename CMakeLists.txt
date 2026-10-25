cmake_minimum_required(VERSION 3.25)

project(Engine
    VERSION 0.1.0
    LANGUAGES CXX
)

# Editor SVG decoding uses ID2D1DeviceContext5 (Windows 10 Creators Update).
if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION AND
   CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION VERSION_LESS "10.0.15063.0")
    message(FATAL_ERROR "Editor SVG icons require Windows SDK 10.0.15063.0 or newer.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENGINE_BUILD_DIAGNOSTICS "Build local regression diagnostics (Debug only)" OFF)

# NVIDIA RTXDI - required rendering dependency.
# Kept outside the Engine repository under NVIDIA's own license.
set(ENGINE_RTXDI_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../RTXDI-Library"
    CACHE PATH "Path to NVIDIA RTXDI-Library"
)

if(NOT EXISTS "${ENGINE_RTXDI_ROOT}/CMakeLists.txt"
   OR NOT EXISTS "${ENGINE_RTXDI_ROOT}/Include/Rtxdi/DI/ReSTIRDI.h")
    message(FATAL_ERROR
        "RTXDI-Library not found at '${ENGINE_RTXDI_ROOT}'. "
        "Set ENGINE_RTXDI_ROOT to your RTXDI-Library checkout."
    )
endif()

add_subdirectory(
    "${ENGINE_RTXDI_ROOT}"
    "${CMAKE_CURRENT_BINARY_DIR}/RTXDI"
)

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

include(cmake/AgilitySDK.cmake)
include(cmake/DXC.cmake)
include(cmake/DLSS.cmake)

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

# RTXPT.1A.1 offline generator for the bundled GGX single-scatter complement tables
# (assets/MaterialTables). Off by default: normal builds neither need nor run it.
option(ENGINE_BUILD_MATERIAL_TABLE_GENERATOR "Build the offline material table generator" OFF)

if(ENGINE_BUILD_MATERIAL_TABLE_GENERATOR)
    add_executable(MaterialTableGenerator
        src/Tools/MaterialTableGenerator/main.cpp
        src/Tools/MaterialTableGenerator/MaterialTableGeneration.cpp
        src/Renderer/MaterialTables/MaterialTableFile.cpp)
    target_include_directories(MaterialTableGenerator PRIVATE src)
    target_link_libraries(MaterialTableGenerator PRIVATE bcrypt)
endif()

if(ENGINE_BUILD_DIAGNOSTICS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/Tools/MaterialTableGenerator/tests/MaterialTableTests.cpp")
    add_executable(MaterialTableTests
        src/Tools/MaterialTableGenerator/tests/MaterialTableTests.cpp
        src/Tools/MaterialTableGenerator/MaterialTableGeneration.cpp
        src/Renderer/MaterialTables/MaterialTableFile.cpp)
    target_include_directories(MaterialTableTests PRIVATE src)
    target_link_libraries(MaterialTableTests PRIVATE bcrypt)
    target_compile_definitions(MaterialTableTests PRIVATE ENGINE_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
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
    src/Renderer/Upscaling/DlssBackend.cpp
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
    src/Renderer/Shader/ShaderLibraryCompileWorker.cpp
    src/Renderer/LightingModeTransition.cpp
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
    src/Renderer/Passes/PathTraceReferencePass.cpp
    src/Renderer/Passes/PathTraceSpatialPass.cpp
    src/Renderer/Passes/PathTraceTemporalPass.cpp
    src/Renderer/RadianceCache/RadianceCache.cpp
    src/Renderer/PreviousGBufferHistory.cpp
    src/Renderer/Rtxdi/RtxdiLightBridge.cpp
    src/Renderer/Rtxdi/RtxdiResources.cpp
    src/Renderer/MaterialTables/MaterialTableFile.cpp
    src/Renderer/MaterialTables/MaterialTableResources.cpp
    src/Renderer/Passes/ShadowDenoisePass.cpp
    src/Renderer/ComputePipeline.cpp
    src/Editor/Editor.cpp
    src/Editor/EditorPreferences.cpp
    src/Editor/ProjectBrowser.cpp
    src/Editor/ProjectSettingsWindow.cpp
    src/Editor/TriggerColliderControls.cpp
    src/Editor/CharacterControllerControls.cpp
    src/Editor/CameraControls.cpp
    src/Editor/LightControls.cpp
    src/Editor/CharacterControllerDiagnosticsControls.cpp
    src/Editor/ViewportMaterialDrop.cpp
    src/Editor/EditorWorkspace.cpp
    src/Editor/ViewportMarquee.cpp
    src/Editor/ViewportMarqueeControls.cpp
    src/Editor/SceneHierarchyPanel.cpp
    src/Editor/EditorIcons.cpp
    src/Editor/EditorViewportIcons.cpp
    src/Editor/SvgIconRasterizer.cpp
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
    src/Renderer/Passes/NrdPathTracePreparePass.cpp
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
    src/Renderer/FrameLighting.cpp
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
    src/Scene/RenderView.cpp
    src/Scene/SceneLighting.cpp
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
    src/Scene/CameraFrustumOverlay.cpp
    src/Scene/SceneLightCapture.cpp
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
    # The scene-hierarchy verification unit exceeds the default COFF section limit (C1128).
    if(MSVC)
        target_compile_options(EngineVerification PRIVATE /bigobj)
    endif()
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
        $<$<CONFIG:Debug>:src/Engine/tests/ResidencyBatchVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/MaterialBindingVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/ViewportMaterialDropVerification.cpp>
        $<$<CONFIG:Debug>:src/Engine/tests/PackagedBootstrapVerification.cpp>
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
engine_use_agility(Engine)

if(ENGINE_VERIFICATION_AVAILABLE)
    target_link_libraries(Engine PRIVATE $<$<CONFIG:Debug>:EngineVerification>)
endif()
add_executable(EngineRuntime WIN32 src/runtime_main.cpp)
target_link_libraries(EngineRuntime PRIVATE EngineCore shell32)
engine_use_agility(EngineRuntime)
if(ENGINE_VERIFICATION_AVAILABLE)
    target_link_libraries(EngineRuntime PRIVATE $<$<CONFIG:Debug>:EngineVerification>)
endif()
add_dependencies(Engine EngineRuntime)

if(ENGINE_BUILD_DIAGNOSTICS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/Engine/tests/AS2ApplicationSmokeTests.cpp")
    add_executable(AS2ApplicationSmokeTests src/Engine/tests/AS2ApplicationSmokeTests.cpp)
    target_link_libraries(AS2ApplicationSmokeTests PRIVATE EngineCore)
    engine_use_agility(AS2ApplicationSmokeTests)
    if(ENGINE_VERIFICATION_AVAILABLE)
        target_link_libraries(AS2ApplicationSmokeTests PRIVATE $<$<CONFIG:Debug>:EngineVerification>)
    endif()
endif()
target_compile_definitions(EngineCore PRIVATE ENGINE_RUNTIME_DEPLOY_DIR="$<TARGET_FILE_DIR:EngineRuntime>")

# Local-only Camera data/persistence regression; clean clones do not need test sources.
if(ENGINE_BUILD_DIAGNOSTICS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/Engine/tests/CameraDataTests.cpp")
    add_executable(CameraDataTests src/Engine/tests/CameraDataTests.cpp)
    target_link_libraries(CameraDataTests PRIVATE EngineCore)

    if(ENGINE_VERIFICATION_AVAILABLE)
        target_link_libraries(CameraDataTests PRIVATE $<$<CONFIG:Debug>:EngineVerification>)
    endif()
endif()

# Local-only LIGHT.1 data/persistence/lighting/wireframe regression, same guard as CameraDataTests.
if(ENGINE_BUILD_DIAGNOSTICS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/Engine/tests/LightDataTests.cpp")
    add_executable(LightDataTests src/Engine/tests/LightDataTests.cpp)
    target_link_libraries(LightDataTests PRIVATE EngineCore)

    if(ENGINE_VERIFICATION_AVAILABLE)
        target_link_libraries(LightDataTests PRIVATE $<$<CONFIG:Debug>:EngineVerification>)
    endif()
endif()
# Local-only RTXPT.3 Batch 1 RTXDI foundation verification (headless D3D12), same guard as LightDataTests.
if(ENGINE_BUILD_DIAGNOSTICS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/Renderer/tests/RtxdiFoundationTests.cpp")
    add_executable(RtxdiFoundationTests src/Renderer/tests/RtxdiFoundationTests.cpp)
    target_link_libraries(RtxdiFoundationTests PRIVATE EngineCore)
    engine_use_agility(RtxdiFoundationTests)
    target_compile_definitions(RtxdiFoundationTests PRIVATE ENGINE_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
        ENGINE_RTXDI_INCLUDE="${ENGINE_RTXDI_ROOT}/Include")
endif()
# Local-only path-trace DXR library reuse regression (headless D3D12), same guard.
if(ENGINE_BUILD_DIAGNOSTICS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/Renderer/tests/PathTraceLibraryReuseTests.cpp")
    add_executable(PathTraceLibraryReuseTests src/Renderer/tests/PathTraceLibraryReuseTests.cpp)
    target_link_libraries(PathTraceLibraryReuseTests PRIVATE EngineCore)
    engine_use_agility(PathTraceLibraryReuseTests)
endif()
# Local-only async DXR preparation Gate 1 compile-worker regression (headless DXC), same guard.
if(ENGINE_BUILD_DIAGNOSTICS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/Renderer/tests/ShaderLibraryCompileWorkerTests.cpp")
    add_executable(ShaderLibraryCompileWorkerTests src/Renderer/tests/ShaderLibraryCompileWorkerTests.cpp)
    target_link_libraries(ShaderLibraryCompileWorkerTests PRIVATE EngineCore)
    engine_use_agility(ShaderLibraryCompileWorkerTests)
endif()
# Local-only async DXR preparation Gate 2 regressions: transition logic (pure) and pass preparation
# (headless D3D12), same guard.
if(ENGINE_BUILD_DIAGNOSTICS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/Renderer/tests/LightingModeTransitionTests.cpp")
    add_executable(LightingModeTransitionTests src/Renderer/tests/LightingModeTransitionTests.cpp)
    target_link_libraries(LightingModeTransitionTests PRIVATE EngineCore)
    engine_use_agility(LightingModeTransitionTests)
endif()
if(ENGINE_BUILD_DIAGNOSTICS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/Renderer/tests/PathTraceAsyncPreparationTests.cpp")
    add_executable(PathTraceAsyncPreparationTests src/Renderer/tests/PathTraceAsyncPreparationTests.cpp)
    target_link_libraries(PathTraceAsyncPreparationTests PRIVATE EngineCore)
    engine_use_agility(PathTraceAsyncPreparationTests)
endif()
# Local-only async DXR preparation Gate 3 Editor presentation regression (pure logic), same guard.
if(ENGINE_BUILD_DIAGNOSTICS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/Debug/tests/LightingModeTransitionPresentationTests.cpp")
    add_executable(LightingModeTransitionPresentationTests src/Debug/tests/LightingModeTransitionPresentationTests.cpp)
    target_link_libraries(LightingModeTransitionPresentationTests PRIVATE EngineCore)
    engine_use_agility(LightingModeTransitionPresentationTests)
endif()
# Local-only Phase 0 residency candidate allocator-wait regression (headless D3D12), same guard.
if(ENGINE_BUILD_DIAGNOSTICS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/Renderer/tests/CommandContextFrameFenceTests.cpp")
    add_executable(CommandContextFrameFenceTests src/Renderer/tests/CommandContextFrameFenceTests.cpp)
    target_link_libraries(CommandContextFrameFenceTests PRIVATE EngineCore)
    engine_use_agility(CommandContextFrameFenceTests)
endif()
# Local-only Phase 1 shared residency validation regression (headless D3D12), same guard.
if(ENGINE_BUILD_DIAGNOSTICS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/Scene/tests/ResidencyValidationPassTests.cpp")
    add_executable(ResidencyValidationPassTests src/Scene/tests/ResidencyValidationPassTests.cpp)
    target_link_libraries(ResidencyValidationPassTests PRIVATE EngineCore)
    engine_use_agility(ResidencyValidationPassTests)
    target_compile_definitions(ResidencyValidationPassTests PRIVATE ENGINE_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
endif()
# Local-only headless Build Game Agility deployment and ownership regression, same guard.
if(ENGINE_BUILD_DIAGNOSTICS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/Engine/tests/BuildGameDeploymentTests.cpp")
    add_executable(BuildGameDeploymentTests src/Engine/tests/BuildGameDeploymentTests.cpp)
    target_link_libraries(BuildGameDeploymentTests PRIVATE EngineCore)
endif()
set_target_properties(EngineRuntime PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Game/$<CONFIG>"
)

# Runtime deployment contains binaries only. Archives are supplied by the offline cook.
add_custom_command(TARGET EngineRuntime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/external/NRD/Lib/$<CONFIG>/NRD.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/external/NRI/Lib/$<CONFIG>/NRI.dll"
        $<TARGET_FILE:assimp>
        $<TARGET_FILE:DirectXTex>
        $<TARGET_FILE_DIR:EngineRuntime>
    COMMENT "Copying packaged runtime DLL dependencies (no development assets)"
    VERBATIM
)
engine_deploy_dxc(EngineRuntime)

add_custom_command(
    TARGET Engine
    POST_BUILD

    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:Engine>/assets"

    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:Engine>/assets"

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

    # Deploy NVIDIA RTXDI shader headers for runtime DXC compilation.
    # The SDK remains outside the Engine source repository.
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${ENGINE_RTXDI_ROOT}/Include/Rtxdi"
        "$<TARGET_FILE_DIR:Engine>/assets/Shaders/Rtxdi"

    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:assimp>
        $<TARGET_FILE_DIR:Engine>

    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:DirectXTex>
        $<TARGET_FILE_DIR:Engine>

    COMMENT "Copying runtime assets and dependencies"
)
engine_deploy_dxc(Engine)
# The Editor initializes DLSS; the packaged runtime does not yet, so it gets no DLSS runtime.
engine_deploy_dlss(Engine)

# Agility SDK D3D12 headers take precedence over the Windows SDK copies.
target_include_directories(EngineCore BEFORE PUBLIC "${ENGINE_AGILITY_SDK_INCLUDE_DIR}")
engine_configure_dlss(EngineCore)
target_compile_definitions(EngineCore PRIVATE ENGINE_AGILITY_SDK_VERSION=${ENGINE_AGILITY_SDK_VERSION}
    ENGINE_AGILITY_SDK_PACKAGE_VERSION="${ENGINE_AGILITY_SDK_PACKAGE_VERSION}")

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
        Rtxdi
        shell32
        ole32
        bcrypt
        dxgi
        d3d12
        d2d1
        windowscodecs
        EngineDXC

        "$<$<CONFIG:Debug>:${CMAKE_CURRENT_SOURCE_DIR}/external/NRD/Lib/Debug/NRD.lib>"
        "$<$<CONFIG:Release>:${CMAKE_CURRENT_SOURCE_DIR}/external/NRD/Lib/Release/NRD.lib>"

        "$<$<CONFIG:Debug>:${CMAKE_CURRENT_SOURCE_DIR}/external/NRI/Lib/Debug/NRI.lib>"
        "$<$<CONFIG:Release>:${CMAKE_CURRENT_SOURCE_DIR}/external/NRI/Lib/Release/NRI.lib>"

        assimp
        DirectXTex
        nlohmann_json
)

include(cmake/RuntimeNotices.cmake)

foreach(application IN ITEMS Engine EngineRuntime)
    add_custom_command(TARGET ${application} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/AngelScriptNotices.txt" "$<TARGET_FILE_DIR:${application}>/AngelScriptNotices.txt"
        VERBATIM
    )
endforeach()

# MSVC compiles the C++ sources of each Engine-owned target in parallel (/MP12; MSBuild -m still runs
# targets side by side). Only targets defined in Engine's own directories are changed: FetchContent
# dependencies and RTXDI live in their own directories, and the AngelScript add-on targets compile
# third-party sources. /MP changes scheduling only, never generated code.
set(engine_serial_compile_targets EngineScriptString EngineScriptBuilder)

foreach(engine_directory IN ITEMS "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src/Physics")
    get_property(engine_directory_targets DIRECTORY "${engine_directory}" PROPERTY BUILDSYSTEM_TARGETS)

    foreach(engine_target IN LISTS engine_directory_targets)
        get_target_property(engine_target_type ${engine_target} TYPE)
        set(engine_compiled_types EXECUTABLE STATIC_LIBRARY SHARED_LIBRARY MODULE_LIBRARY OBJECT_LIBRARY)

        if(engine_target_type IN_LIST engine_compiled_types AND NOT engine_target IN_LIST engine_serial_compile_targets)
            target_compile_options(${engine_target} PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/MP12>)
        endif()
    endforeach()
endforeach()
