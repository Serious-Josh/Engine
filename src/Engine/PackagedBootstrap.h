#pragma once
#include "assets/AssetID.h"
#include <filesystem>
namespace PackagedBootstrap
{
    struct Startup { AssetID scene; std::string title; };
    void WriteStartup(const std::filesystem::path&, const AssetID&, std::string_view title = {});
    Startup ReadStartupConfiguration(const std::filesystem::path&);
    AssetID ReadStartup(const std::filesystem::path&);
    inline AssetID ShaderID() { return *AssetID::Parse("00000000-0000-0000-0000-000000000035"); }
    // Explicit offline command. Reads authoring shaders; never called by packaged startup.
    void Cook(const std::filesystem::path& inputTOC,const std::filesystem::path& inputPak,
        const std::filesystem::path& outputTOC,const std::filesystem::path& outputPak,
        const std::filesystem::path& shaderRoot);
}
