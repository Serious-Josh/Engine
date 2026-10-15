#include "Engine/BuildGame.h"
#include "Engine/PackagedBootstrap.h"
#include "assets/CookedScene.h"
#include "assets/CookedEnvironment.h"
#include "assets/CookedSurface.h"
#include "assets/ModelImporter.h"
#include "assets/ProjectAssetResolver.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include <Windows.h>
#include "Platform/Windows/WindowsText.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
namespace BuildGame
{
namespace fs=std::filesystem;
namespace
{
std::string UTF8Path(const fs::path& path) { const auto text=path.u8string(); return {text.begin(),text.end()}; }
void Require(bool ok,const std::string& error) { if(!ok) throw std::runtime_error(error); }
struct Pin
{
    HANDLE handle=INVALID_HANDLE_VALUE;
    explicit Pin(const fs::path& path)
    {
        handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        Require(handle!=INVALID_HANDLE_VALUE,"Cannot pin build input (missing, locked or permission denied): "+path.string());
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(handle,&info) || (info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)))
        { CloseHandle(handle); handle=INVALID_HANDLE_VALUE; throw std::runtime_error("Build inputs must be ordinary files: "+path.string()); }
    }
    ~Pin() { if(handle!=INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
void WriteText(const fs::path& path,const std::string& text)
{
    std::ofstream f(path,std::ios::binary|std::ios::trunc); f.write(text.data(),text.size()); f.close();
    Require(bool(f),"Cannot write deployment file: "+path.string());
}
void CheckDestination(const fs::path& output)
{
    if(!fs::exists(output)) return;
    Require(fs::is_directory(output) && !fs::is_symlink(output),"Output must be an ordinary directory.");
    if(fs::is_empty(output)) return;
    const auto marker=output/"BuildGame.files";
    Require(fs::exists(marker) && fs::file_size(marker)<64*1024,"Output is nonempty and is not an owned Build Game deployment. Choose an empty/new directory.");
    std::ifstream f(marker); std::string line; std::set<std::string> files{"BuildGame.files"};
    while(std::getline(f,line)) { Require(fs::u8path(line).filename()==fs::u8path(line) && !line.empty(),"Invalid deployment ownership list."); files.insert(line); }
    for(const auto& entry:fs::directory_iterator(output))
        Require(entry.is_regular_file() && !entry.is_symlink() && files.contains([&] { auto name=entry.path().filename().u8string(); return std::string(name.begin(),name.end()); }()),"Output contains unrelated files; choose a new directory. Nothing was overwritten.");
    for(const auto& name:files) if(fs::u8path(name).extension()==".exe" && fs::exists(output/fs::u8path(name)))
    {
        HANDLE test=CreateFileW((output/fs::u8path(name)).c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        Require(test!=INVALID_HANDLE_VALUE,"Previous game executable is locked or not writable. Close it before rebuilding."); CloseHandle(test);
    }
}
}
bool ValidateTitle(std::string_view title,std::string& error)
{
    error.clear();
    try { WindowsText::FromUTF8(title); } catch(const std::exception& e) { error=e.what(); return false; }
    if(title.empty() || title.size()>100 || title.front()==' ' || title.back()==' ' || title.back()=='.') error="Title must contain 1..100 characters and cannot start/end with spaces or end with a dot.";
    for(unsigned char c:title) if(c<32 || c==127 || std::string_view("<>:\"/\\|?*").find(char(c))!=std::string_view::npos) error="Title must be a safe UTF-8 Windows filename (no path separators or reserved characters).";
    std::string base(title.substr(0,title.find('.'))); for(auto& c:base) c=char(std::toupper(static_cast<unsigned char>(c)));
    while(!base.empty() && (base.back()==' ' || base.back()=='.')) base.pop_back();
    if(base=="CON" || base=="PRN" || base=="AUX" || base=="NUL" || base=="CLOCK$" || base=="CONIN$" || base=="CONOUT$" ||
        (base.size()==4 && (base.starts_with("COM") || base.starts_with("LPT")) && base[3]>='1' && base[3]<='9')) error="Title is a reserved Windows device name.";
    if(title.size()>=4) { std::string ext(title.substr(title.size()-4)); for(auto& c:ext)c=char(std::tolower(static_cast<unsigned char>(c))); if(ext==".exe") error="Enter the title without the .exe extension."; }
    return error.empty();
}
AssetID SceneIdentity(std::string_view persistentID)
{
    Require(!persistentID.empty(),"Current scene has no persistent identity.");
    if(auto id=AssetID::Parse(persistentID)) return *id;
    const std::string namespaced="Engine.Scene:"+std::string(persistentID);
    auto digest=CookedSurface::Hash(std::as_bytes(std::span(namespaced.data(),namespaced.size())));
    digest[6]=std::byte((std::to_integer<unsigned>(digest[6])&15)|0x50); digest[8]=std::byte((std::to_integer<unsigned>(digest[8])&63)|0x80);
    constexpr char hex[]="0123456789abcdef"; std::string uuid;
    for(size_t i=0;i<16;++i) { if(i==4||i==6||i==8||i==10) uuid+='-'; auto b=std::to_integer<unsigned>(digest[i]); uuid+=hex[b>>4]; uuid+=hex[b&15]; }
    return *AssetID::Parse(uuid);
}
struct Service::State { mutable std::mutex mutex; Status status; bool cancel=false; };
Service::Service():m_state(std::make_shared<State>()) {}
Service::~Service() { Cancel(); } // jthread joins; no scene/renderer/UI borrows exist on the worker.
void Service::Cancel() { std::lock_guard lock(m_state->mutex); m_state->cancel=true; }
Status Service::Poll() const { std::lock_guard lock(m_state->mutex); return m_state->status; }
bool Service::Start(Request request,std::string& error)
{
    if(!ValidateTitle(request.settings.title,error)) return false;
    if(request.settings.output.empty()) { error="Select an output directory."; return false; }
    { std::lock_guard lock(m_state->mutex); if(m_state->status.busy) { error="A game build is already running."; return false; } }
    if(m_worker.joinable()) m_worker.join();
    { std::lock_guard lock(m_state->mutex); m_state->cancel=false; m_state->status={true,false,0,"Starting game build..."}; }
    try { m_worker=std::jthread([state=m_state,request=std::move(request)]()
    {
        fs::path stage;
        bool stagingOwned=false;
        const auto progress=[&](float value,const std::string& message)
        { std::lock_guard lock(state->mutex); Require(!state->cancel,"Build canceled; previous output preserved."); state->status.progress=value; state->status.message=message; };
        try
        {
            progress(.02f,"Validating output and current scene snapshot...");
            std::string validation; Require(ValidateSceneDocument(request.document,&validation),validation);
            const auto sceneID=SceneIdentity(request.document.id);
            const auto output=fs::absolute(request.settings.output).lexically_normal();
            Require(output.has_filename() && output!=output.root_path(),"Choose a named output directory, not a drive root.");
            // Never stage/publish over project/source trees, including an ancestor of the project.
            const auto project=fs::weakly_canonical(request.project);
            const auto destination=fs::weakly_canonical(output);
            const auto within=[](const fs::path& a,const fs::path& b) { const auto rel=a.lexically_relative(b); return !rel.empty() && *rel.begin()!=".."; };
            Require(!within(project,destination) && !within(destination,project),"Output cannot be inside or contain the development project.");
            CheckDestination(output); fs::create_directories(output.parent_path());
            const auto token=AssetID::Generate(); Require(bool(token),"Cannot generate staging identity.");
            stage=output.parent_path()/(output.filename().string()+".building-"+token->ToString());
            Require(fs::create_directory(stage),"Cannot create private staging directory.");
            stagingOwned=true;
            std::vector<CookedSurface::Asset> assets; std::vector<std::unique_ptr<Pin>> pins;
            std::uint64_t retained=0;
            const auto append=[&](CookedSurface::Asset asset)
            { Require(asset.bytes.size()<=512ull*1024*1024-retained,"Build payloads exceed 512 MiB batch limit."); retained+=asset.bytes.size(); assets.push_back(std::move(asset)); };
            std::set<std::string> meshes;
            for(const auto& object:request.document.objects) if(!object.meshReference.empty())
            { auto id=AssetID::ParseReference(object.meshReference); Require(bool(id),"Only registered mesh assets can be packaged; procedural/builtin mesh references are unsupported."); meshes.insert(id->ToString()); }
            for(const auto& text:meshes)
            {
                progress(.1f,"Cooking mesh "+text);
                const auto id=*AssetID::Parse(text); const auto* record=request.registry.Find(id);
                const auto resolved=ProjectAssetResolver::Resolve(ProjectPaths(request.project),request.registry,id);
                Require(record && record->type==AssetType::Mesh && resolved.status==AssetAvailability::Available,"Referenced mesh is missing or invalid: "+text);
                Require(fs::file_size(resolved.resolvedPath)<=64ull*1024*1024,"Mesh source exceeds 64 MiB build limit.");
                pins.push_back(std::make_unique<Pin>(resolved.resolvedPath));
                auto geometry=ModelImporter::LoadFlattened(resolved.resolvedPath,MeshUVOrientation::FlipV,CookedMesh::Policy{}.maxDecodedBytes);
                auto encoded=CookedMesh::Encode(geometry,CookedMesh::Policy{}); Require(encoded.status==CookedMesh::Status::Success,std::string(encoded.message));
                append({id,EngineAssetPak::Type::Mesh,1,std::move(encoded.payload)});
            }
            progress(.35f,"Cooking scene metadata, materials and textures...");
            std::vector<CookedScene::MeshInput> inputs; for(const auto& a:assets) inputs.push_back({a.id,a.generation,a.bytes});
            std::optional<CookedScene::EnvironmentInput> environment;
            if(!request.document.environmentReference.empty())
            {
                progress(.3f,"Cooking assigned HDR environment...");
                const auto id=*AssetID::ParseReference(request.document.environmentReference);
                const auto* record=request.registry.Find(id);
                const auto resolved=ProjectAssetResolver::Resolve(ProjectPaths(request.project),request.registry,id);
                Require(record && record->type==AssetType::Texture && resolved.status==AssetAvailability::Available,"Required scene HDR environment is missing or invalid.");
                auto payload=CookedEnvironment::Cook(resolved.resolvedPath,id,1);
                append({id,EngineAssetPak::Type::Resource,1,std::move(payload)});
                environment=CookedScene::EnvironmentInput{id,1,assets.back().bytes};
            }
            auto sceneBytes=CookedScene::Cook(request.document,inputs,request.registry,{},environment?&*environment:nullptr);
            auto surfaces=CookedSurface::CookDependencies(CookedScene::Decode(sceneBytes),ProjectPaths(request.project),request.registry);
            for(auto& a:surfaces) append(std::move(a));
            append({sceneID,EngineAssetPak::Type::Scene,1,std::move(sceneBytes)});
            progress(.55f,"Compiling packaged shaders offline...");
            auto shaders=ShaderLibrary::CookBootstrap(request.shaderRoot);
            Require(shaders.size()<=64ull*1024*1024,"Shader bootstrap exceeds the runtime 64 MiB limit.");
            append({PackagedBootstrap::ShaderID(),EngineAssetPak::Type::Shader,1,std::move(shaders)});
            std::uint64_t total=0; for(const auto& a:assets) { total+=a.bytes.size(); Require(total<=512ull*1024*1024,"Build payloads exceed 512 MiB batch limit."); }
            progress(.75f,"Writing and validating matching game archives...");
            std::vector<EngineAssetPak::PreparedAsset> prepared; for(const auto& a:assets) prepared.push_back({a.id,a.type,1,a.generation,a.bytes,16});
            EngineAssetPak::Write(stage/"Game.toc",stage/"Game.eap",prepared);
            { auto reader=EngineAssetPak::Reader::Open(stage/"Game.toc",stage/"Game.eap"); for(const auto& entry:reader.Entries()) reader.Read(entry.id); }
            { auto verified=CookedScene::OpenArchive(stage/"Game.toc",stage/"Game.eap",sceneID); Require(verified->metadata.document.id==request.document.id,"Packaged startup scene identity differs."); }
            progress(.9f,"Assembling standalone executable and runtime dependencies...");
            std::vector<std::string> files{"Game.toc","Game.eap","Game.startup",request.settings.title+".exe"};
            const auto copy=[&](const fs::path& source,const std::string& name) { Pin pin(source); fs::copy_file(source,stage/fs::u8path(name)); };
            copy(request.runtimeDirectory/"EngineRuntime.exe",request.settings.title+".exe");
            std::set<std::string> requiredDLLs{"NRD.dll","NRI.dll","dxcompiler.dll","dxil.dll"};
            unsigned int assimpDLLs=0, textureDLLs=0;
            for(const auto& entry:fs::directory_iterator(request.runtimeDirectory))
            {
                const auto name=entry.path().filename().string();
                if(entry.path().extension()==".dll" && (name=="NRD.dll" || name=="NRI.dll" || name=="dxcompiler.dll" || name=="dxil.dll" || name.starts_with("assimp-") || name.starts_with("DirectXTex")))
                { copy(entry.path(),name); files.push_back(name); requiredDLLs.erase(name);
                  if(name.starts_with("assimp-")) ++assimpDLLs; if(name.starts_with("DirectXTex")) ++textureDLLs; }
            }
            Require(files.size()==10 && requiredDLLs.empty() && assimpDLLs==1 && textureDLLs==1,"Runtime directory is missing required DLLs or has ambiguous dependency versions; build EngineRuntime first.");
            PackagedBootstrap::WriteStartup(stage/"Game.startup",sceneID,request.settings.title);
            const auto config=PackagedBootstrap::ReadStartupConfiguration(stage/"Game.startup");
            Require(config.scene==sceneID && config.title==request.settings.title,"Packaged startup configuration differs from build settings.");
            std::string owned; for(const auto& file:files) owned+=file+"\n"; WriteText(stage/"BuildGame.files",owned);
            progress(.98f,"Publishing validated deployment...");
            // Cancellation and publication serialize; shutdown cannot authorize publication after cancel.
            std::lock_guard lock(state->mutex); Require(!state->cancel,"Build canceled before publication; previous output preserved.");
            CheckDestination(output);
            fs::path previous;
            if(fs::exists(output)) { previous=output.parent_path()/(output.filename().string()+".previous-"+token->ToString()); fs::rename(output,previous); }
            try { fs::rename(stage,output); stage.clear(); }
            catch(...) { if(!previous.empty()) fs::rename(previous,output); throw; }
            state->status={false,true,1,"Build succeeded: "+UTF8Path(output/fs::u8path(request.settings.title+".exe"))+(previous.empty()?"":"; previous output retained at "+previous.string())};
        }
        catch(const std::exception& e)
        {
            if(stagingOwned && !stage.empty()) { std::error_code ignored; fs::remove_all(stage,ignored); }
            std::lock_guard lock(state->mutex); state->status={false,false,0,e.what()};
        }
    }); }
    catch(const std::exception& e) { std::lock_guard lock(m_state->mutex); m_state->status={false,false,0,e.what()}; error=e.what(); return false; }
    return true;
}
}
