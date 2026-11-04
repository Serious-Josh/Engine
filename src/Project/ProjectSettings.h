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
    NrdHybrid
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

    // Dedicated and pixel-centred caustics are mutually exclusive. Caustic-only view applies only with
    // dedicated caustics, but its value is kept while they are off (the Debug panel behaves the same).
    bool dedicatedCaustics = false;
    bool causticOnlyView = false;
    bool pixelCentredCaustics = false;

    bool operator==(const PathTracingRenderingSettings&) const = default;
};

struct RenderingSettings
{
    ProjectLightingMode lightingMode = ProjectLightingMode::Legacy;
    ProjectToneMapper toneMapper = ProjectToneMapper::AgX;
    // Both mode sections are always kept, so switching modes never loses the other mode's values.
    LegacyRenderingSettings legacy;
    PathTracingRenderingSettings pathTracing;

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
std::string_view ToSerializedName(ProjectPathTraceDenoiserView value);
std::string_view ToSerializedName(ProjectPathTraceDenoiserSignal value);

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
