#include "Scene/CloudPresets.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace
{
    using Json = nlohmann::json;

    void Fields(const Json& object, std::initializer_list<const char*> allowed)
    {
        if(!object.is_object())
        {
            throw std::runtime_error("expected an object");
        }

        for(auto field = object.begin(); field != object.end(); ++field)
        {
            bool known = false;

            for(const char* name : allowed)
            {
                known = known || field.key() == name;
            }

            if(!known)
            {
                throw std::runtime_error("unknown field '" + field.key() + "'");
            }
        }
    }

    void Number(const Json& object, const char* field, float& target)
    {
        if(!object.contains(field))
        {
            return;
        }

        if(!object.at(field).is_number())
        {
            throw std::runtime_error(std::string("'") + field + "' must be a number");
        }

        target = object.at(field).get<float>();
    }

    CloudLayerSettings ReadLayer(const Json& entry)
    {
        Fields(entry, {"name", "kind", "altitudeKm", "thicknessKm", "coverage", "density", "shape", "lighting", "wind"});
        CloudLayerSettings layer;
        const std::string kind = entry.value("kind", std::string("volumetric"));

        if(kind != "layer" && kind != "volumetric")
        {
            throw std::runtime_error("kind must be layer or volumetric");
        }

        layer.kind = kind == "layer" ? CloudLayerKind::Layer : CloudLayerKind::Volumetric;
        Number(entry, "altitudeKm", layer.altitudeKm);
        Number(entry, "thicknessKm", layer.thicknessKm);
        Number(entry, "coverage", layer.coverage);
        Number(entry, "density", layer.density);

        if(entry.contains("shape"))
        {
            const Json& shape = entry.at("shape");
            Fields(shape, {"scaleKm", "detailScaleKm", "erosion", "factor"});
            Number(shape, "scaleKm", layer.shapeScaleKm);
            Number(shape, "detailScaleKm", layer.detailScaleKm);
            Number(shape, "erosion", layer.erosion);
            Number(shape, "factor", layer.shapeFactor);
        }

        if(entry.contains("lighting"))
        {
            const Json& lighting = entry.at("lighting");
            Fields(lighting, {"albedo", "phaseForward", "phaseBackward", "phaseBlend", "multipleScattering", "ambientStrength"});

            if(lighting.contains("albedo"))
            {
                const Json& albedo = lighting.at("albedo");

                if(!albedo.is_array() || albedo.size() != 3)
                {
                    throw std::runtime_error("albedo must be three numbers");
                }

                for(std::size_t channel = 0; channel < 3; ++channel)
                {
                    layer.albedo[channel] = albedo.at(channel).get<float>();
                }
            }

            Number(lighting, "phaseForward", layer.phaseForward);
            Number(lighting, "phaseBackward", layer.phaseBackward);
            Number(lighting, "phaseBlend", layer.phaseBlend);
            Number(lighting, "multipleScattering", layer.multipleScattering);
            Number(lighting, "ambientStrength", layer.ambientStrength);
        }

        if(entry.contains("wind"))
        {
            const Json& wind = entry.at("wind");
            Fields(wind, {"directionDegrees", "speedMetersPerSecond"});
            Number(wind, "directionDegrees", layer.windDirectionDegrees);
            Number(wind, "speedMetersPerSecond", layer.windSpeedMetersPerSecond);
        }

        return layer;
    }
}

namespace CloudPresets
{
    std::vector<CloudPreset> Parse(std::string_view text, std::string* error)
    {
        std::vector<CloudPreset> presets;

        try
        {
            const Json document = Json::parse(text);

            if(document.value("format", std::string()) != "EngineCloudPresets" || document.value("version", 0) != 1 ||
                !document.contains("presets") || !document.at("presets").is_array())
            {
                throw std::runtime_error("not an EngineCloudPresets version 1 document");
            }

            for(const Json& entry : document.at("presets"))
            {
                CloudPreset preset;
                preset.name = entry.value("name", std::string());

                if(preset.name.empty() || preset.name.size() > CloudLayerSettings::MaximumPresetNameLength)
                {
                    throw std::runtime_error("every preset needs a name of at most 64 characters");
                }

                try
                {
                    preset.layer = ReadLayer(entry);
                }
                catch(const std::exception& exception)
                {
                    throw std::runtime_error("preset '" + preset.name + "': " + exception.what());
                }

                preset.layer.preset = preset.name;
                PhysicalSkySettings check;
                check.cloudLayers.push_back(preset.layer);
                std::string invalid;

                if(!ValidatePhysicalSkySettings(check, &invalid))
                {
                    throw std::runtime_error("preset '" + preset.name + "': " + invalid);
                }

                presets.push_back(std::move(preset));
            }
        }
        catch(const std::exception& exception)
        {
            if(error)
            {
                *error = std::string("Cloud presets: ") + exception.what();
            }

            return {};
        }

        return presets;
    }

    std::vector<CloudPreset> Load(const std::filesystem::path& path, std::string* error)
    {
        std::ifstream file(path, std::ios::binary);

        if(!file)
        {
            if(error)
            {
                *error = "Cloud presets: cannot read " + path.string();
            }

            return {};
        }

        std::ostringstream text;
        text << file.rdbuf();
        return Parse(text.str(), error);
    }

    void Apply(const CloudPreset& preset, CloudLayerSettings& layer)
    {
        const CloudControlMode mode = layer.controlMode;
        const bool shadows = layer.castShadows;
        const std::string map = layer.coverageMap;
        const float extent = layer.coverageMapExtentKm;
        layer = preset.layer;
        layer.controlMode = mode;
        layer.castShadows = shadows;
        layer.coverageMap = map;
        layer.coverageMapExtentKm = extent;
        layer.preset = preset.name;
    }
}
