#pragma once
#include "assets/AssetID.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>
namespace PackagedBootstrap
{
    struct Startup { AssetID scene; std::string title; };
    void WriteStartup(const std::filesystem::path&, const AssetID&, std::string_view title = {});
    Startup ReadStartupConfiguration(const std::filesystem::path&);
    AssetID ReadStartup(const std::filesystem::path&);
    inline AssetID ShaderID() { return *AssetID::Parse("00000000-0000-0000-0000-000000000035"); }

    // RTXPT.1A.2 bundled GGX material tables: an optional Resource entry, payload version 1, containing the
    // .emt file bytes unchanged. Archives built before RTXPT.1A.2 lack it and still boot (path tracing then
    // reports itself unavailable).
    inline AssetID MaterialTablesID() { return *AssetID::Parse("00000000-0000-0000-0000-000000000036"); }
    inline constexpr std::uint32_t MaterialTablesPayloadVersion = 1;

    // Reads assets/MaterialTables beside the given shader root and verifies the SHA-256 pin and layout.
    // Throws std::runtime_error when the tables are missing or do not match: packaging never ships others.
    std::vector<std::byte> ReadBundledMaterialTables(const std::filesystem::path& shaderRoot);
    // Explicit offline command. Reads authoring shaders; never called by packaged startup.
    void Cook(const std::filesystem::path& inputTOC,const std::filesystem::path& inputPak,
        const std::filesystem::path& outputTOC,const std::filesystem::path& outputPak,
        const std::filesystem::path& shaderRoot);
}
