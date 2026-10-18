#include "Engine/Engine.h"
#include "Engine/PackagedBootstrap.h"
#include "Core/Log.h"
#include <exception>
#include "Platform/Windows/WindowsText.h"
#include <shellapi.h>
#include <fstream>
#include <mutex>
#include <vector>
#include <string_view>

#if !defined(NDEBUG) && defined(ENGINE_HAS_VERIFICATION)
void VerifyPackagedPhysicsApplication(Engine& engine);
#endif

int RunRuntime(int argc, const std::vector<std::string>& argv)
{
#if !defined(NDEBUG) && defined(ENGINE_HAS_VERIFICATION)
    const bool verifyPhysics = argc > 1 && argv[1] == "--verify-physics";
#endif
    bool verify = false;
#if !defined(NDEBUG) && defined(ENGINE_HAS_VERIFICATION)
    verify = verifyPhysics || (argc > 1 && std::string_view(argv[1]) == "--verify-bootstrap");
#else
    if(argc > 1 && std::string_view(argv[1]).starts_with("--verify-"))
    {
        Log::Error("Verification requires a Debug build with ENGINE_BUILD_DIAGNOSTICS=ON and local test sources.");
        return 1;
    }
#endif
    if(verify)
    {
        if(GetConsoleWindow()!=nullptr) throw std::runtime_error("Packaged runtime unexpectedly has a console window.");
        Log::Info("Verified GUI runtime has no console window.");
    }
    const int sceneArgument=verify ? 2 : 1;
    const bool configured=argc==sceneArgument;
    if(!configured && argc!=sceneArgument+1 && argc!=sceneArgument+3)
    {
        Log::Error("Usage: EngineRuntime.exe [--verify-bootstrap] [<scene-uuid> [archive.toc archive.eap]]. No arguments use Game.startup. Relative archives resolve beside the executable; defaults are Game.toc and Game.eap.");
        return 1;
    }
    const auto deployment=WindowsWindow::GetExecutableDirectory();
    if(deployment.empty()) { Log::Error("Cannot resolve the runtime executable directory."); return 1; }
    std::optional<AssetID> scene;
    std::string gameTitle;
    try { if(configured) { auto config=PackagedBootstrap::ReadStartupConfiguration(deployment/"Game.startup"); scene=config.scene; gameTitle=std::move(config.title); }
        else scene=AssetID::Parse(argv[sceneArgument]); }
    catch(const std::exception& e) { Log::Error(std::string("Packaged startup selection failed: ")+e.what()); return 1; }
    if(!scene) { Log::Error("Packaged runtime requires a canonical cooked scene UUID."); return 1; }
    const auto resolve=[&](const std::string& path) { return (deployment / std::filesystem::u8path(path)).lexically_normal(); };
    EngineStartup startup;
    startup.mode=EngineStartup::Mode::PackagedRuntime;
    startup.sceneID=scene;
    startup.gameTitle=std::move(gameTitle);
    startup.toc=resolve(argc==sceneArgument+3 ? argv[sceneArgument+1] : "Game.toc");
    startup.pak=resolve(argc==sceneArgument+3 ? argv[sceneArgument+2] : "Game.eap");
    const auto pathUTF8=[](const std::filesystem::path& path) { const auto text=path.u8string(); return std::string(text.begin(),text.end()); };
    Log::Info("Packaged runtime archive: " + pathUTF8(startup.toc) + " / " + pathUTF8(startup.pak));
    auto runtimeEngine=std::make_unique<Engine>();
    auto& engine=*runtimeEngine;
    bool success=false;
    try
    {
        success=engine.Initialize(startup);
        if(success)
        {
#if !defined(NDEBUG) && defined(ENGINE_HAS_VERIFICATION)
            if(verifyPhysics)
            {
                VerifyPackagedPhysicsApplication(engine);
            }
            if(verify)
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
    catch(const std::exception& error)
    {
        Log::Error(std::string("Packaged runtime failed: ")+error.what());
        success=false;
    }
    // Includes failed preflight and partial initialization; GPU owners retire through
    // the same checked shutdown as the editor. Unconfirmed GPU completion remains fatal.
    engine.Shutdown();
    return success ? 0 : 1;
}

namespace
{
std::mutex logMutex;
std::ofstream runtimeLog;
std::filesystem::path logPath;
std::string lastError;
void RuntimeLog(std::string_view level,std::string_view message)
{
    std::lock_guard lock(logMutex);
    const std::string line="["+std::string(level)+"] "+std::string(message)+"\n";
    if(runtimeLog.is_open()) { runtimeLog << line; runtimeLog.flush(); }
    try { OutputDebugStringW(WindowsText::FromUTF8(line).c_str()); }
    catch(...) { OutputDebugStringA(line.c_str()); }
    if(level=="Error") lastError=message;
}
std::string ArgumentUTF8(const wchar_t* argument)
{
    const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argument,-1,nullptr,0,nullptr,nullptr);
    if(!size) throw std::runtime_error("Invalid Unicode command-line argument.");
    std::string result(size,'\0');
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argument,-1,result.data(),size,nullptr,nullptr))
        throw std::runtime_error("Cannot convert runtime command-line argument.");
    result.pop_back();
    return result;
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int)
{
    // No console is allocated or required. The editor retains its default log sink.
    Log::SetSink(RuntimeLog);
    int result=1;
    try
    {
        wchar_t temporary[MAX_PATH+1]{};
        const auto length=GetTempPathW(MAX_PATH+1,temporary);
        if(length && length<=MAX_PATH)
        {
            logPath=std::filesystem::path(temporary)/(L"EngineRuntime-"+std::to_wstring(GetCurrentProcessId())+L".log");
            runtimeLog.open(logPath,std::ios::binary|std::ios::trunc);
        }
        int argc=0;
        auto** wideArgs=CommandLineToArgvW(GetCommandLineW(),&argc);
        if(!wideArgs) throw std::runtime_error("Cannot read runtime command line.");
        struct ArgumentsOwner { wchar_t** data; ~ArgumentsOwner() { LocalFree(data); } } owner{wideArgs};
        std::vector<std::string> args;
        for(int i=0;i<argc;++i) args.push_back(ArgumentUTF8(wideArgs[i]));
        result=RunRuntime(argc,args);
    }
    catch(const std::exception& error) { Log::Error(std::string("Packaged runtime failed: ")+error.what()); }
    catch(...) { Log::Error("Packaged runtime failed with an unknown error."); }
    if(result!=0)
    {
        std::wstring message;
        try { message=WindowsText::FromUTF8(lastError.empty() ? "The packaged game could not start." : lastError); }
        catch(...) { message=L"The packaged game could not start. See debugger output for details."; }
        if(runtimeLog.is_open()) message+=L"\n\nDiagnostic log: "+logPath.native();
        else message+=L"\n\nA diagnostic log could not be created. Details were sent to Windows debugger output.";
        MessageBoxW(nullptr,message.c_str(),L"Packaged game error",MB_OK|MB_ICONERROR|MB_TASKMODAL);
    }
    Log::SetSink(nullptr);
    return result;
}
