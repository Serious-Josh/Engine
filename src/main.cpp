#include "Engine/Engine.h"
#include "Engine/PackagedBootstrap.h"
#include "Core/Log.h"
#include "Platform/Windows/CommandLineArguments.h"
#include "Platform/Windows/WindowsText.h"
#include "Project/EditorCommandLine.h"
#include "Project/ProjectLoader.h"

#include <Windows.h>

#include <cstdlib>
#include <string_view>
#include <filesystem>
#include <exception>

// Verification diagnostics live in Git-ignored tests folders. CMake defines
// ENGINE_HAS_VERIFICATION only with explicit diagnostics opt-in; Debug builds alone still
// do not imply that the --verify-* entry points exist.
#if !defined(NDEBUG) && defined(ENGINE_HAS_VERIFICATION)
#define ENGINE_VERIFICATION_ENABLED 1
#else
#define ENGINE_VERIFICATION_ENABLED 0
#endif

#ifndef NDEBUG
int CookProjectMeshes(const std::filesystem::path& projectRoot);
#endif

#if ENGINE_VERIFICATION_ENABLED
int VerifySceneHierarchy();
int VerifyProjectIndependentStartup();
int VerifyEditorPreferences();
int VerifyProjectBrowser();
int VerifySceneLifecycle();
int VerifyAssetBrowserScenes();
int VerifyStartupScene(const std::filesystem::path& outputRoot);
int VerifyCausticsVisible(const std::filesystem::path& project, const std::filesystem::path& out);
int VerifyGamePlayGPU(const std::filesystem::path& projectDirectory, const std::filesystem::path& scene);
int VerifyPhysicsPerformance(const std::filesystem::path& projectDirectory, const std::filesystem::path& scene);
int VerifyScriptWorkflow(const std::filesystem::path& projectDirectory, const std::filesystem::path& scene,
    const std::vector<std::string>& objectNames);
int VerifyGameplayAudio(const std::filesystem::path& projectDirectory, const std::filesystem::path& scene,
    const std::filesystem::path& buildOutput);
int VerifySceneAudio(const std::filesystem::path& projectDirectory, const std::filesystem::path& baseScene,
    const std::filesystem::path& buildOutput);
int VerifyAcousticPlay(const std::filesystem::path& projectDirectory, const std::filesystem::path& baseScene);
int VerifyEditorAudio(const std::filesystem::path& projectDirectory, const std::filesystem::path& scene,
    const std::filesystem::path& buildOutput);
int VerifyScriptAudio(const std::filesystem::path& projectDirectory, const std::filesystem::path& scene,
    const std::filesystem::path& buildOutput);
int MigrateSceneLighting(const std::filesystem::path& scene, float referenceEV100, const std::filesystem::path& report);
int VerifyProjectSystemWorkflow(const std::string& stage, const std::filesystem::path& workRoot);
int VerifyProjectRelaunch(const std::filesystem::path& source, const std::filesystem::path& target, const std::string& mode);
int VerifyProjectWorkflowRegressions();
int VerifyMeshImportUnits(const std::filesystem::path& root);
int VerifyCookedMesh();
int VerifyScenePhysicsData();
int VerifyPrimitiveCreation();
int VerifyColliderVisualization();
int VerifyViewportMarquee();
int VerifyScenePhysicsPersistence();
int VerifyColliderScaleMigration();
int VerifyVoxelEditor(const std::filesystem::path& work);
int VerifyVoxelOverlay(const std::filesystem::path& work);
int VerifyVoxelResidency();
int VerifyVoxelQueries();
int VerifyVoxelStreaming(const std::filesystem::path& work);
int VerifyAirField();
int VerifyAirSolver();
int VerifyAirMotors();
int VerifyAirMotorEditor(const std::filesystem::path& work);
int VerifyAirVisualization();
int VerifyAirObstacles(const std::filesystem::path& work);
int VerifyAirPieces();
int VerifyAirPieceSolver();
int VerifyAirMist();
int VerifyPresentationTransforms();
int VerifyPhysicsSimulationController();
int VerifyEditorPhysicsIntegration();
int VerifyPackagedPhysicsScheduling();
int VerifyPackagedPhysicsStartupFailure(const std::filesystem::path& game);
int VerifyDevelopmentCookedMeshProvider(const std::filesystem::path& projectRoot);
int VerifyCookedMeshPreparation(const std::filesystem::path& projectRoot);
#endif

namespace
{
    // Startup failures go to the log (console). When Explorer launched the Editor, the console
    // is owned by this process alone and vanishes on exit, so the error is also shown in a
    // message box. A shell launch shares its console and gets only the log line.
    void ReportStartupFailure(const std::string& message)
    {
        Log::Error(message);

        DWORD consoleProcesses[2]{};
        const DWORD attachedProcesses = GetConsoleProcessList(consoleProcesses, 2);

        if(attachedProcesses != 1)
        {
            return;
        }

        std::wstring text;

        try
        {
            text = WindowsText::FromUTF8(message);
        }
        catch(...)
        {
            text = L"The Engine Editor could not open the requested project. See the console log for details.";
        }

        MessageBoxW(nullptr, text.c_str(), L"Engine Editor", MB_OK | MB_ICONERROR | MB_TASKMODAL);
    }

    // Classified from UTF-16 before any option dispatch. Developer options keep using the
    // narrow argv below exactly as before; only a project path needs the UTF-16 form.
    bool ReadEditorCommandLine(EditorCommandLine& commandLine)
    {
        try
        {
            commandLine = EditorCommandLine::Parse(CommandLineArguments::Read());
        }
        catch(const std::exception& error)
        {
            ReportStartupFailure(error.what());
            return false;
        }

        if(commandLine.kind == EditorCommandLine::Kind::Invalid)
        {
            ReportStartupFailure(commandLine.errorMessage);
            return false;
        }

        return true;
    }
}

int main(int argc, char** argv)
{
    EditorCommandLine commandLine;

    if(!ReadEditorCommandLine(commandLine))
    {
        return 1;
    }

#if ENGINE_VERIFICATION_ENABLED
    if(argc == 2 && std::string_view(argv[1]) == "--verify-normal-maps")
    {
        auto engine = std::make_unique<Engine>();
        bool success = false;

        try
        {
            success = engine->Initialize() && engine->VerifyNormalMaps();
        }
        catch(const std::exception& error)
        {
            Log::Error(std::string("B.4 normal diagnostic: ") + error.what());
        }

        engine->Shutdown();
        return success ? 0 : 1;
    }

    if(argc == 3 && std::string_view(argv[1]) == "--verify-mesh-import-units")
    {
        return VerifyMeshImportUnits(std::filesystem::absolute(argv[2]));
    }

    if(argc == 3 && std::string_view(argv[1]) == "--verify-mesh-import-units-gpu")
    {
        auto engine = std::make_unique<Engine>();
        bool success = false;

        try
        {
            success = engine->Initialize() && engine->VerifyMeshImportUnitsGPU(argv[2]);
        }
        catch(const std::exception& error)
        {
            Log::Error(std::string("B.3 GPU diagnostic: ") + error.what());
        }

        engine->Shutdown();
        return success ? 0 : 1;
    }

    if(argc == 2 && std::string_view(argv[1]) == "--verify-scene-hierarchy")
    {
        return VerifySceneHierarchy();
    }
#endif
#if !ENGINE_VERIFICATION_ENABLED
    if(argc >= 2 && (std::string_view(argv[1]).starts_with("--verify-") ||
        std::string_view(argv[1]) == "--activate-scene-archive"))
    {
        Log::Error("Verification requires a Debug build with ENGINE_BUILD_DIAGNOSTICS=ON and local test sources.");
        return 1;
    }
#endif
#if ENGINE_VERIFICATION_ENABLED
    if(argc == 3 && std::string_view(argv[1]) == "--verify-packaged-physics-startup-failure")
    {
        return VerifyPackagedPhysicsStartupFailure(std::filesystem::absolute(argv[2]));
    }
    if(argc == 2 && std::string_view(argv[1]) == "--verify-packaged-physics")
    {
        return VerifyPackagedPhysicsScheduling();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--verify-viewport-marquee")
    {
        return VerifyViewportMarquee();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--verify-collider-visualization")
    {
        return VerifyColliderVisualization();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--verify-primitive-creation")
    {
        return VerifyPrimitiveCreation();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--verify-editor-physics")
    {
        return VerifyEditorPhysicsIntegration();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--verify-physics-simulation")
    {
        return VerifyPhysicsSimulationController();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--verify-presentation-transforms")
    {
        return VerifyPresentationTransforms();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--verify-scene-physics-persistence")
    {
        return VerifyScenePhysicsPersistence();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--verify-scene-physics-data")
    {
        return VerifyScenePhysicsData();
    }
    if(argc == 2 && std::string_view(argv[1]) == "--verify-collider-scale-migration")
    {
        return VerifyColliderScaleMigration();
    }
    // VX.4C.1: <empty work directory>; creates a scratch project there.
    if(argc == 3 && std::string_view(argv[1]) == "--verify-voxel-editor")
    {
        return VerifyVoxelEditor(std::filesystem::absolute(argv[2]));
    }
    // AIR.4A: the velocity-vector geometry against a CPU reference.
    if(argc == 2 && std::string_view(argv[1]) == "--verify-air-visualization")
    {
        return VerifyAirVisualization();
    }

    if(argc == 2 && std::string_view(argv[1]) == "--verify-air-pieces")
    {
        return VerifyAirPieces();
    }

    // AIR.4D D2: the piece-aware solver against its CPU reference, rebuild remapping and cost.
    if(argc == 2 && std::string_view(argv[1]) == "--verify-air-piece-solver")
    {
        return VerifyAirPieceSolver();
    }
    // AIR.4F F1: passive mist transport against its CPU reference, isolation, window behaviour and cost.
    if(argc == 2 && std::string_view(argv[1]) == "--verify-air-mist")
    {
        return VerifyAirMist();
    }
    // AIR.3: Air Motors through the Editor and Engine frames; <empty work directory>, a scratch project is created there.
    if(argc == 3 && std::string_view(argv[1]) == "--verify-air-motor-editor")
    {
        return VerifyAirMotorEditor(std::filesystem::absolute(argv[2]));
    }

    if(argc == 3 && std::string_view(argv[1]) == "--verify-air-obstacles")
    {
        return VerifyAirObstacles(std::filesystem::absolute(argv[2]));
    }
    // AIR.3: Air Motors in the solver against a CPU reference.
    if(argc == 2 && std::string_view(argv[1]) == "--verify-air-motors")
    {
        return VerifyAirMotors();
    }
    // AIR.2: the velocity solver against a CPU fp32 reference.
    if(argc == 2 && std::string_view(argv[1]) == "--verify-air-solver")
    {
        return VerifyAirSolver();
    }
    // AIR.1: the air field's grid, recentring and obstacle mask against CPU references.
    if(argc == 2 && std::string_view(argv[1]) == "--verify-air-field")
    {
        return VerifyAirField();
    }
    // VX.5: camera-centred residency through Engine frames; <empty work directory>, a scratch project is created there.
    if(argc == 3 && std::string_view(argv[1]) == "--verify-voxel-streaming")
    {
        return VerifyVoxelStreaming(std::filesystem::absolute(argv[2]));
    }
    // VX.5: Voxel.hlsli queries on the GPU against CPU references.
    if(argc == 2 && std::string_view(argv[1]) == "--verify-voxel-queries")
    {
        return VerifyVoxelQueries();
    }
    // VX.5: GPU voxel residency resources against GPU readback.
    if(argc == 2 && std::string_view(argv[1]) == "--verify-voxel-residency")
    {
        return VerifyVoxelResidency();
    }
    // VX.4C.2: <empty work directory>; creates a scratch project there.
    if(argc == 3 && std::string_view(argv[1]) == "--verify-voxel-overlay")
    {
        return VerifyVoxelOverlay(std::filesystem::absolute(argv[2]));
    }
#endif

    if(argc>=2 && std::string_view(argv[1])=="--package-bootstrap")
    {
        if(argc!=7) { Log::Error("Usage: --package-bootstrap <input.toc> <input.eap> <output.toc> <output.eap> <authoring-shader-root>"); return 1; }
        try { PackagedBootstrap::Cook(argv[2],argv[3],argv[4],argv[5],argv[6]); return 0; }
        catch(const std::exception& e) { Log::Error(std::string("Offline bootstrap cook: ")+e.what()); return 1; }
    }
    const bool packaged=argc>=2 && (std::string_view(argv[1])=="--packaged-runtime" || std::string_view(argv[1])=="--verify-packaged-bootstrap");
    if(packaged)
    {
        if(argc!=5 || !AssetID::Parse(argv[4])) { Log::Error("Usage: --packaged-runtime|--verify-packaged-bootstrap <toc> <eap> <scene-uuid>"); return 1; }
        auto packagedEngine=std::make_unique<Engine>();
        auto& engine=*packagedEngine; EngineStartup startup; startup.mode=EngineStartup::Mode::PackagedRuntime;
        startup.toc=argv[2]; startup.pak=argv[3]; startup.sceneID=AssetID::Parse(argv[4]);
        bool success=false;
        try
        {
            success=engine.Initialize(startup);
            if(success)
            {
#if ENGINE_VERIFICATION_ENABLED
                if(std::string_view(argv[1]) == "--verify-packaged-bootstrap")
                {
                    engine.VerifyPackagedBootstrap();
                }
                else if(std::getenv("ENGINE_DIAG_PACKAGED_PROBES"))
                {
                    engine.VerifyPackagedProbes();
                }
                else if(std::getenv("ENGINE_DIAG_PACKAGED_PATHING"))
                {
                    engine.VerifyPackagedPathing();
                }
                else if(std::getenv("ENGINE_DIAG_PACKAGED_REFLECTIONS"))
                {
                    engine.VerifyPackagedReflections();
                }
                else
#endif
                {
                    engine.Run();
                }
            }
        }
        catch(const std::exception& e) { Log::Error(std::string("Packaged bootstrap: ")+e.what()); success=false; }
        engine.Shutdown(); return success ? 0 : 1;
    }
#if ENGINE_VERIFICATION_ENABLED
    if(argc>=2 && std::string_view(argv[1])=="--verify-scene-hdri")
    {
        if(argc!=4) { Log::Error("Usage: --verify-scene-hdri <private-scene-copy> <output-directory>"); return 1; }
        auto owned=std::make_unique<Engine>(); bool success=false;
        try { success=owned->Initialize(); if(success) success=owned->VerifySceneHDRI(std::filesystem::absolute(argv[2]),std::filesystem::absolute(argv[3])); }
        catch(const std::exception& e) { Log::Error(std::string("Scene HDRI diagnostic: ")+e.what()); success=false; }
        owned->Shutdown(); return success ? 0 : 1;
    }
    if(argc==2 && std::string_view(argv[1])=="--verify-viewport-pan")
    {
        auto engine=std::make_unique<Engine>(); bool success=false;
        try { success=engine->Initialize(); if(success) success=engine->VerifyViewportPan(); }
        catch(const std::exception& e) { Log::Error(std::string("Viewport pan diagnostic: ")+e.what()); success=false; }
        engine->Shutdown(); return success ? 0 : 1;
    }
    if(argc==2 && std::string_view(argv[1])=="--verify-editor-preferences")
    {
        return VerifyEditorPreferences();
    }
    if(argc == 2 && std::string_view(argv[1]) == "--verify-project-browser")
    {
        return VerifyProjectBrowser();
    }
    if(argc == 2 && std::string_view(argv[1]) == "--verify-scene-lifecycle")
    {
        return VerifySceneLifecycle();
    }
    if(argc == 2 && std::string_view(argv[1]) == "--verify-asset-browser-scenes")
    {
        return VerifyAssetBrowserScenes();
    }
    if(argc == 4 && std::string_view(argv[1]) == "--verify-project-system-workflow")
    {
        return VerifyProjectSystemWorkflow(argv[2], CommandLineArguments::Read()[3]);
    }
    if(argc == 5 && std::string_view(argv[1]) == "--verify-project-relaunch")
    {
        const auto arguments = CommandLineArguments::Read();
        return VerifyProjectRelaunch(arguments[2], arguments[3], argv[4]);
    }
    // Physical Lighting C1: one-time conversion of a legacy-unit scene at an explicit reference exposure.
    if(argc == 5 && std::string_view(argv[1]) == "--migrate-scene-lighting")
    {
        return MigrateSceneLighting(std::filesystem::absolute(argv[2]), std::stof(argv[3]), std::filesystem::absolute(argv[4]));
    }
    if(argc == 4 && std::string_view(argv[1]) == "--verify-caustics-visible")
    {
        return VerifyCausticsVisible(std::filesystem::absolute(argv[2]), std::filesystem::absolute(argv[3]));
    }
    if(argc == 4 && std::string_view(argv[1]) == "--verify-physics-performance")
    {
        return VerifyPhysicsPerformance(std::filesystem::absolute(argv[2]), std::filesystem::absolute(argv[3]));
    }
    if(argc == 4 && std::string_view(argv[1]) == "--verify-game-play-gpu")
    {
        return VerifyGamePlayGPU(std::filesystem::absolute(argv[2]), std::filesystem::absolute(argv[3]));
    }
    if(argc >= 5 && std::string_view(argv[1]) == "--verify-script-workflow")
    {
        return VerifyScriptWorkflow(std::filesystem::absolute(argv[2]), std::filesystem::absolute(argv[3]),
            std::vector<std::string>(argv + 4, argv + argc));
    }
    if((argc == 4 || argc == 5) && std::string_view(argv[1]) == "--verify-gameplay-audio")
    {
        // Optional fifth argument (Audio.1D): Build Game output directory for the packaged acceptance run.
        return VerifyGameplayAudio(std::filesystem::absolute(argv[2]), std::filesystem::absolute(argv[3]),
            argc == 5 ? std::filesystem::absolute(argv[4]) : std::filesystem::path{});
    }
    if(argc == 4 && std::string_view(argv[1]) == "--verify-acoustic-play")
    {
        // M4: <project with Steam Audio enabled and its banks> <base scene with a Main Camera>.
        return VerifyAcousticPlay(std::filesystem::absolute(argv[2]), std::filesystem::absolute(argv[3]));
    }
    if((argc == 4 || argc == 5) && std::string_view(argv[1]) == "--verify-scene-audio")
    {
        // Audio.2A: <project> <base scene with a Main Camera> [Build Game output].
        return VerifySceneAudio(std::filesystem::absolute(argv[2]), std::filesystem::absolute(argv[3]),
            argc == 5 ? std::filesystem::absolute(argv[4]) : std::filesystem::path{});
    }
    if((argc == 4 || argc == 5) && std::string_view(argv[1]) == "--verify-editor-audio")
    {
        // Audio.2B: <project with banks and Scenes/Flat.scene> <scene to author> [Build Game output].
        return VerifyEditorAudio(std::filesystem::absolute(argv[2]), std::filesystem::absolute(argv[3]),
            argc == 5 ? std::filesystem::absolute(argv[4]) : std::filesystem::path{});
    }
    if((argc == 4 || argc == 5) && std::string_view(argv[1]) == "--verify-script-audio")
    {
        // Audio.2C: <project with banks, Scenes/Flat.scene and Content/Scripts/AudioDirector.as> <scene to author>
        // [Build Game output].
        return VerifyScriptAudio(std::filesystem::absolute(argv[2]), std::filesystem::absolute(argv[3]),
            argc == 5 ? std::filesystem::absolute(argv[4]) : std::filesystem::path{});
    }
    if(argc == 3 && std::string_view(argv[1]) == "--verify-startup-scene")
    {
        return VerifyStartupScene(std::filesystem::absolute(argv[2]));
    }
    if(argc == 2 && std::string_view(argv[1]) == "--verify-project-workflow-regressions")
    {
        return VerifyProjectWorkflowRegressions();
    }
    if(argc==2 && std::string_view(argv[1])=="--verify-project-independent-startup")
    {
        return VerifyProjectIndependentStartup();
    }
    if(argc==2 && std::string_view(argv[1])=="--verify-editor-grid")
    {
        auto engine=std::make_unique<Engine>(); bool success=false;
        try { success=engine->Initialize(); if(success) success=engine->VerifyEditorGrid(); }
        catch(const std::exception& e) { Log::Error(std::string("Editor grid diagnostic: ")+e.what()); success=false; }
        engine->Shutdown(); return success ? 0 : 1;
    }
    if(argc==2 && (std::string_view(argv[1])=="--verify-background-tasks" || std::string_view(argv[1])=="--background-tasks-demo" ||
        std::string_view(argv[1])=="--verify-dxr-background-tasks" ||
        std::string_view(argv[1])=="--verify-dxr-background-tasks-failure"))
    {
        const bool demo = std::string_view(argv[1]) == "--background-tasks-demo";
        const bool dxr = std::string_view(argv[1]) == "--verify-dxr-background-tasks";
        const bool dxrFailure = std::string_view(argv[1]) == "--verify-dxr-background-tasks-failure";
        auto engine = std::make_unique<Engine>();
        bool success = false;

        try
        {
            success = engine->Initialize();

            if(success)
            {
                success = demo ? engine->RunBackgroundTasksDemo() :
                    dxr ? engine->VerifyDxrBackgroundTasks() :
                    dxrFailure ? engine->VerifyDxrBackgroundTaskFailure() : engine->VerifyBackgroundTasks();
            }
        }
        catch(const std::exception& e)
        {
            Log::Error(std::string("Background tasks diagnostic: ") + e.what());
            success = false;
        }

        engine->Shutdown();
        return success ? 0 : 1;
    }
    if(argc==2 && std::string_view(argv[1])=="--verify-viewport-grid-snap")
    {
        auto engine=std::make_unique<Engine>(); bool success=false;
        try { success=engine->Initialize(); if(success) success=engine->VerifyViewportGridSnap(); }
        catch(const std::exception& e) { Log::Error(std::string("Viewport grid snap diagnostic: ")+e.what()); success=false; }
        engine->Shutdown(); return success ? 0 : 1;
    }
    if(argc==2 && std::string_view(argv[1])=="--verify-material-locate")
    {
        auto engine=std::make_unique<Engine>(); bool success=false;
        try { success=engine->Initialize(); if(success) success=engine->VerifyMaterialLocate(); }
        catch(const std::exception& e) { Log::Error(std::string("Material locate diagnostic: ")+e.what()); success=false; }
        engine->Shutdown(); return success ? 0 : 1;
    }
    if(argc>=2 && std::string_view(argv[1])=="--verify-scene-duplication")
    {
        if(argc!=3) { Log::Error("Usage: --verify-scene-duplication <private-fixture.scene>"); return 1; }
        auto engine=std::make_unique<Engine>(); bool success=false;
        try { success=engine->Initialize(); if(success) success=engine->VerifySceneDuplication(argv[2]); }
        catch(const std::exception& e) { Log::Error(std::string("Duplication diagnostic: ")+e.what()); success=false; }
        engine->Shutdown(); return success ? 0 : 1;
    }
    if(argc>=2 && std::string_view(argv[1])=="--verify-scene-membership")
    {
        if(argc!=3) { Log::Error("Usage: --verify-scene-membership <private-fixture-scene>"); return 1; }
        auto engine=std::make_unique<Engine>(); bool success=false;
        try { success=engine->Initialize(); if(success) success=engine->VerifySceneMembership(argv[2]); }
        catch(const std::exception& e) { Log::Error(std::string("Membership diagnostic: ")+e.what()); success=false; }
        engine->Shutdown(); return success ? 0 : 1;
    }
    if(argc == 2 && std::string_view(argv[1]) == "--verify-residency-batches")
    {
        auto engine = std::make_unique<Engine>();
        bool success = false;

        try
        {
            success = engine->Initialize() && engine->VerifyResidencyBatches();
        }
        catch(const std::exception& exception)
        {
            Log::Error(std::string("Residency batch diagnostic: ") + exception.what());
            success = false;
        }

        engine->Shutdown();
        return success ? 0 : 1;
    }
    if(argc >= 2 && std::string_view(argv[1]) == "--verify-thumbnail-scheduling")
    {
        if(argc != 3)
        {
            Log::Error("Usage: --verify-thumbnail-scheduling <isolated-fixture-project>");
            return 1;
        }

        auto engine = std::make_unique<Engine>();
        bool success = false;

        try
        {
            success = engine->Initialize() && engine->VerifyThumbnailScheduling(argv[2]);
        }
        catch(const std::exception& exception)
        {
            Log::Error(std::string("Thumbnail scheduling diagnostic: ") + exception.what());
            success = false;
        }

        engine->Shutdown();
        return success ? 0 : 1;
    }
    if(argc >= 2 && std::string_view(argv[1]) == "--verify-descriptor-recovery")
    {
        if(argc != 3)
        {
            Log::Error("Usage: --verify-descriptor-recovery <isolated-fixture-project>");
            return 1;
        }

        auto engine = std::make_unique<Engine>();
        bool success = false;

        try
        {
            success = engine->Initialize() && engine->VerifyDescriptorRecovery(argv[2]);
        }
        catch(const std::exception& exception)
        {
            Log::Error(std::string("Descriptor recovery diagnostic: ") + exception.what());
            success = false;
        }

        engine->Shutdown();
        return success ? 0 : 1;
    }
    if(argc >= 2 && std::string_view(argv[1]) == "--verify-scene-hierarchy-gpu")
    {
        if(argc != 3)
        {
            Log::Error("Usage: --verify-scene-hierarchy-gpu <schema-9-project-scene>");
            return 1;
        }

        auto engine = std::make_unique<Engine>();
        bool success = false;

        try
        {
            success = engine->Initialize() && engine->VerifySceneHierarchyGPU(argv[2]);
        }
        catch(const std::exception& exception)
        {
            Log::Error(std::string("Scene hierarchy GPU diagnostic: ") + exception.what());
            success = false;
        }

        engine->Shutdown();
        return success ? 0 : 1;
    }
    if(argc>=2 && std::string_view(argv[1])=="--verify-build-game")
    {
        if(argc!=4) { Log::Error("Usage: --verify-build-game <current-scene> <output-directory>"); return 1; }
        auto diagnosticEngine=std::make_unique<Engine>();
        auto& engine=*diagnosticEngine; bool success=false;
        try { success=engine.Initialize(); if(success) success=engine.VerifyBuildGame(argv[2],std::filesystem::absolute(argv[3])); }
        catch(const std::exception& e) { Log::Error(std::string("Build Game diagnostic: ")+e.what()); success=false; }
        engine.Shutdown(); return success ? 0 : 1;
    }
#endif
    const bool surfaces = argc >= 2 && std::string_view(argv[1]) == "--verify-archive-materials";
    if(surfaces && argc!=3) { Log::Error("Usage: --verify-archive-materials <authoring-scene>"); return 1; }
    const bool cookedScene = argc >= 2 && std::string_view(argv[1]) == "--verify-cooked-scene-archive";
    const bool activateArchive = argc >= 2 && std::string_view(argv[1]) == "--activate-scene-archive";
    if((cookedScene && argc != 3) || (activateArchive && (argc != 5 || !AssetID::Parse(argv[4]))))
    {
        Log::Error("Usage: --verify-cooked-scene-archive <authoring-scene> | --activate-scene-archive <toc> <eap> <scene-uuid>");
        return 1;
    }
    const bool archiveGPU = argc >= 2 && std::string_view(argv[1]) == "--verify-archive-cooked-publication";
    const bool cookedGPU = argc >= 2 && (std::string_view(argv[1]) == "--verify-cooked-gpu-preparation" ||
        std::string_view(argv[1]) == "--verify-cooked-publication" || archiveGPU);
    const bool visualReview = argc == 4 && std::string_view(argv[1]) == "--verify-cooked-publication" &&
        std::string_view(argv[3]) == "--visual";
    if(cookedGPU && argc != 3 && !visualReview)
    {
        Log::Error("Usage: Engine.exe --verify-cooked-gpu-preparation|--verify-cooked-publication|--verify-archive-cooked-publication <absolute-scene-path> [--visual (publication only)]");
        return 1;
    }
#if ENGINE_VERIFICATION_ENABLED
    if(argc >= 2 && std::string_view(argv[1]) == "--verify-cooked-preparation")
    {
        if(argc > 3)
        {
            Log::Error("Usage: Engine.exe --verify-cooked-preparation [project-root]");
            return 1;
        }

        return VerifyCookedMeshPreparation(argc == 3 ? std::filesystem::path(argv[2]) : std::filesystem::path{});
    }

    if(argc >= 2 && std::string_view(argv[1]) == "--verify-cooked-provider")
    {
        if(argc > 3)
        {
            Log::Error("Usage: Engine.exe --verify-cooked-provider [project-root]");
            return 1;
        }

        return VerifyDevelopmentCookedMeshProvider(argc == 3 ? std::filesystem::path(argv[2]) : std::filesystem::path{});
    }

    if(argc == 2 && std::string_view(argv[1]) == "--verify-cooked-mesh")
    {
        return VerifyCookedMesh();
    }
#endif

#ifndef NDEBUG
    if(argc >= 2 && std::string_view(argv[1]) == "--cook-meshes")
    {
        if(argc > 3)
        {
            Log::Error("Usage: Engine.exe --cook-meshes [project-root]");
            return 1;
        }

        return CookProjectMeshes(argc == 3 ? std::filesystem::path(argv[2]) : std::filesystem::path{});
    }
#else
    if(argc >= 2 && std::string_view(argv[1]) == "--cook-meshes")
    {
        Log::Error("Mesh development diagnostics require the Debug executable.");
        return 1;
    }
#endif

    // An explicit project is validated and locked before the Engine exists, so a failure creates
    // no window, GPU or project state and never falls back to the default project.
    std::optional<OpenedProject> explicitProject;

    if(commandLine.kind == EditorCommandLine::Kind::ExplicitProject)
    {
        std::string projectError;
        explicitProject = ProjectLoader::Open(commandLine.projectManifest, projectError);

        if(!explicitProject)
        {
            ReportStartupFailure("Cannot open project: " + projectError);
            return 1;
        }
    }

    auto editorEngine=std::make_unique<Engine>();
    auto& engine=*editorEngine;

    if(explicitProject)
    {
        engine.SetStartupProject(std::move(*explicitProject));
        explicitProject.reset();
    }

    // Only the ordinary interactive Editor reads and writes personal settings; the archive
    // diagnostics that share this path do not.
    const bool archiveDiagnostic = cookedScene || activateArchive || surfaces;
    EngineStartup startup;
    startup.mode = archiveDiagnostic ? EngineStartup::Mode::ArchiveDiagnostic : EngineStartup::Mode::Editor;
    startup.persistPersonalSettings = !archiveDiagnostic;
    startup.projectBrowserWorkflow = !archiveDiagnostic &&
        commandLine.kind != EditorCommandLine::Kind::DeveloperOption;

    if (!engine.Initialize(startup))
    {
        Log::Error("Engine initialization failed; aborting without automatic resource teardown because GPU completion may be unconfirmed.");
        std::abort();
    }

#if ENGINE_VERIFICATION_ENABLED
    if(argc == 2 && (std::string_view(argv[1]) == "--verify-material-bindings" ||
        std::string_view(argv[1]) == "--verify-viewport-material-drops"))
    {
        bool verified = false;
        try { verified = engine.VerifyMaterialBindingPublication(); }
        catch(const std::exception& error) { Log::Error("Material binding diagnostic: " + std::string(error.what())); }
        engine.Shutdown();
        return verified ? 0 : 1;
    }
    if(cookedScene || activateArchive || surfaces)
    {
        bool verified = false;
        try
        {
            verified = surfaces ? engine.VerifyArchiveMaterials(argv[2]) : cookedScene ? engine.VerifyCookedSceneArchive(argv[2]) :
                engine.ActivateCookedSceneArchive(argv[2],argv[3],*AssetID::Parse(argv[4]));
        }
        catch(const std::exception& error) { Log::Error("Cooked scene archive diagnostic: " + std::string(error.what())); }
        engine.Shutdown(); return verified ? 0 : 1;
    }
    if(cookedGPU)
    {
        bool verified = false;
        try
        {
            verified = archiveGPU ? engine.VerifyArchiveCookedMeshPublication(std::filesystem::path(argv[2])) :
                std::string_view(argv[1]) == "--verify-cooked-gpu-preparation" ?
                engine.VerifyCookedMeshGPUPreparation(std::filesystem::path(argv[2])) :
                engine.VerifyCookedMeshPublication(std::filesystem::path(argv[2]), visualReview);
        }
        catch(const std::exception& error) { Log::Error("Cooked GPU diagnostic exception: " + std::string(error.what())); }
        catch(...) { Log::Error("Unknown cooked GPU diagnostic exception."); }

        // Checked shutdown aborts rather than tearing down uncertain GPU owners.
        engine.Shutdown();
        return verified ? 0 : 1;
    }
#endif

    engine.Run();

    engine.Shutdown();

    return 0;
}
