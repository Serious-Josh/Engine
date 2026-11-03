#include "Project/ProjectSettings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

namespace
{
    constexpr std::pair<ProjectLightingMode, std::string_view> kLightingModes[] = {
        {ProjectLightingMode::Legacy, "legacy"},
        {ProjectLightingMode::ReferencePathTracer, "referencePathTracer"},
        {ProjectLightingMode::PathTracing, "pathTracing"}};
    constexpr std::pair<ProjectToneMapper, std::string_view> kToneMappers[] = {
        {ProjectToneMapper::AgX, "agx"},
        {ProjectToneMapper::GT7, "gt7"}};
    constexpr std::pair<ProjectReflectionDenoiser, std::string_view> kReflectionDenoisers[] = {
        {ProjectReflectionDenoiser::Nrd, "nrd"},
        {ProjectReflectionDenoiser::Native, "native"}};
    constexpr std::pair<ProjectPsrMode, std::string_view> kPsrModes[] = {
        {ProjectPsrMode::Off, "off"},
        {ProjectPsrMode::SingleBounce, "singleBounce"},
        {ProjectPsrMode::TwoBounce, "twoBounce"}};
    constexpr std::pair<ProjectPsrDebugView, std::string_view> kPsrDebugViews[] = {
        {ProjectPsrDebugView::Off, "off"},
        {ProjectPsrDebugView::ReplacementMask, "replacementMask"},
        {ProjectPsrDebugView::ReflectedNormals, "reflectedNormals"},
        {ProjectPsrDebugView::ReflectedAlbedo, "reflectedAlbedo"}};
    constexpr std::pair<ProjectDlssQuality, std::string_view> kDlssQualities[] = {
        {ProjectDlssQuality::Quality, "quality"},
        {ProjectDlssQuality::Balanced, "balanced"},
        {ProjectDlssQuality::Performance, "performance"}};
    constexpr std::pair<ProjectPathTraceDenoiser, std::string_view> kDenoisers[] = {
        {ProjectPathTraceDenoiser::Off, "off"},
        {ProjectPathTraceDenoiser::Engine, "engine"},
        {ProjectPathTraceDenoiser::NrdReblur, "nrdReblur"},
        {ProjectPathTraceDenoiser::NrdRelax, "nrdRelax"},
        {ProjectPathTraceDenoiser::NrdHybrid, "nrdHybrid"}};
    constexpr std::pair<ProjectPathTraceDenoiserView, std::string_view> kDenoiserViews[] = {
        {ProjectPathTraceDenoiserView::Normal, "normal"},
        {ProjectPathTraceDenoiserView::Temporal, "temporal"},
        {ProjectPathTraceDenoiserView::Spatial, "spatial"},
        {ProjectPathTraceDenoiserView::SpatialTemporalDifference, "spatialTemporalDifference"},
        {ProjectPathTraceDenoiserView::EstimatedUncertainty, "estimatedUncertainty"},
        {ProjectPathTraceDenoiserView::HistoryLength, "historyLength"},
        {ProjectPathTraceDenoiserView::RawExtraction, "rawExtraction"}};
    constexpr std::pair<ProjectPathTraceDenoiserSignal, std::string_view> kDenoiserSignals[] = {
        {ProjectPathTraceDenoiserSignal::Diffuse, "diffuse"},
        {ProjectPathTraceDenoiserSignal::Specular, "specular"}};

    template <typename Enum, std::size_t Count>
    std::string_view NameOf(const std::pair<Enum, std::string_view> (&table)[Count], Enum value)
    {
        for(const auto& [entry, name] : table)
        {
            if(entry == value)
            {
                return name;
            }
        }

        return table[0].second;
    }

    template <typename Enum, std::size_t Count>
    bool IsKnown(const std::pair<Enum, std::string_view> (&table)[Count], Enum value)
    {
        return std::any_of(std::begin(table), std::end(table), [value](const auto& entry) { return entry.first == value; });
    }

    // Reads members of one JSON object. A missing member keeps its default silently (a newer
    // Engine may add fields); a member of the wrong type or value is recovered with a warning.
    class FieldReader
    {
    public:
        FieldReader(const nlohmann::json* object, std::string path, std::vector<std::string>& warnings)
            : m_object(object), m_path(std::move(path)), m_warnings(warnings)
        {
        }

        FieldReader Child(std::string_view key) const
        {
            const auto* member = Find(key);

            if(member && !member->is_object())
            {
                Warn(key, "must be an object; its settings use defaults");
                member = nullptr;
            }

            return FieldReader(member, Join(key), m_warnings);
        }

        void Bool(std::string_view key, bool& value) const
        {
            if(const auto* member = Find(key))
            {
                if(member->is_boolean())
                {
                    value = member->get<bool>();
                }
                else
                {
                    Warn(key, "must be true or false; using the default");
                }
            }
        }

        void UInt(std::string_view key, std::uint32_t& value, std::uint32_t minimum, std::uint32_t maximum) const
        {
            const auto* member = Find(key);

            if(!member)
            {
                return;
            }

            if(!member->is_number_integer())
            {
                Warn(key, "must be a whole number; using the default");
                return;
            }

            const auto read = member->get<long long>();
            const auto clamped = std::clamp<long long>(read, minimum, maximum);

            if(clamped != read)
            {
                Warn(key, "is outside " + std::to_string(minimum) + "-" + std::to_string(maximum) +
                    "; clamped to " + std::to_string(clamped));
            }

            value = static_cast<std::uint32_t>(clamped);
        }

        void Float(std::string_view key, float& value, float minimum, float maximum) const
        {
            const auto* member = Find(key);

            if(!member)
            {
                return;
            }

            if(!member->is_number())
            {
                Warn(key, "must be a number; using the default");
                return;
            }

            const double read = member->get<double>();

            if(!std::isfinite(read))
            {
                Warn(key, "must be finite; using the default");
                return;
            }

            const double clamped = std::clamp(read, static_cast<double>(minimum), static_cast<double>(maximum));

            if(clamped != read)
            {
                Warn(key, "is outside " + std::to_string(minimum) + " to " + std::to_string(maximum) +
                    "; clamped to " + std::to_string(clamped));
            }

            value = static_cast<float>(clamped);
        }

        template <typename Enum, std::size_t Count>
        void Name(std::string_view key, Enum& value, const std::pair<Enum, std::string_view> (&table)[Count]) const
        {
            const auto* member = Find(key);

            if(!member)
            {
                return;
            }

            if(member->is_string())
            {
                const auto text = member->get<std::string>();

                for(const auto& [entry, name] : table)
                {
                    if(name == text)
                    {
                        value = entry;
                        return;
                    }
                }
            }

            std::string choices;

            for(const auto& [entry, name] : table)
            {
                choices += (choices.empty() ? "" : ", ") + std::string(name);
            }

            Warn(key, "must be one of " + choices + "; using the default \"" + std::string(NameOf(table, value)) + "\"");
        }

    private:
        const nlohmann::json* Find(std::string_view key) const
        {
            if(!m_object)
            {
                return nullptr;
            }

            const auto found = m_object->find(key);
            return found == m_object->end() ? nullptr : &*found;
        }

        std::string Join(std::string_view key) const
        {
            return m_path + "." + std::string(key);
        }

        void Warn(std::string_view key, const std::string& problem) const
        {
            m_warnings.push_back(Join(key) + " " + problem + ".");
        }

        const nlohmann::json* m_object;
        std::string m_path;
        std::vector<std::string>& m_warnings;
    };

    void ReadRendering(const FieldReader& rendering, RenderingSettings& settings)
    {
        rendering.Name("lightingMode", settings.lightingMode, kLightingModes);
        rendering.Name("toneMapper", settings.toneMapper, kToneMappers);

        auto& legacy = settings.legacy;
        const auto legacyReader = rendering.Child("legacy");
        legacyReader.Name("reflectionDenoiser", legacy.reflectionDenoiser, kReflectionDenoisers);
        legacyReader.Bool("contributionLimiter", legacy.contributionLimiter);
        legacyReader.Bool("rtgi", legacy.rtgi);
        legacyReader.Float("rtgiContactAOStrength", legacy.rtgiContactAOStrength,
            LegacyRenderingSettings::MinimumContactAOStrength, LegacyRenderingSettings::MaximumContactAOStrength);
        legacyReader.Float("rtgiContactAORadius", legacy.rtgiContactAORadius,
            LegacyRenderingSettings::MinimumContactAORadius, LegacyRenderingSettings::MaximumContactAORadius);

        using PT = PathTracingRenderingSettings;
        auto& pathTracing = settings.pathTracing;
        const auto pathTracingReader = rendering.Child("pathTracing");
        const auto quality = pathTracingReader.Child("quality");
        quality.UInt("samplesPerFrame", pathTracing.samplesPerFrame, 1, PT::MaximumSamplesPerFrame);
        quality.UInt("maxPathDepth", pathTracing.maxPathDepth, 1, PT::MaximumPathDepth);
        quality.UInt("rouletteFromDepth", pathTracing.rouletteFromDepth, 1, PT::MaximumRouletteDepth);
        quality.UInt("sampleTarget", pathTracing.sampleTarget, 1, PT::MaximumSampleTarget);

        const auto psr = pathTracingReader.Child("psr");
        psr.Name("mode", pathTracing.psrMode, kPsrModes);
        psr.Name("debugView", pathTracing.psrDebugView, kPsrDebugViews);

        const auto dlss = pathTracingReader.Child("dlss");
        dlss.Bool("enabled", pathTracing.dlss);
        dlss.Name("quality", pathTracing.dlssQuality, kDlssQualities);

        const auto denoising = pathTracingReader.Child("denoising");
        denoising.Bool("cameraJitter", pathTracing.cameraJitter);
        denoising.Name("denoiser", pathTracing.denoiser, kDenoisers);
        denoising.Name("view", pathTracing.denoiserView, kDenoiserViews);
        denoising.Name("signal", pathTracing.denoiserSignal, kDenoiserSignals);
        denoising.Float("viewExposureStops", pathTracing.viewExposureStops, PT::MinimumViewExposureStops,
            PT::MaximumViewExposureStops);
        denoising.Float("differenceAmplification", pathTracing.differenceAmplification,
            PT::MinimumDifferenceAmplification, PT::MaximumDifferenceAmplification);
        denoising.Float("uncertaintyFloorLog10", pathTracing.uncertaintyFloorLog10, PT::MinimumUncertaintyFloorLog10,
            PT::MaximumUncertaintyFloorLog10);

        pathTracingReader.Child("sharc").Bool("enabled", pathTracing.sharc);

        const auto caustics = pathTracingReader.Child("caustics");
        caustics.Bool("dedicated", pathTracing.dedicatedCaustics);
        caustics.Bool("causticOnlyView", pathTracing.causticOnlyView);
        caustics.Bool("pixelCentred", pathTracing.pixelCentredCaustics);
    }

    // The JSON object member `key`, created (or replaced, when it is not an object) for writing.
    nlohmann::json& ObjectMember(nlohmann::json& parent, std::string_view key)
    {
        auto& member = parent[std::string(key)];

        if(!member.is_object())
        {
            member = nlohmann::json::object();
        }

        return member;
    }

    template <typename Value>
    void Repair(Value& value, Value repaired, std::string message, std::vector<std::string>* warnings, bool& valid)
    {
        if(value != repaired)
        {
            value = repaired;
            valid = false;

            if(warnings)
            {
                warnings->push_back(std::move(message));
            }
        }
    }

    float ClampFloat(float value, float minimum, float maximum, float fallback)
    {
        return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
    }
}

bool ValidateProjectSettings(ProjectSettings& settings, std::vector<std::string>* warnings)
{
    bool valid = true;
    auto& rendering = settings.rendering;
    const RenderingSettings defaults;

    // Enum values outside the named choices can only come from a cast; they recover to the default.
    if(!IsKnown(kLightingModes, rendering.lightingMode))
    {
        Repair(rendering.lightingMode, defaults.lightingMode, "rendering.lightingMode was invalid; using legacy.", warnings, valid);
    }

    if(!IsKnown(kToneMappers, rendering.toneMapper))
    {
        Repair(rendering.toneMapper, defaults.toneMapper, "rendering.toneMapper was invalid; using agx.", warnings, valid);
    }

    auto& legacy = rendering.legacy;

    if(!IsKnown(kReflectionDenoisers, legacy.reflectionDenoiser))
    {
        Repair(legacy.reflectionDenoiser, defaults.legacy.reflectionDenoiser,
            "rendering.legacy.reflectionDenoiser was invalid; using nrd.", warnings, valid);
    }

    Repair(legacy.rtgiContactAOStrength, ClampFloat(legacy.rtgiContactAOStrength, LegacyRenderingSettings::MinimumContactAOStrength,
        LegacyRenderingSettings::MaximumContactAOStrength, defaults.legacy.rtgiContactAOStrength),
        "rendering.legacy.rtgiContactAOStrength was outside 0-1; clamped.", warnings, valid);
    Repair(legacy.rtgiContactAORadius, ClampFloat(legacy.rtgiContactAORadius, LegacyRenderingSettings::MinimumContactAORadius,
        LegacyRenderingSettings::MaximumContactAORadius, defaults.legacy.rtgiContactAORadius),
        "rendering.legacy.rtgiContactAORadius was outside 0.05-5; clamped.", warnings, valid);

    using PT = PathTracingRenderingSettings;
    auto& pathTracing = rendering.pathTracing;
    const PT& pathTracingDefaults = defaults.pathTracing;
    Repair(pathTracing.samplesPerFrame, std::clamp(pathTracing.samplesPerFrame, 1u, PT::MaximumSamplesPerFrame),
        "rendering.pathTracing.quality.samplesPerFrame was outside 1-64; clamped.", warnings, valid);
    Repair(pathTracing.maxPathDepth, std::clamp(pathTracing.maxPathDepth, 1u, PT::MaximumPathDepth),
        "rendering.pathTracing.quality.maxPathDepth was outside 1-64; clamped.", warnings, valid);
    Repair(pathTracing.rouletteFromDepth, std::clamp(pathTracing.rouletteFromDepth, 1u, PT::MaximumRouletteDepth),
        "rendering.pathTracing.quality.rouletteFromDepth was outside 1-65; clamped.", warnings, valid);
    // The sample target can never be below one frame's samples (the Debug panel raises it the same way).
    Repair(pathTracing.sampleTarget, std::clamp(pathTracing.sampleTarget, pathTracing.samplesPerFrame, PT::MaximumSampleTarget),
        "rendering.pathTracing.quality.sampleTarget must be from samplesPerFrame to 16777216; clamped.", warnings, valid);

    if(!IsKnown(kPsrModes, pathTracing.psrMode))
    {
        Repair(pathTracing.psrMode, pathTracingDefaults.psrMode, "rendering.pathTracing.psr.mode was invalid; using twoBounce.",
            warnings, valid);
    }

    if(!IsKnown(kPsrDebugViews, pathTracing.psrDebugView))
    {
        Repair(pathTracing.psrDebugView, pathTracingDefaults.psrDebugView,
            "rendering.pathTracing.psr.debugView was invalid; using off.", warnings, valid);
    }

    if(!IsKnown(kDlssQualities, pathTracing.dlssQuality))
    {
        Repair(pathTracing.dlssQuality, pathTracingDefaults.dlssQuality,
            "rendering.pathTracing.dlss.quality was invalid; using quality.", warnings, valid);
    }

    if(!IsKnown(kDenoisers, pathTracing.denoiser))
    {
        Repair(pathTracing.denoiser, pathTracingDefaults.denoiser,
            "rendering.pathTracing.denoising.denoiser was invalid; using nrdHybrid.", warnings, valid);
    }

    if(!IsKnown(kDenoiserViews, pathTracing.denoiserView))
    {
        Repair(pathTracing.denoiserView, pathTracingDefaults.denoiserView,
            "rendering.pathTracing.denoising.view was invalid; using normal.", warnings, valid);
    }

    if(!IsKnown(kDenoiserSignals, pathTracing.denoiserSignal))
    {
        Repair(pathTracing.denoiserSignal, pathTracingDefaults.denoiserSignal,
            "rendering.pathTracing.denoising.signal was invalid; using diffuse.", warnings, valid);
    }

    Repair(pathTracing.viewExposureStops, ClampFloat(pathTracing.viewExposureStops, PT::MinimumViewExposureStops,
        PT::MaximumViewExposureStops, pathTracingDefaults.viewExposureStops),
        "rendering.pathTracing.denoising.viewExposureStops was outside -8 to 16; clamped.", warnings, valid);
    Repair(pathTracing.differenceAmplification, ClampFloat(pathTracing.differenceAmplification, PT::MinimumDifferenceAmplification,
        PT::MaximumDifferenceAmplification, pathTracingDefaults.differenceAmplification),
        "rendering.pathTracing.denoising.differenceAmplification was outside 1-1024; clamped.", warnings, valid);
    Repair(pathTracing.uncertaintyFloorLog10, ClampFloat(pathTracing.uncertaintyFloorLog10, PT::MinimumUncertaintyFloorLog10,
        PT::MaximumUncertaintyFloorLog10, pathTracingDefaults.uncertaintyFloorLog10),
        "rendering.pathTracing.denoising.uncertaintyFloorLog10 was outside -6 to -1; clamped.", warnings, valid);

    // Enabling one caustic technique turns the other off in the Debug panel, so both can never be
    // requested together; a file that asks for both keeps dedicated caustics.
    if(pathTracing.dedicatedCaustics && pathTracing.pixelCentredCaustics)
    {
        Repair(pathTracing.pixelCentredCaustics, false,
            "rendering.pathTracing.caustics: dedicated and pixelCentred are mutually exclusive; pixelCentred turned off.",
            warnings, valid);
    }

    return valid;
}

std::string_view ToSerializedName(ProjectLightingMode value)
{
    return NameOf(kLightingModes, value);
}

std::string_view ToSerializedName(ProjectToneMapper value)
{
    return NameOf(kToneMappers, value);
}

std::string_view ToSerializedName(ProjectReflectionDenoiser value)
{
    return NameOf(kReflectionDenoisers, value);
}

std::string_view ToSerializedName(ProjectPsrMode value)
{
    return NameOf(kPsrModes, value);
}

std::string_view ToSerializedName(ProjectPsrDebugView value)
{
    return NameOf(kPsrDebugViews, value);
}

std::string_view ToSerializedName(ProjectDlssQuality value)
{
    return NameOf(kDlssQualities, value);
}

std::string_view ToSerializedName(ProjectPathTraceDenoiser value)
{
    return NameOf(kDenoisers, value);
}

std::string_view ToSerializedName(ProjectPathTraceDenoiserView value)
{
    return NameOf(kDenoiserViews, value);
}

std::string_view ToSerializedName(ProjectPathTraceDenoiserSignal value)
{
    return NameOf(kDenoiserSignals, value);
}

namespace ProjectSettingsFile
{
    LoadedProjectSettings Parse(std::string_view text)
    {
        using Status = PersonalSettingsFile::Status;
        LoadedProjectSettings loaded;
        loaded.status = Status::Malformed;
        nlohmann::json document = nlohmann::json::parse(text, nullptr, false);

        if(document.is_discarded() || !document.is_object())
        {
            loaded.warnings.push_back("ProjectSettings.json is not a JSON object; using default settings. "
                "The file is kept, and moved aside to ProjectSettings.json.invalid on the next save.");
            return loaded;
        }

        const auto format = document.find("format");

        if(format == document.end() || !format->is_string() || format->get<std::string>() != LoadedProjectSettings::kFormatName)
        {
            loaded.warnings.push_back("ProjectSettings.json is not an EngineProjectSettings file; using default settings.");
            return loaded;
        }

        const auto version = document.find("version");

        if(version == document.end() || !version->is_number_integer() || version->get<long long>() < 1)
        {
            loaded.warnings.push_back("ProjectSettings.json has no valid version; using default settings.");
            return loaded;
        }

        if(version->get<long long>() > LoadedProjectSettings::kCurrentVersion)
        {
            // Never interpreted and never replaced: a newer Engine owns this file.
            loaded.status = Status::UnsupportedVersion;
            loaded.warnings.push_back("ProjectSettings.json version " + std::to_string(version->get<long long>()) +
                " was written by a newer Engine; using default settings read-only for this session.");
            return loaded;
        }

        loaded.status = Status::Loaded;
        const FieldReader root(&document, "ProjectSettings", loaded.warnings);
        ReadRendering(root.Child("rendering"), loaded.settings.rendering);
        ValidateProjectSettings(loaded.settings, &loaded.warnings);
        loaded.document = std::move(document);
        return loaded;
    }

    LoadedProjectSettings Load(const std::filesystem::path& path)
    {
        std::string contents;
        std::string error;
        const auto status = PersonalSettingsFile::Read(path, contents, error);

        if(status != PersonalSettingsFile::Status::Loaded)
        {
            LoadedProjectSettings loaded;
            loaded.status = status;

            if(status != PersonalSettingsFile::Status::Missing)
            {
                loaded.warnings.push_back("Cannot read ProjectSettings.json (" + error + "); using default settings read-only.");
            }

            return loaded;
        }

        return Parse(contents);
    }

    std::string Serialize(const ProjectSettings& settings, const nlohmann::json& retainedDocument)
    {
        nlohmann::json document = retainedDocument.is_object() ? retainedDocument : nlohmann::json::object();
        document["format"] = LoadedProjectSettings::kFormatName;
        document["version"] = LoadedProjectSettings::kCurrentVersion;

        const auto& source = settings.rendering;
        auto& rendering = ObjectMember(document, "rendering");
        rendering["lightingMode"] = ToSerializedName(source.lightingMode);
        rendering["toneMapper"] = ToSerializedName(source.toneMapper);

        auto& legacy = ObjectMember(rendering, "legacy");
        legacy["reflectionDenoiser"] = ToSerializedName(source.legacy.reflectionDenoiser);
        legacy["contributionLimiter"] = source.legacy.contributionLimiter;
        legacy["rtgi"] = source.legacy.rtgi;
        legacy["rtgiContactAOStrength"] = source.legacy.rtgiContactAOStrength;
        legacy["rtgiContactAORadius"] = source.legacy.rtgiContactAORadius;

        const auto& pt = source.pathTracing;
        auto& pathTracing = ObjectMember(rendering, "pathTracing");
        auto& quality = ObjectMember(pathTracing, "quality");
        quality["samplesPerFrame"] = pt.samplesPerFrame;
        quality["maxPathDepth"] = pt.maxPathDepth;
        quality["rouletteFromDepth"] = pt.rouletteFromDepth;
        quality["sampleTarget"] = pt.sampleTarget;

        auto& psr = ObjectMember(pathTracing, "psr");
        psr["mode"] = ToSerializedName(pt.psrMode);
        psr["debugView"] = ToSerializedName(pt.psrDebugView);

        auto& dlss = ObjectMember(pathTracing, "dlss");
        dlss["enabled"] = pt.dlss;
        dlss["quality"] = ToSerializedName(pt.dlssQuality);

        auto& denoising = ObjectMember(pathTracing, "denoising");
        denoising["cameraJitter"] = pt.cameraJitter;
        denoising["denoiser"] = ToSerializedName(pt.denoiser);
        denoising["view"] = ToSerializedName(pt.denoiserView);
        denoising["signal"] = ToSerializedName(pt.denoiserSignal);
        denoising["viewExposureStops"] = pt.viewExposureStops;
        denoising["differenceAmplification"] = pt.differenceAmplification;
        denoising["uncertaintyFloorLog10"] = pt.uncertaintyFloorLog10;

        ObjectMember(pathTracing, "sharc")["enabled"] = pt.sharc;

        auto& caustics = ObjectMember(pathTracing, "caustics");
        caustics["dedicated"] = pt.dedicatedCaustics;
        caustics["causticOnlyView"] = pt.causticOnlyView;
        caustics["pixelCentred"] = pt.pixelCentredCaustics;

        return document.dump(4) + "\n";
    }

    bool Save(const std::filesystem::path& path, LoadedProjectSettings& loaded, ProjectSettings settings,
        std::string& errorMessage)
    {
        errorMessage.clear();

        if(loaded.IsReadOnly())
        {
            errorMessage = std::string("ProjectSettings.json is read-only this session (") +
                PersonalSettingsFile::Describe(loaded.status) + "); it was not changed.";
            return false;
        }

        ValidateProjectSettings(settings);
        const std::string contents = Serialize(settings, loaded.document);

        // Preserves a malformed original as .invalid, then replaces the file atomically.
        if(!PersonalSettingsFile::Save(path, loaded.status, contents, errorMessage))
        {
            return false;
        }

        loaded.settings = settings;
        loaded.status = PersonalSettingsFile::Status::Loaded;
        loaded.warnings.clear();
        loaded.document = nlohmann::json::parse(contents);
        return true;
    }
}
