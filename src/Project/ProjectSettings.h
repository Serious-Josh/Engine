#pragma once

#include "Project/PersonalSettingsFile.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Shared project configuration (PS.1): how the game renders and behaves, saved per project in
// <ProjectRoot>/ProjectSettings.json and later cooked into packaged games. Editor-only preferences
// never belong here (EditorPreferences and ProjectWorkspace hold those).
//
// The model is independent of the Editor and renderer: its enums are the project-facing choices,
// serialized by stable names, and PS.2 maps them onto renderer configuration. Every default equals
// Engine's behaviour before Project Settings existed, so a project without the file renders as before.

enum class ProjectLightingMode
{
    Legacy,
    ReferencePathTracer,
    // Raster primary + ReSTIR DI. The standalone raster-primary mode is internal and not selectable.
    PathTracing
};

// AgX is the operator the renderer currently calls ToneMapOperator::Existing.
enum class ProjectToneMapper
{
    AgX,
    GT7
};

enum class ProjectReflectionDenoiser
{
    Nrd,
    Native
};

enum class ProjectPsrMode
{
    Off,
    SingleBounce,
    // Falls back to single-bounce at runtime when the max path depth is below 3.
    TwoBounce
};

enum class ProjectPsrDebugView
{
    Off,
    ReplacementMask,
    ReflectedNormals,
    ReflectedAlbedo
};

enum class ProjectDlssQuality
{
    Quality,
    Balanced,
    Performance
};

enum class ProjectPathTraceDenoiser
{
    Off,
    Engine,
    NrdReblur,
    NrdRelax,
    NrdHybrid,
    // RR.1D: experimental DLSS Ray Reconstruction (Editor only). The path tracer runs as for NrdHybrid, whose signal extraction RR
    // reads; RR replaces NRD and DLSS Super Resolution. dlss and dlssQuality select its preset (dlss off: DLAA). Unavailable or
    // failed, NRD Hybrid renders (plus Super Resolution at a preset); packaged games always render NRD Hybrid.
    DlssRayReconstruction
};

// M5.1: Path Tracing's light-transport estimator. ReSTIRDI is production (ReSTIR DI direct light plus the indirect path tracer);
// ReSTIRPTEnhanced is the experimental unified ReSTIR PT Enhanced renderer (configuration B), Editor-only: packaged games
// always render ReSTIRDI.
enum class ProjectPathTraceEstimator
{
    ReSTIRDI,
    ReSTIRPTEnhanced
};

enum class ProjectPathTraceDenoiserView
{
    Normal,
    Temporal,
    Spatial,
    SpatialTemporalDifference,
    EstimatedUncertainty,
    HistoryLength,
    RawExtraction
};

enum class ProjectPathTraceDenoiserSignal
{
    Diffuse,
    Specular
};

struct LegacyRenderingSettings
{
    static constexpr float MinimumContactAOStrength = 0.0f;
    static constexpr float MaximumContactAOStrength = 1.0f;
    static constexpr float MinimumContactAORadius = 0.05f;
    static constexpr float MaximumContactAORadius = 5.0f;

    ProjectReflectionDenoiser reflectionDenoiser = ProjectReflectionDenoiser::Nrd;
    bool contributionLimiter = true;
    bool rtgi = true;
    float rtgiContactAOStrength = 1.0f;
    float rtgiContactAORadius = 2.5f;

    bool operator==(const LegacyRenderingSettings&) const = default;
};

struct PathTracingRenderingSettings
{
    // The ranges of PathTraceReferenceSettings / IsValidPathTraceReferenceSettings.
    static constexpr std::uint32_t MaximumSamplesPerFrame = 64;
    static constexpr std::uint32_t MaximumPathDepth = 64;
    static constexpr std::uint32_t MaximumRouletteDepth = MaximumPathDepth + 1;
    static constexpr std::uint32_t MaximumSampleTarget = 1u << 24;
    static constexpr float MinimumViewExposureStops = -8.0f;
    static constexpr float MaximumViewExposureStops = 16.0f;
    static constexpr float MinimumDifferenceAmplification = 1.0f;
    static constexpr float MaximumDifferenceAmplification = 1024.0f;
    static constexpr float MinimumUncertaintyFloorLog10 = -6.0f;
    static constexpr float MaximumUncertaintyFloorLog10 = -1.0f;

    // Quality.
    std::uint32_t samplesPerFrame = 1;
    std::uint32_t maxPathDepth = 3;
    std::uint32_t rouletteFromDepth = 3;
    // At least samplesPerFrame.
    std::uint32_t sampleTarget = 1u << 16;

    ProjectPsrMode psrMode = ProjectPsrMode::TwoBounce;
    ProjectPsrDebugView psrDebugView = ProjectPsrDebugView::Off;

    bool dlss = false;
    ProjectDlssQuality dlssQuality = ProjectDlssQuality::Quality;

    bool cameraJitter = false;
    ProjectPathTraceDenoiser denoiser = ProjectPathTraceDenoiser::NrdHybrid;
    ProjectPathTraceDenoiserView denoiserView = ProjectPathTraceDenoiserView::Normal;
    ProjectPathTraceDenoiserSignal denoiserSignal = ProjectPathTraceDenoiserSignal::Diffuse;
    float viewExposureStops = 0.0f;
    float differenceAmplification = 16.0f;
    float uncertaintyFloorLog10 = -3.0f;

    bool sharc = false;

    // M5.1: serialized as rendering.pathTracing.estimator ("restirDI" | "restirPTEnhanced"); older files keep ReSTIRDI.
    ProjectPathTraceEstimator estimator = ProjectPathTraceEstimator::ReSTIRDI;

    bool operator==(const PathTracingRenderingSettings&) const = default;
};

// R1.3: the volumetric fog's (environmental haze and AIR mist) cost and resolution, in every lighting mode. Low, Medium and High
// are the fog pass's presets (quarter resolution with 32 or 64 steps, half resolution with 96).
enum class ProjectVolumetricQuality
{
    Low,
    Medium,
    High
};

// What the quality's resolution scales: the frame's render extent (lower under DLSS or a render scale) or the output.
enum class ProjectVolumetricResolutionBasis
{
    Internal,
    Output
};

// The defaults are the Editor's and packaged games' behaviour before R1.3.
struct VolumetricRenderingSettings
{
    ProjectVolumetricQuality quality = ProjectVolumetricQuality::Medium;
    ProjectVolumetricResolutionBasis resolutionBasis = ProjectVolumetricResolutionBasis::Internal;

    bool operator==(const VolumetricRenderingSettings&) const = default;
};

// Physical sky S4 (Physical_Sky_Design_Proposal.md §4.11, §4.12.5): an HDRI sky's resolution. The tier sizes the HDRI sky's
// background equirect (its width; height is half) and its GGX reflection cube (a quarter of the width). 1024 uses the sky's two
// persistent environment sets alone; 2048 and 4096 add one fixed background slot. Read once when the renderer starts (a change
// applies from the next launch), because resizing would mean a release.
enum class ProjectHdriSkyQuality
{
    Q1024,
    Q2048,
    Q4096
};

// The tier's background width in texels.
inline unsigned int HdriSkyBackgroundWidth(ProjectHdriSkyQuality quality)
{
    return quality == ProjectHdriSkyQuality::Q4096 ? 4096u : (quality == ProjectHdriSkyQuality::Q2048 ? 2048u : 1024u);
}

// Physical sky S4 (§4.12.5): the sky's environment sets, for every sky type: the equirect width (lighting, importance sampling and
// the HDRI sky's 1024-quality background) and the GGX reflection cube's size. Read once when the renderer starts.
enum class ProjectSkyEnvironmentResolution
{
    R512,
    R1024,
    R2048
};

enum class ProjectSkyCubeSize
{
    C128,
    C256,
    C512
};

inline unsigned int SkyEnvironmentWidth(ProjectSkyEnvironmentResolution resolution)
{
    return resolution == ProjectSkyEnvironmentResolution::R2048 ? 2048u :
        (resolution == ProjectSkyEnvironmentResolution::R512 ? 512u : 1024u);
}

inline unsigned int SkyCubeSize(ProjectSkyCubeSize size)
{
    return size == ProjectSkyCubeSize::C512 ? 512u : (size == ProjectSkyCubeSize::C128 ? 128u : 256u);
}

// Serialized as rendering.sky; an older file keeps 2048, 1024 and 256.
struct SkyRenderingSettings
{
    ProjectHdriSkyQuality hdriQuality = ProjectHdriSkyQuality::Q2048;
    ProjectSkyEnvironmentResolution environmentResolution = ProjectSkyEnvironmentResolution::R1024;
    ProjectSkyCubeSize reflectionCubeSize = ProjectSkyCubeSize::C256;

    bool operator==(const SkyRenderingSettings&) const = default;
};

// NVIDIA Reflex Low Latency, NVIDIA's three user-facing modes. Off (the default) is Engine's behaviour before Reflex.
enum class ProjectReflexMode
{
    Off,
    On,
    OnBoost
};

// Mode-independent. DLSS Frame Generation runs Reflex at On whatever is saved here (ResolveReflexMode); the saved choice
// applies again when Frame Generation is off.
struct LatencyRenderingSettings
{
    ProjectReflexMode reflex = ProjectReflexMode::Off;

    bool operator==(const LatencyRenderingSettings&) const = default;
};

// FG.3: DLSS Frame Generation (2x), mode-independent. Editor viewport only for now: built games keep the setting and ignore
// it until packaged Frame Generation exists. Serialized as rendering.frameGeneration.enabled; an older file keeps it off.
struct FrameGenerationRenderingSettings
{
    bool enabled = false;

    bool operator==(const FrameGenerationRenderingSettings&) const = default;
};

struct RenderingSettings
{
    ProjectLightingMode lightingMode = ProjectLightingMode::Legacy;
    ProjectToneMapper toneMapper = ProjectToneMapper::AgX;
    // Both mode sections are always kept, so switching modes never loses the other mode's values.
    LegacyRenderingSettings legacy;
    PathTracingRenderingSettings pathTracing;
    // R1.3: mode-independent.
    VolumetricRenderingSettings volumetrics;
    // Serialized as rendering.latency; an older file keeps Off.
    LatencyRenderingSettings latency;
    FrameGenerationRenderingSettings frameGeneration;
    // Physical sky S4: mode-independent, read at renderer start.
    SkyRenderingSettings sky;

    bool operator==(const RenderingSettings&) const = default;
};

// SA.1: audio choices that change how FMOD runs. Steam Audio is opt-in per project and read when the project opens
// (before its banks load), so a change applies from the project's next launch.
enum class ProjectSteamAudioRayTracer
{
    Default,
    Embree
};

struct AudioSettings
{
    bool steamAudio = false;
    // Saved as a name, and never rewritten because of what a session could actually run.
    ProjectSteamAudioRayTracer steamAudioRayTracer = ProjectSteamAudioRayTracer::Default;
    // How many times reflection rays bounce: 8, 16, 32 or 64. More gives longer tails and more level, at more simulation cost.
    std::uint32_t steamAudioReflectionBounces = 8;

    bool operator==(const AudioSettings&) const = default;
};

struct ProjectSettings
{
    RenderingSettings rendering;
    AudioSettings audio;

    bool operator==(const ProjectSettings&) const = default;
};

// Checks every field and repairs invalid values in place: out-of-range numbers are clamped, and
// conflicting combinations resolve as the Debug panel would. Each repair adds a warning. Returns
// true when nothing needed repair.
bool ValidateProjectSettings(ProjectSettings& settings, std::vector<std::string>* warnings = nullptr);

// Stable serialized names; enum values are never written.
std::string_view ToSerializedName(ProjectLightingMode value);
std::string_view ToSerializedName(ProjectToneMapper value);
std::string_view ToSerializedName(ProjectReflectionDenoiser value);
std::string_view ToSerializedName(ProjectPsrMode value);
std::string_view ToSerializedName(ProjectPsrDebugView value);
std::string_view ToSerializedName(ProjectDlssQuality value);
std::string_view ToSerializedName(ProjectPathTraceDenoiser value);
std::string_view ToSerializedName(ProjectPathTraceEstimator value);
std::string_view ToSerializedName(ProjectPathTraceDenoiserView value);
std::string_view ToSerializedName(ProjectPathTraceDenoiserSignal value);
std::string_view ToSerializedName(ProjectVolumetricQuality value);
std::string_view ToSerializedName(ProjectVolumetricResolutionBasis value);
std::string_view ToSerializedName(ProjectReflexMode value);
std::string_view ToSerializedName(ProjectHdriSkyQuality value);
std::string_view ToSerializedName(ProjectSkyEnvironmentResolution value);
std::string_view ToSerializedName(ProjectSkyCubeSize value);

// ProjectSettings.json as loaded: the effective settings, why they look the way they do, and the
// retained document whose unknown members a later save writes back.
struct LoadedProjectSettings
{
    static constexpr std::string_view kFormatName = "EngineProjectSettings";
    static constexpr int kCurrentVersion = 1;

    ProjectSettings settings;
    // Missing and Malformed use defaults; UnsupportedVersion and Unreadable are read-only for the
    // session, so their file is never replaced.
    PersonalSettingsFile::Status status = PersonalSettingsFile::Status::Missing;
    // Malformed-file details and per-field recoveries, in a form a user can act on.
    std::vector<std::string> warnings;
    nlohmann::json document = nlohmann::json::object();

    bool IsReadOnly() const noexcept { return !PersonalSettingsFile::MayReplace(status); }
    // A valid file whose fields needed repair (the file itself is unchanged until a save).
    bool WasRecovered() const noexcept { return status == PersonalSettingsFile::Status::Loaded && !warnings.empty(); }
};

namespace ProjectSettingsFile
{
    // Parses text as loaded from disk. A malformed document or newer version yields defaults.
    LoadedProjectSettings Parse(std::string_view text);

    // Reads the file (never writes). A missing file yields defaults with status Missing.
    LoadedProjectSettings Load(const std::filesystem::path& path);

    // The document a save writes: the retained document with every known field replaced.
    std::string Serialize(const ProjectSettings& settings, const nlohmann::json& retainedDocument);

    // Validates `settings`, then replaces the file atomically. Refuses read-only states; a malformed
    // original is first preserved as <name>.invalid. On success `loaded` describes the saved file.
    bool Save(const std::filesystem::path& path, LoadedProjectSettings& loaded, ProjectSettings settings,
        std::string& errorMessage);
}
