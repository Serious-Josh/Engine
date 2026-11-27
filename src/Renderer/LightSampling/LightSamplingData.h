#pragma once

#include "Renderer/ShaderData.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// M4.1 shared light-sampling infrastructure: the CPU build of the sampling structures every scalable light-sampling
// consumer reads (M4.2 indirect NEE and SHARC training, M4.3 ReGIR candidate construction). Built from one frame light
// table and published with it (LightSamplingBuffer), so records, cell lists and distributions always share one lighting
// revision. Design and proofs: Documentation/Rendering/Scalable_Light_Sampling_M4_0_Validation.md; layout and API:
// Scalable_Light_Sampling_M4_1_Implementation.md. Shader side: assets/Shaders/LightSampling.hlsli.
//
// Every table light belongs to exactly one category:
// - Excluded: zero, negative or non-finite power. It contributes nothing anywhere, so no pool needs it.
// - Directional: unbounded; listed for enumeration.
// - Binned: a finite (Point/Spot) light listed in every grid cell its range sphere reaches.
// - Wide: a finite light whose range would make binning too expensive. Sampled from the wide distribution instead.
// A shading point inside the grid therefore sees the disjoint pools {its cell's binned lights, wide, directional}. Grid
// bounds contain every binned light's padded range sphere, so outside the grid no binned light contributes and the
// pools are {wide, directional}. A global distribution over every finite light is also published for consumers that
// need a position-independent pool.
//
// Cell lists are complete: a list holds every binned light that can reach any point of its cell, without a capacity.
// Memory is bounded by demoting lights to the wide pool, never by truncating a list. Each list stores its nominal
// members (range sphere reaches the cell box, widened by a float-precision guard) first, then the members that reach
// only the padded box (the box widened by the lookup-jitter margin, for RTXDI ReGIR's jittered cell lookup). Indirect
// NEE samples the nominal part; ReGIR construction samples the whole list.
//
// Probabilities stored or computed for the GPU are those the GPU selection procedure actually realizes: a uint32 alias
// table (bin = mulhi(r0, n), accept if r1 < threshold) and, for lists, index = mulhi(r, n), whose exact probabilities the
// shader computes with 64-bit integer arithmetic.

// GPU light types (SceneLights.hlsli SCENE_LIGHT_*).
inline constexpr std::uint32_t LightSamplingPointType = 0;
inline constexpr std::uint32_t LightSamplingSpotType = 1;
inline constexpr std::uint32_t LightSamplingDirectionalType = 2;

struct LightSamplingSettings
{
    // World-space cell edge in meters; 0 selects it automatically (the smallest power-of-two size from 1/8 m to 1024 m
    // whose estimated references fit the budget and whose cells fit the cell limit, with few lights over the per-light
    // limit).
    float cellSize = 0.0f;
    // Padded coverage beyond each cell, in cells: RTXDI ReGIR's lookup jitter reaches half its samplingJitter times the
    // cell size per axis, so 0.5 supports samplingJitter 1.
    float lookupJitterMargin = 0.5f;
    // Cell-list references (nominal plus padded, 4 bytes each) the lists may hold; lights are demoted beyond it.
    std::uint64_t referenceBudget = 8ull * 1024 * 1024;
    // A light whose padded range reaches more cells than this is wide.
    std::uint32_t perLightCellLimit = 4096;
    // Grid cells the automatic size may create.
    std::uint32_t maximumCells = 1u << 21;
    // A light whose range exceeds this multiple of the median finite range is wide (it would otherwise inflate the grid
    // bounds for every other light).
    float wideRangeFactor = 16.0f;
    // The automatic size prefers sizes leaving at most this fraction of finite lights over the per-light limit.
    double maximumWideFraction = 0.01;
    // Defensive uniform share of the wide and global distributions: no light's probability can underflow.
    double defensiveShare = 1.0e-3;
    // M4.1.1: spare alias entries reserved in the wide and global distributions for lights an incremental update adds to
    // them (moved lights, lights that start contributing): this fraction of the binned lights, at least the minimum.
    double dynamicReserveFraction = 1.0 / 16.0;
    std::uint32_t minimumDynamicReserve = 64;
    // World-level noise fix (2026-11-27): the global distribution (ReSTIR PT's light tiles, and the out-of-grid source of
    // the ReGIR slots and sampled NEE) weights each light by its power times 1 if it can light what the camera sees, else
    // cameraFarFactor. A light can when its range sphere, widened by cameraReachRadius, contains the camera or intersects
    // the view frustum (to the far plane), or when it has no finite range (range attenuation off). A few bright lights
    // elsewhere in a World (behind the camera or beside the view) otherwise take nearly every light-tile candidate from the
    // dim lights near the camera. Proposal only: every consumer divides by the realized probability, and the defensive
    // share keeps every light's probability above zero. The classification is re-evaluated when the camera enters another
    // cell of cameraCellSize metres or turns by more than cameraTurnDegrees (or its field of view changes); the
    // distribution is rewritten (in place) only when some light's classification changed.
    // ENGINE_LIGHT_SAMPLING_CAMERA_WEIGHT=0 restores pure power weighting.
    bool cameraWeighting = true;
    // Measured 2026-11-27: a 4 m sphere around the camera alone (16 m and 1/64 left New World 8.6x noisier than Demo Scene)
    // doubled ReSTIR_Benchmark_64's noise (mid-depth 4.4x), whose visible hall is lit by lights that never come near the
    // camera; the frustum test keeps those. Off-view lights still light visible points through bounces only, at higher
    // variance, never bias. ENGINE_LIGHT_SAMPLING_CAMERA_REACH / _FAR / _FRUSTUM=0 override these.
    float cameraReachRadius = 4.0f;
    double cameraFarFactor = 1.0 / 1024.0;
    float cameraCellSize = 8.0f;
    bool cameraFrustum = true;
    float cameraTurnDegrees = 5.0f;
};

// World-level noise fix: the view the global distribution is weighted for (RenderView's basis, +Z forward).
struct LightSamplingCameraView
{
    float position[3]{};
    float forward[3]{0.0f, 0.0f, 1.0f};
    float right[3]{1.0f, 0.0f, 0.0f};
    float up[3]{0.0f, 1.0f, 0.0f};
    float verticalFov = 0.0f;
    float aspectRatio = 0.0f;
    float farPlane = 0.0f;
};

// M4.1.1: a light's category in a built structure (as the builder classified it, or as an incremental update moved it).
enum class LightSamplingCategory : std::uint8_t
{
    Excluded,
    Directional,
    Binned,
    Wide,
    // A binned light that moved (or a light that started contributing) after the build: removed from every cell list and
    // sampled from the wide pool until the next full build bins it again.
    Dynamic
};

// M4.1.1: what an incremental update needs from the build it updates (CPU only; never uploaded).
struct LightSamplingIncrementalState
{
    bool valid = false;
    // The grid exactly as built (double precision, as the builder traversed it).
    double gridOrigin[3]{};
    std::int64_t gridDimensions[3]{};
    double cellSize = 1.0;
    double paddedMargin = 0.0;
    std::uint64_t cellCount = 0;
    std::uint64_t listsStartWord = 0;
    std::uint64_t wideStartWord = 0;
    std::uint64_t globalStartWord = 0;
    std::uint32_t wideCapacity = 0;
    std::uint32_t globalCapacity = 0;
    // Per table light: category, and for binned lights the guard-widened range sphere they were binned with.
    std::vector<LightSamplingCategory> categories;
    std::vector<std::array<double, 4>> spheres;
    // The wide and global distributions' members, in alias-entry order.
    std::vector<std::uint32_t> wideMembers;
    std::vector<std::uint32_t> globalMembers;
    // M4.2C: incremental updates applied since the build, and per table light the update in which its position or range
    // last changed (0: not since the build). A Dynamic light that did not change in the latest update has settled, so a
    // full build can bin it again.
    std::uint64_t updateSerial = 0;
    std::vector<std::uint64_t> lastMoved;
};

// M4.1.1: a range of words an incremental update changed (relative to the start of the data, after the header).
struct LightSamplingDirtyRange
{
    std::uint64_t firstWord = 0;
    std::uint64_t wordCount = 0;
};

// Observability of one build: counts, occupancy, memory and the reasons lights became wide.
struct LightSamplingStatistics
{
    std::uint32_t tableLights = 0;
    std::uint32_t excludedLights = 0;
    std::uint32_t directionalLights = 0;
    std::uint32_t binnedLights = 0;
    std::uint32_t wideLights = 0;
    std::uint32_t wideByRange = 0;
    std::uint32_t wideByCellLimit = 0;
    std::uint32_t wideByBudget = 0;
    std::uint32_t cells = 0;
    std::uint32_t nonEmptyCells = 0;
    std::uint64_t nominalReferences = 0;
    std::uint64_t paddedReferences = 0;
    std::uint32_t maximumNominalOccupancy = 0;
    std::uint32_t maximumPaddedOccupancy = 0;
    double meanNominalOccupancy = 0.0;
    std::uint64_t cellBytes = 0;
    std::uint64_t listBytes = 0;
    std::uint64_t distributionBytes = 0;
    std::uint64_t totalBytes = 0;
    // The float-precision guard (meters) added to every binned range.
    double precisionGuard = 0.0;
    // M4.2B: CPU milliseconds per build phase (classification and grid sizing; counting and demotion; offsets and list
    // fill; distributions and layout), and whether the weight-only path reused the cell lists.
    double classifyMilliseconds = 0.0;
    double countMilliseconds = 0.0;
    double fillMilliseconds = 0.0;
    double distributionMilliseconds = 0.0;
    bool reusedLists = false;
    // M4.1.1: lights an incremental update moved to the wide pool since the last full build, the reserved alias entries,
    // and the last incremental update's CPU milliseconds, changed words and lights it demoted.
    std::uint32_t dynamicLights = 0;
    std::uint32_t wideCapacity = 0;
    std::uint32_t globalCapacity = 0;
    double incrementalMilliseconds = 0.0;
    std::uint64_t incrementalDirtyWords = 0;
    std::uint32_t incrementalDemotions = 0;
    // M4.2C: Dynamic lights whose position and range did not change in the latest incremental update.
    std::uint32_t settledDynamicLights = 0;
    // True when the reference budget forced demotions (wideByBudget > 0): the configuration is not represented at the
    // chosen cell size without moving lights to the wide pool.
    bool budgetForcedDemotion = false;

    double WideFraction() const noexcept
    {
        const std::uint32_t finite = binnedLights + wideLights;
        return finite ? static_cast<double>(wideLights) / finite : 0.0;
    }
};

// One build's result: the GPU constants (offsets relative to the start of the data) and the raw buffer contents.
struct LightSamplingData
{
    LightSamplingConstants constants{};
    std::vector<std::uint32_t> words;
    LightSamplingStatistics statistics;
    LightSamplingIncrementalState incremental;

    // World-level noise fix: the camera weighting the global distribution holds (LightSamplingSettings::cameraWeighting).
    // A full build writes pure power weights (applied false); incremental updates keep the applied classification.
    struct CameraWeighting
    {
        bool applied = false;
        // The camera cell (cameraCellSize), forward direction and field of view the classification was last evaluated for.
        bool cellValid = false;
        std::int64_t cell[3]{};
        float forward[3]{};
        float verticalFov = 0.0f;
        float aspectRatio = 0.0f;
        // Per table light: 1 when it can light what the camera sees (weight 1), 0 when far (cameraFarFactor).
        std::vector<std::uint8_t> reach;
    } cameraWeighting;
};

// World-level noise fix: what UpdateLightSamplingCameraWeighting did.
struct LightSamplingCameraUpdate
{
    // The global distribution was rewritten (its words are in `dirty`).
    bool rewritten = false;
    // Lights whose reach classification changed against the previously applied one (all far lights on a first application).
    std::uint32_t changedLights = 0;
    std::uint32_t farLights = 0;
    double milliseconds = 0.0;
};

// World-level noise fix: applies the camera weighting (LightSamplingSettings::cameraWeighting) to `data`'s global
// distribution for `view`, in place. Nothing happens while the camera stays in the cell, direction (within
// cameraTurnDegrees) and field of view it was last evaluated for; otherwise the lights are reclassified and the
// distribution is rewritten only when a classification differs from the applied one (or none is applied yet and some
// light is far). With weighting off, a weighted distribution is restored to pure power. `dirty` receives the changed
// words (sorted, merged).
LightSamplingCameraUpdate UpdateLightSamplingCameraWeighting(const SceneLightTable& table,
    const LightSamplingSettings& settings, const LightSamplingCameraView& view, LightSamplingData& data,
    std::vector<LightSamplingDirtyRange>& dirty);

// Builds the sampling structures for `table`. Deterministic: the same table and settings give the same words. Fails
// (data untouched) only when the table is inconsistent or the structures would exceed 32-bit addressing.
bool BuildLightSamplingData(const SceneLightTable& table, const LightSamplingSettings& settings, LightSamplingData& data,
    std::string* error = nullptr);

// M4.1.1 incremental update, in place: `data` was built from (or last updated to) `previousTable`; afterwards it samples
// `table` exactly (the same disjoint pools property and realized probabilities a full build guarantees), and `dirty`
// lists the words that changed (sorted, merged). Per light that changed:
// - radiance only (intensity, colour): the wide and global distributions are rebuilt in place over the same members;
// - position or range of a binned light: it leaves every cell list it was in (each list keeps its other members) and
//   joins the wide pool (Dynamic) until the next full build;
// - a light that starts contributing: it joins the wide (if finite) and global pools;
// - a light that stops contributing (power 0) keeps its membership: its candidates contribute nothing, so the
//   estimate is unchanged, and the next full build drops it;
// - anything else that does not affect sampling (spot cones, direction, a directional light's parameters): nothing.
// Returns false, with `data` untouched, when the change needs a full build: a different light count or type, a new
// directional contributor, more wide or global members than the reserved capacity, or data without incremental state.
// (Data that no longer matches its incremental state, which only a defect could cause, is detected while lists are
// edited; the state is then marked invalid and false returned, and the caller's full build replaces the data.)
bool UpdateLightSamplingIncremental(const SceneLightTable& table, const SceneLightTable& previousTable,
    const LightSamplingSettings& settings, LightSamplingData& data, std::vector<LightSamplingDirtyRange>& dirty);

// The light's sampling weight for the wide and global distributions. Zero, negative or non-finite means the light is
// excluded. Point-light noise fix #3 (user-approved 2026-11-23): its emitted power, the luminance of its radiance record
// (luminous intensity for Point/Spot) times its emission solid angle: 4 pi for a Point, the Spot profile's integral
// (Photometry::SpotEmissionSolidAngle over the uploaded cone cosines) for a Spot, 1 for Directional (illuminance, never
// in these distributions). Before the fix it was the intensity alone, which weighted a Spot of equal flux 4 pi / Omega
// (about 18x for a 36 degree cone) over a Point. ENGINE_LIGHT_SAMPLING_POWER=intensity restores that weighting. Every
// consumer divides by the realized alias probability, so the weighting changes only variance.
double LightSamplingPower(const GPULight& light) noexcept;

// The emission solid angle LightSamplingPower applies: 4 pi (Point), the Spot profile's integral, 1 (Directional or
// ENGINE_LIGHT_SAMPLING_POWER=intensity).
double LightSamplingEmissionSolidAngle(const GPULight& light) noexcept;

// One-line summary for logs.
std::string DescribeLightSamplingStatistics(const LightSamplingData& data);
