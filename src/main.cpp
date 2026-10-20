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
int VerifyProjectSystemWorkflow(const std::string& stage, const std::filesystem::path& workRoot);
int VerifyProjectRelaunch(const std::filesystem::path& source, const std::filesystem::path& target, const std::string& mode);
int VerifyProjectWorkflowRegressions();
int VerifyMeshImportUnits(const std::filesystem::path& root);
int VerifyPortableEditor(const std::filesystem::path& output);
int VerifyCookedMesh();
int VerifyScenePhysicsData();
int VerifyPrimitiveCreation();
int VerifyColliderVisualization();
int VerifyViewportMarquee();
int VerifyScenePhysicsPersistence();
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

    if(argc == 3 && std::string_view(argv[1]) == "--verify-portable-editor")
    {
        return VerifyPortableEditor(std::filesystem::absolute(argv[2]));
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
