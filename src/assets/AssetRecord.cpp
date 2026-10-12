#include "assets/AssetRecord.h"

bool ValidateAssetRecord(const AssetRecord& record)
{
    std::filesystem::path expectedDirectory;

    switch(record.type){
        case AssetType::Material:
            expectedDirectory = "Materials";
            break;
        case AssetType::Mesh:
            expectedDirectory = "Meshes";
            break;
        case AssetType::Texture:
            expectedDirectory = "Textures";
            break;

        default:
            return false;
    }

    const auto& path = record.relativePath;

    if(path.empty() ||
       path.is_absolute() ||
       path.has_root_name() ||
       path.has_root_directory() ||
       !path.has_filename() ||
       path.filename() == "." ||
       path.filename() == "..")
    {
        return false;
    }

    for(const auto& component : path){
        if(component.empty() ||
           component == "." ||
           component == ".."){
            return false;
        }
    }

    auto it = path.begin();

    if(it == path.end() || *it != expectedDirectory){
        return false;
    }

    ++it;

    if(it == path.end()){
        return false;
    }

    return true;
}