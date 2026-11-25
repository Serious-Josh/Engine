#pragma once

// Physical sky S0 (Documentation/Rendering/Physical_Sky_Design_Proposal.md §4): the scene's sky type and its physically based
// sky, a Hillaire (EGSR 2020) atmosphere of air (Rayleigh), aerosols (Mie) and ozone over a planet. Stored in the scene's "Sky"
// environment group (SceneDocument::skyType and SceneDocument::physicalSky; World-owned with Local overrides), in scene JSON from
// schema 31 and in cooked scenes from payload 38.
//
// Units: the settings store physical values only. Scattering and absorption coefficients are per metre (the Editor shows them
// as 10^-6 m^-1, and HDRP's density and tint as exact views of them, §4.12.3); scale heights, the planet radius, the atmosphere
// height and the ozone layer are kilometres; the sea level is metres of world Y. The renderer converts once, when it fills the
// atmosphere constants.
//
// Like EnvironmentalHazeSettings, invalid values are refused, never clamped (ValidatePhysicalSkySettings).

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Which sky lights a scene. Neutral is Engine's uniform grey fallback; Hdri is the scene's environmentReference (an HDRI,
// builtin:none or, when empty, the neutral environment, exactly as before the physical sky); Physical is the scene's
// PhysicalSkySettings, baked by the renderer's SkyAtmosphere.
enum class SkyType : std::uint8_t
{
    Neutral,
    Hdri,
    Physical
};

// HDRP's Model presets. They decide which settings the Editor shows; the renderer reads the stored values only.
enum class PhysicalSkyModel : std::uint8_t
{
    EarthSimple,
    EarthAdvanced,
    CustomPlanet
};

// The sky luminance factor (§4.2): 2^exposureCompensation (Exposure) or multiplier (Multiplier).
enum class PhysicalSkyIntensityMode : std::uint8_t
{
    Exposure,
    Multiplier
};

// HDRP's update modes (§4.9). Stored and serialized from S0; the renderer re-bakes on every change until S4.
enum class PhysicalSkyUpdateMode : std::uint8_t
{
    OnChanged,
    OnDemand,
    Realtime
};

// S5 (Physical_Sky_Design_Proposal.md §14.2): one layer of clouds. A Layer is a 2D sheet at altitudeKm (HDRP's Cloud Layer); a
// Volumetric layer fills the spherical shell from altitudeKm to altitudeKm + thicknessKm (HDRP's Volumetric Clouds). The control mode
// only decides what the Sky window shows. Presets are data (assets/Sky/CloudPresets.json); the layer stores the expanded values.
enum class CloudLayerKind : std::uint8_t
{
    Layer,
    Volumetric
};

enum class CloudControlMode : std::uint8_t
{
    Simple,
    Advanced,
    Manual
};

struct CloudLayerSettings
{
    static constexpr float MaximumAltitudeKm = 50.0f;
    static constexpr float MinimumThicknessKm = 0.05f;
    static constexpr float MaximumThicknessKm = 20.0f;
    static constexpr float MaximumDensity = 1000.0f;
    static constexpr float MinimumShapeScaleKm = 0.1f;
    static constexpr float MaximumShapeScaleKm = 1000.0f;
    static constexpr float MinimumDetailScaleKm = 0.01f;
    static constexpr float MaximumDetailScaleKm = 100.0f;
    static constexpr float MaximumPhase = 0.99f;
    static constexpr float MaximumAmbientStrength = 4.0f;
    static constexpr float MaximumWindSpeed = 200.0f;
    static constexpr float MinimumCoverageMapExtentKm = 0.1f;
    static constexpr float MaximumCoverageMapExtentKm = 10000.0f;
    static constexpr std::size_t MaximumPresetNameLength = 64;

    CloudLayerKind kind = CloudLayerKind::Volumetric;
    CloudControlMode controlMode = CloudControlMode::Simple;
    // The preset last applied (a label; "Custom" once edited away from it in the Editor).
    std::string preset = "Cloudy";
    // The layer's bottom (a Layer's sheet) above sea level, and (Volumetric) its thickness; a Layer has thickness 0.
    float altitudeKm = 1.5f;
    float thicknessKm = 2.5f;
    // The share of the sky the layer covers, 0-1.
    float coverage = 0.45f;
    // Volumetric: extinction at full density, per km. Layer: optical depth of the sheet at full coverage. The defaults are the
    // Cloudy preset's (2026-11-25: 120 /km, real cumulus: its densest extinction is 0.064 /m).
    float density = 120.0f;
    // The noise: the shape's period and the detail's period (km), the detail's erosion (0-1) and the shape factor (0: flat
    // bottoms and tops, 1: rounded, anvil-like tops).
    float shapeScaleKm = 12.0f;
    float detailScaleKm = 1.0f;
    float erosion = 0.35f;
    float shapeFactor = 0.6f;
    // Lighting: single-scattering albedo, a dual-lobe Henyey-Greenstein phase (forward g, backward g, forward lobe's weight), the
    // multiple-scattering octaves' strength (0-1) and the ambient sky light's strength.
    std::array<float, 3> albedo{0.95f, 0.95f, 0.95f};
    float phaseForward = 0.75f;
    float phaseBackward = -0.25f;
    float phaseBlend = 0.6f;
    float multipleScattering = 0.6f;
    float ambientStrength = 1.0f;
    // Wind: the direction the clouds move toward (degrees from +x toward +z) and their speed.
    float windDirectionDegrees = 0.0f;
    float windSpeedMetersPerSecond = 8.0f;
    // Whether the layer darkens the sun (the cloud shadow map).
    bool castShadows = true;
    // The coverage/weather map slot (§14.7, step 1): an optional asset reference and the world extent it covers. Empty: the
    // coverage is procedural. S5 stores the slot; loading a map into it is the first growth step.
    std::string coverageMap;
    float coverageMapExtentKm = 50.0f;

    bool operator==(const CloudLayerSettings&) const = default;
};

struct PhysicalSkySettings
{
    static constexpr float MinimumPlanetRadiusKm = 1.0f;
    static constexpr float MaximumPlanetRadiusKm = 100000.0f;
    static constexpr float MaximumSeaLevelMeters = 100000.0f;
    static constexpr float MinimumAtmosphereHeightKm = 1.0f;
    static constexpr float MaximumAtmosphereHeightKm = 1000.0f;
    // Per metre: 0.01 m^-1 is an optical depth of 80 over Earth's 8 km Rayleigh scale height, far denser than any sky.
    static constexpr float MaximumCoefficientPerMeter = 0.01f;
    static constexpr float MinimumScaleHeightKm = 0.001f;
    static constexpr float MaximumScaleHeightKm = 1000.0f;
    // Cornette-Shanks is singular at |g| = 1.
    static constexpr float MaximumAnisotropy = 0.999f;
    static constexpr float MaximumOzoneAltitudeKm = 1000.0f;
    static constexpr float MinimumOzoneLayerWidthKm = 0.001f;
    static constexpr float MaximumMultipleScattering = 2.0f;
    static constexpr float MaximumExposureCompensation = 20.0f;
    static constexpr float MaximumMultiplier = 1000.0f;
    static constexpr float MaximumUpdatePeriodSeconds = 3600.0f;

    // Earth's coefficients (Hillaire 2020; §3.3), per metre: the defaults and what Reset to defaults restores.
    static constexpr std::array<float, 3> EarthRayleighScattering{5.802e-6f, 13.558e-6f, 33.1e-6f};
    static constexpr std::array<float, 3> EarthMieScattering{3.996e-6f, 3.996e-6f, 3.996e-6f};
    static constexpr std::array<float, 3> EarthMieAbsorption{4.40e-6f, 4.40e-6f, 4.40e-6f};
    static constexpr std::array<float, 3> EarthOzoneAbsorption{0.650e-6f, 1.881e-6f, 0.085e-6f};

    PhysicalSkyModel model = PhysicalSkyModel::EarthSimple;

    // Planet: a sphere whose top is at world y = seaLevelMeters (Y up), with an atmosphere atmosphereHeightKm thick above it.
    float planetRadiusKm = 6360.0f;
    float seaLevelMeters = 0.0f;
    float atmosphereHeightKm = 100.0f;
    // Lambertian albedo of the planet ground (HDRP's Ground Tint).
    std::array<float, 3> groundAlbedo{0.3f, 0.3f, 0.3f};

    // Air: Rayleigh scattering with an exponential density of scale height rayleighScaleHeightKm. Earth's air does not absorb;
    // rayleighAbsorption lets HDRP's Air Tint (single-scattering albedo) below 1 be stored exactly.
    std::array<float, 3> rayleighScattering = EarthRayleighScattering;
    std::array<float, 3> rayleighAbsorption{0.0f, 0.0f, 0.0f};
    float rayleighScaleHeightKm = 8.0f;

    // Aerosols: Mie scattering and absorption with an exponential density; mieAnisotropy is the Cornette-Shanks g.
    std::array<float, 3> mieScattering = EarthMieScattering;
    std::array<float, 3> mieAbsorption = EarthMieAbsorption;
    float mieAnisotropy = 0.8f;
    float mieScaleHeightKm = 1.2f;

    // Ozone: absorption only, in a tent that starts at ozoneMinimumAltitudeKm and peaks at its middle,
    // ozoneMinimumAltitudeKm + ozoneLayerWidthKm / 2 (HDRP's definition; Earth: 10 km and 30 km, the peak at 25 km).
    std::array<float, 3> ozoneAbsorption = EarthOzoneAbsorption;
    float ozoneMinimumAltitudeKm = 10.0f;
    float ozoneLayerWidthKm = 30.0f;

    // Scales the multiple-scattering term (Unreal's factor; 1 is physical).
    float multipleScattering = 1.0f;

    // Aerial perspective on surfaces (S3); stored from S0.
    bool atmosphericScattering = true;
    // Off: the lower hemisphere of the baked environment is black (scenes with their own terrain).
    bool showGroundBelowHorizon = true;
    // Off: directions below the horizon keep their radiance but get no importance mass (§4.5.2).
    bool sampleBelowHorizon = false;

    PhysicalSkyIntensityMode intensityMode = PhysicalSkyIntensityMode::Exposure;
    // EV: the sky luminance factor is 2^exposureCompensation in Exposure mode.
    float exposureCompensation = 0.0f;
    float multiplier = 1.0f;

    // Artistic overrides (HDRP; non-physical). Stored and serialized from S0; the renderer applies them from S4.
    float colorSaturation = 1.0f;
    std::array<float, 3> horizonTint{1.0f, 1.0f, 1.0f};
    std::array<float, 3> zenithTint{1.0f, 1.0f, 1.0f};
    float horizonZenithShift = 0.0f;

    // Stored and serialized from S0; used from S4.
    PhysicalSkyUpdateMode updateMode = PhysicalSkyUpdateMode::OnChanged;
    // Realtime only: seconds between re-bakes (0: every frame).
    float updatePeriodSeconds = 0.0f;

    // S5 (§14.1): the night sky. Space emission (stars and airglow) times nightSkyIntensity; the airglow's zenith luminance
    // (cd/m^2); and HDRP's Planet Rotation, which turns the stars about the planet's axis (degrees).
    static constexpr float MaximumNightSkyIntensity = 1000.0f;
    static constexpr float MaximumAirglowLuminance = 10.0f;
    static constexpr float MaximumPlanetRotationDegrees = 360.0f;
    float nightSkyIntensity = 1.0f;
    float airglowLuminance = 0.001f;
    float planetRotationDegrees = 0.0f;

    // S5 (§14.2): the cloud layers, at most MaximumCloudLayers, bottom to top and not overlapping. Clouds render with the
    // physically based sky (they are lit through its atmosphere).
    static constexpr std::size_t MaximumCloudLayers = 4;
    std::vector<CloudLayerSettings> cloudLayers;

    bool operator==(const PhysicalSkySettings&) const = default;
};

// The two stored values of the Sky group that the Sky window edits (SceneDocument::skyType and physicalSky), as one edit's value.
// The HDRI reference and the environment luminance stay where they were edited before (the runtime Scene).
struct SceneSky
{
    // Absent: derived from the environment reference (ResolveSkyType).
    std::optional<SkyType> type;
    std::optional<PhysicalSkySettings> physical;

    bool operator==(const SceneSky&) const = default;
};

// The sky type of a scene that stores none (a document before schema 31, a cooked payload before 38, or any scene whose sky type
// was never chosen): an HDRI reference (or builtin:none) is Hdri, no reference is Neutral, exactly as those scenes always rendered.
// Nothing else is ever derived, so no scene is silently converted.
inline SkyType DeriveSkyType(const std::string& environmentReference)
{
    return environmentReference.empty() ? SkyType::Neutral : SkyType::Hdri;
}

// The sky type a stored (possibly absent) type and its environment reference stand for.
inline SkyType ResolveSkyType(const std::optional<SkyType>& storedType, const std::string& environmentReference)
{
    return storedType ? *storedType : DeriveSkyType(environmentReference);
}

inline bool IsValidSkyType(SkyType type)
{
    return type == SkyType::Neutral || type == SkyType::Hdri || type == SkyType::Physical;
}

// The sky luminance factor (§4.2), applied to the baked environment (and, from S3, to the visible sky).
inline double PhysicalSkyLuminanceFactor(const PhysicalSkySettings& settings)
{
    if(settings.intensityMode == PhysicalSkyIntensityMode::Multiplier)
    {
        return double(settings.multiplier);
    }

    return std::exp2(double(settings.exposureCompensation));
}

// HDRP's views of the stored coefficients (§4.12.3), per channel: density is the optical depth at the zenith of an exponential
// medium, sigma_t * H (H in metres); tint is the single-scattering albedo sigma_s / sigma_t (1 for a medium that does not
// extinguish, so the view is defined everywhere).
inline double PhysicalSkyZenithDensity(float scattering, float absorption, float scaleHeightKm)
{
    return (double(scattering) + double(absorption)) * double(scaleHeightKm) * 1000.0;
}

inline double PhysicalSkyTint(float scattering, float absorption)
{
    const double extinction = double(scattering) + double(absorption);
    return extinction > 0.0 ? double(scattering) / extinction : 1.0;
}

// The coefficients of a density and tint view: sigma_t = density / H, sigma_s = tint * sigma_t, sigma_a = (1 - tint) * sigma_t.
inline void PhysicalSkyCoefficientsFromView(double density, double tint, float scaleHeightKm, float& scattering, float& absorption)
{
    const double extinction = density / (double(scaleHeightKm) * 1000.0);
    scattering = float(tint * extinction);
    absorption = float((1.0 - tint) * extinction);
}

// Every value finite and inside its range, every enum known. Invalid values are refused, never clamped.
inline bool ValidatePhysicalSkySettings(const PhysicalSkySettings& settings, std::string* error = nullptr)
{
    const auto fail = [&](const char* message)
    {
        if(error != nullptr)
        {
            *error = message;
        }

        return false;
    };

    const auto within = [](float value, float minimum, float maximum)
    {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    };

    const auto channelsWithin = [&](const std::array<float, 3>& channels, float minimum, float maximum)
    {
        for(const float channel : channels)
        {
            if(!within(channel, minimum, maximum))
            {
                return false;
            }
        }

        return true;
    };

    const bool knownModel = settings.model == PhysicalSkyModel::EarthSimple || settings.model == PhysicalSkyModel::EarthAdvanced ||
        settings.model == PhysicalSkyModel::CustomPlanet;
    const bool knownIntensity = settings.intensityMode == PhysicalSkyIntensityMode::Exposure ||
        settings.intensityMode == PhysicalSkyIntensityMode::Multiplier;
    const bool knownUpdate = settings.updateMode == PhysicalSkyUpdateMode::OnChanged ||
        settings.updateMode == PhysicalSkyUpdateMode::OnDemand || settings.updateMode == PhysicalSkyUpdateMode::Realtime;

    if(!knownModel || !knownIntensity || !knownUpdate)
    {
        return fail("Physical sky model, intensity mode and update mode must be known values.");
    }

    if(!within(settings.planetRadiusKm, PhysicalSkySettings::MinimumPlanetRadiusKm, PhysicalSkySettings::MaximumPlanetRadiusKm))
    {
        return fail("Planet radius must be finite, from 1 to 100,000 km.");
    }

    if(!within(settings.seaLevelMeters, -PhysicalSkySettings::MaximumSeaLevelMeters, PhysicalSkySettings::MaximumSeaLevelMeters))
    {
        return fail("Sea level must be finite, within 100 km of 0.");
    }

    if(!within(settings.atmosphereHeightKm, PhysicalSkySettings::MinimumAtmosphereHeightKm,
        PhysicalSkySettings::MaximumAtmosphereHeightKm))
    {
        return fail("Atmosphere height must be finite, from 1 to 1,000 km.");
    }

    if(!channelsWithin(settings.groundAlbedo, 0.0f, 1.0f))
    {
        return fail("Ground tint channels must be from 0 to 1.");
    }

    const float maximumCoefficient = PhysicalSkySettings::MaximumCoefficientPerMeter;

    if(!channelsWithin(settings.rayleighScattering, 0.0f, maximumCoefficient) ||
        !channelsWithin(settings.rayleighAbsorption, 0.0f, maximumCoefficient) ||
        !channelsWithin(settings.mieScattering, 0.0f, maximumCoefficient) ||
        !channelsWithin(settings.mieAbsorption, 0.0f, maximumCoefficient) ||
        !channelsWithin(settings.ozoneAbsorption, 0.0f, maximumCoefficient))
    {
        return fail("Scattering and absorption coefficients must be finite, from 0 to 10,000 x 10^-6 per metre.");
    }

    if(!within(settings.rayleighScaleHeightKm, PhysicalSkySettings::MinimumScaleHeightKm, PhysicalSkySettings::MaximumScaleHeightKm) ||
        !within(settings.mieScaleHeightKm, PhysicalSkySettings::MinimumScaleHeightKm, PhysicalSkySettings::MaximumScaleHeightKm))
    {
        return fail("Air and aerosol scale heights must be finite, from 0.001 to 1,000 km.");
    }

    if(!within(settings.mieAnisotropy, -PhysicalSkySettings::MaximumAnisotropy, PhysicalSkySettings::MaximumAnisotropy))
    {
        return fail("Aerosol anisotropy must be from -0.999 to 0.999.");
    }

    if(!within(settings.ozoneMinimumAltitudeKm, 0.0f, PhysicalSkySettings::MaximumOzoneAltitudeKm) ||
        !within(settings.ozoneLayerWidthKm, PhysicalSkySettings::MinimumOzoneLayerWidthKm, PhysicalSkySettings::MaximumOzoneAltitudeKm))
    {
        return fail("Ozone minimum altitude must be from 0 to 1,000 km and its layer width from 0.001 to 1,000 km.");
    }

    if(!within(settings.multipleScattering, 0.0f, PhysicalSkySettings::MaximumMultipleScattering))
    {
        return fail("Multiple scattering must be from 0 to 2.");
    }

    if(!within(settings.exposureCompensation, -PhysicalSkySettings::MaximumExposureCompensation,
        PhysicalSkySettings::MaximumExposureCompensation))
    {
        return fail("Sky exposure compensation must be from -20 to 20 EV.");
    }

    if(!within(settings.multiplier, 0.0f, PhysicalSkySettings::MaximumMultiplier))
    {
        return fail("Sky multiplier must be from 0 to 1,000.");
    }

    if(!within(settings.colorSaturation, 0.0f, 1.0f) || !channelsWithin(settings.horizonTint, 0.0f, 1.0f) ||
        !channelsWithin(settings.zenithTint, 0.0f, 1.0f))
    {
        return fail("Color saturation and the horizon and zenith tint channels must be from 0 to 1.");
    }

    if(!within(settings.horizonZenithShift, -1.0f, 1.0f))
    {
        return fail("Horizon zenith shift must be from -1 to 1.");
    }

    if(!within(settings.updatePeriodSeconds, 0.0f, PhysicalSkySettings::MaximumUpdatePeriodSeconds))
    {
        return fail("Sky update period must be from 0 to 3,600 seconds.");
    }

    if(!within(settings.nightSkyIntensity, 0.0f, PhysicalSkySettings::MaximumNightSkyIntensity) ||
        !within(settings.airglowLuminance, 0.0f, PhysicalSkySettings::MaximumAirglowLuminance) ||
        !within(settings.planetRotationDegrees, -PhysicalSkySettings::MaximumPlanetRotationDegrees,
            PhysicalSkySettings::MaximumPlanetRotationDegrees))
    {
        return fail("Night sky intensity must be from 0 to 1,000, airglow from 0 to 10 cd/m^2 and planet rotation from -360 to 360 "
            "degrees.");
    }

    if(settings.cloudLayers.size() > PhysicalSkySettings::MaximumCloudLayers)
    {
        return fail("A sky has at most 4 cloud layers.");
    }

    float previousTop = -1.0f;

    for(const CloudLayerSettings& layer : settings.cloudLayers)
    {
        using C = CloudLayerSettings;
        const bool knownKind = layer.kind == CloudLayerKind::Layer || layer.kind == CloudLayerKind::Volumetric;
        const bool knownMode = layer.controlMode == CloudControlMode::Simple || layer.controlMode == CloudControlMode::Advanced ||
            layer.controlMode == CloudControlMode::Manual;

        if(!knownKind || !knownMode || layer.preset.size() > C::MaximumPresetNameLength)
        {
            return fail("A cloud layer's kind and control mode must be known values and its preset name at most 64 characters.");
        }

        const bool thicknessValid = layer.kind == CloudLayerKind::Layer ? layer.thicknessKm == 0.0f :
            within(layer.thicknessKm, C::MinimumThicknessKm, C::MaximumThicknessKm);

        if(!within(layer.altitudeKm, 0.0f, C::MaximumAltitudeKm) || !thicknessValid)
        {
            return fail("A cloud layer's altitude must be from 0 to 50 km; a Volumetric layer's thickness from 0.05 to 20 km, a "
                "Layer's 0.");
        }

        if(layer.altitudeKm < previousTop || layer.altitudeKm + layer.thicknessKm > settings.atmosphereHeightKm)
        {
            return fail("Cloud layers must be ordered bottom to top, must not overlap, and must stay inside the atmosphere.");
        }

        previousTop = layer.altitudeKm + layer.thicknessKm;

        if(!within(layer.coverage, 0.0f, 1.0f) || !within(layer.density, 0.0f, C::MaximumDensity))
        {
            return fail("A cloud layer's coverage must be from 0 to 1 and its density from 0 to 1,000.");
        }

        if(!within(layer.shapeScaleKm, C::MinimumShapeScaleKm, C::MaximumShapeScaleKm) ||
            !within(layer.detailScaleKm, C::MinimumDetailScaleKm, C::MaximumDetailScaleKm) || !within(layer.erosion, 0.0f, 1.0f) ||
            !within(layer.shapeFactor, 0.0f, 1.0f))
        {
            return fail("A cloud layer's shape scale must be from 0.1 to 1,000 km, its detail scale from 0.01 to 100 km, and its "
                "erosion and shape factor from 0 to 1.");
        }

        if(!channelsWithin(layer.albedo, 0.0f, 1.0f) || !within(layer.phaseForward, 0.0f, C::MaximumPhase) ||
            !within(layer.phaseBackward, -C::MaximumPhase, 0.0f) || !within(layer.phaseBlend, 0.0f, 1.0f) ||
            !within(layer.multipleScattering, 0.0f, 1.0f) || !within(layer.ambientStrength, 0.0f, C::MaximumAmbientStrength))
        {
            return fail("A cloud layer's albedo, phase (forward 0 to 0.99, backward -0.99 to 0, blend 0 to 1), multiple scattering "
                "(0 to 1) and ambient strength (0 to 4) are out of range.");
        }

        if(!within(layer.windDirectionDegrees, -360.0f, 360.0f) || !within(layer.windSpeedMetersPerSecond, 0.0f, C::MaximumWindSpeed) ||
            !within(layer.coverageMapExtentKm, C::MinimumCoverageMapExtentKm, C::MaximumCoverageMapExtentKm))
        {
            return fail("A cloud layer's wind direction must be from -360 to 360 degrees, its speed from 0 to 200 m/s and its "
                "coverage map extent from 0.1 to 10,000 km.");
        }
    }

    return true;
}
