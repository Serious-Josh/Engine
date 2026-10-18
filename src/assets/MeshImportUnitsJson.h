#pragma once

#include "assets/MeshImportUnits.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

inline nlohmann::json SerializeMeshImportUnits(const MeshImportUnits& units)
{
    if(!units.IsValid())
    {
        throw std::runtime_error("Invalid mesh import-unit policy.");
    }

    const char* mode = units.mode == MeshImportUnitMode::Legacy ? "Legacy" :
        units.mode == MeshImportUnitMode::FileUnits ? "FileUnits" : "Manual";
    nlohmann::json result{{"mode", mode}};

    if(units.mode == MeshImportUnitMode::Manual)
    {
        result["scale"] = units.scale;
    }

    return result;
}

inline MeshImportUnits DeserializeMeshImportUnits(const nlohmann::json& value)
{
    if(!value.is_object() || !value.contains("mode") || !value.at("mode").is_string())
    {
        throw std::runtime_error("Mesh import units require an explicit mode.");
    }

    MeshImportUnits units;
    const auto mode = value.at("mode").get<std::string>();

    if(mode == "Legacy")
    {
        units.mode = MeshImportUnitMode::Legacy;
    }
    else if(mode == "FileUnits")
    {
        units.mode = MeshImportUnitMode::FileUnits;
    }
    else if(mode == "Manual" && value.contains("scale") && value.at("scale").is_number())
    {
        units.mode = MeshImportUnitMode::Manual;
        units.scale = value.at("scale").get<double>();
    }
    else
    {
        throw std::runtime_error("Unsupported mesh import-unit mode or missing manual factor.");
    }

    const size_t expectedFields = units.mode == MeshImportUnitMode::Manual ? 2 : 1;

    if(value.size() != expectedFields || !units.IsValid())
    {
        throw std::runtime_error("Mesh import units contain invalid fields or a nonpositive/nonfinite factor.");
    }

    return units;
}
