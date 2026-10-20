#include "Engine/PackagedBootstrap.h"
#include "Engine/BuildGame.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/MaterialTables/MaterialTableFile.h"
#include "assets/EngineAssetPak.h"
#include "Core/Log.h"
#include <stdexcept>
#include <fstream>
namespace PackagedBootstrap
{
void WriteStartup(const std::filesystem::path& path,const AssetID& scene,std::string_view title)
{
    std::string error;
    if(!title.empty() && !BuildGame::ValidateTitle(title,error)) throw std::runtime_error(error);
    std::ofstream f(path,std::ios::binary);
    f << "ENGINE_GAME_STARTUP 2\n" << scene.ToString() << "\n" << title << "\n"; f.close();
    if(!f) throw std::runtime_error("Cannot write packaged startup manifest.");
}
Startup ReadStartupConfiguration(const std::filesystem::path& path)
{
    if(std::filesystem::file_size(path)>256) throw std::runtime_error("Packaged startup manifest is too large.");
    std::ifstream f(path,std::ios::binary); std::string header,id,title,extra,error;
    if(!std::getline(f,header) || (header!="ENGINE_GAME_STARTUP 1" && header!="ENGINE_GAME_STARTUP 2") ||
        !std::getline(f,id) || !AssetID::Parse(id))
        throw std::runtime_error("Invalid packaged startup manifest header or scene UUID.");
    if(header=="ENGINE_GAME_STARTUP 2" && (!std::getline(f,title) || (!title.empty() && !BuildGame::ValidateTitle(title,error))))
        throw std::runtime_error("Invalid packaged startup title: "+error);
    if(std::getline(f,extra)) throw std::runtime_error("Unexpected packaged startup data.");
    return {*AssetID::Parse(id),std::move(title)};
}
AssetID ReadStartup(const std::filesystem::path& path) { return ReadStartupConfiguration(path).scene; }
std::vector<std::byte> ReadBundledMaterialTables(const std::filesystem::path& shaderRoot)
{
    const std::filesystem::path path = shaderRoot.parent_path() / MaterialTables::BundledTableRelativePath;
    std::vector<std::byte> bytes;
    MaterialTables::MaterialTableSet tables;
    std::string error;

    if(!MaterialTables::ReadMaterialTableBytes(path, bytes, &error) ||
        !MaterialTables::ValidateBundledMaterialTables(bytes, tables, &error))
    {
        throw std::runtime_error("Bundled material tables cannot be packaged: " + error);
    }

    return bytes;
}

void Cook(const std::filesystem::path& inputTOC,const std::filesystem::path& inputPak,
    const std::filesystem::path& outputTOC,const std::filesystem::path& outputPak,const std::filesystem::path& shaderRoot)
{
    auto archive=EngineAssetPak::Reader::Open(inputTOC,inputPak);
    std::vector<std::vector<std::byte>> owned;
    // PreparedAsset holds spans into owned: reserve every payload (entries, shaders, material tables) up front
    // so no push_back can reallocate and leave earlier spans dangling.
    owned.reserve(archive.Entries().size()+2);
    std::vector<EngineAssetPak::PreparedAsset> assets;
    size_t bytes=0;
    for(const auto& entry:archive.Entries())
    {
        if(entry.id==ShaderID() || entry.id==MaterialTablesID()) throw std::runtime_error("Bootstrap ID already exists in input archive.");
        if(entry.storedBytes>512*1024*1024-bytes) throw std::runtime_error("Bootstrap cook batch exceeds 512 MiB.");
        owned.push_back(archive.Read(entry.id)); bytes+=owned.back().size();
        assets.push_back({entry.id,entry.type,entry.payloadVersion,entry.generation,owned.back(),entry.alignment});
    }
    owned.push_back(ShaderLibrary::CookBootstrap(shaderRoot));
    if(owned.back().size()>64*1024*1024) throw std::runtime_error("Shader bundle exceeds 64 MiB.");
    assets.push_back({ShaderID(),EngineAssetPak::Type::Shader,1,1,owned.back(),16});
    owned.push_back(ReadBundledMaterialTables(shaderRoot));
    assets.push_back({MaterialTablesID(),EngineAssetPak::Type::Resource,MaterialTablesPayloadVersion,1,owned.back(),16});
    EngineAssetPak::Write(outputTOC,outputPak,assets);
    Log::Info("Packaged bootstrap archive written; shaders compiled offline. Runtime scene IDs are unchanged.");
}
}
