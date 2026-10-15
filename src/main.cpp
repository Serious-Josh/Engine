#include "Engine/Engine.h"
#include "Core/Log.h"

#include <cstdlib>
#include <string_view>
#include <filesystem>
#include <exception>

#ifndef NDEBUG
int VerifyCookedMesh();
int CookProjectMeshes(const std::filesystem::path& projectRoot);
int VerifyDevelopmentCookedMeshProvider(const std::filesystem::path& projectRoot);
int VerifyCookedMeshPreparation(const std::filesystem::path& projectRoot);
#endif

int main(int argc, char** argv)
{
    const bool cookedGPU = argc >= 2 && (std::string_view(argv[1]) == "--verify-cooked-gpu-preparation" ||
        std::string_view(argv[1]) == "--verify-cooked-publication");
    const bool visualReview = argc == 4 && std::string_view(argv[1]) == "--verify-cooked-publication" &&
        std::string_view(argv[3]) == "--visual";
    if(cookedGPU && argc != 3 && !visualReview)
    {
        Log::Error("Usage: Engine.exe --verify-cooked-gpu-preparation|--verify-cooked-publication <absolute-scene-path> [--visual (publication only)]");
        return 1;
    }
#ifndef NDEBUG
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

    if(argc >= 2 && std::string_view(argv[1]) == "--cook-meshes")
    {
        if(argc > 3)
        {
            Log::Error("Usage: Engine.exe --cook-meshes [project-root]");
            return 1;
        }

        return CookProjectMeshes(argc == 3 ? std::filesystem::path(argv[2]) : std::filesystem::path{});
    }

    if(argc == 2 && std::string_view(argv[1]) == "--verify-cooked-mesh")
    {
        return VerifyCookedMesh();
    }
#else
    if(argc >= 2 && (std::string_view(argv[1]) == "--cook-meshes" || std::string_view(argv[1]) == "--verify-cooked-provider" ||
        std::string_view(argv[1]) == "--verify-cooked-preparation" || cookedGPU))
    {
        Log::Error("Mesh development diagnostics require the Debug executable.");
        return 1;
    }
#endif

    Engine engine;

    if (!engine.Initialize())
    {
        Log::Error("Engine initialization failed; aborting without automatic resource teardown because GPU completion may be unconfirmed.");
        std::abort();
    }

#ifndef NDEBUG
    if(argc == 2 && (std::string_view(argv[1]) == "--verify-material-bindings" ||
        std::string_view(argv[1]) == "--verify-viewport-material-drops"))
    {
        bool verified = false;
        try { verified = engine.VerifyMaterialBindingPublication(); }
        catch(const std::exception& error) { Log::Error("Material binding diagnostic: " + std::string(error.what())); }
        engine.Shutdown();
        return verified ? 0 : 1;
    }
    if(cookedGPU)
    {
        bool verified = false;
        try
        {
            verified = std::string_view(argv[1]) == "--verify-cooked-gpu-preparation" ?
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
