#pragma once

// M4.3 ReGIR (experimental, off by default): the CPU side of the ReGIR presampling that feeds RTXDI ReSTIR DI's
// local-light candidates from the shared light-sampling structure (LightSamplingData, M4.1/M4.2C). It defines the
// camera-centred window of M4.1 cells that RTXDI's Grid-mode ReGIR indexes, the per-cell source distributions, the cell
// target, one slot's presampling (the reference the GPU pass must match word for word), and the scene-table to RTXDI
// local-light index translation.
//
// Estimator (Documentation/Rendering/ReGIR_M4_3_Math.md):
// - a slot of window cell c holds one light Y drawn by M-candidate RIS: candidates X_1..X_M are drawn independently
//   from the cell's source distribution p_c, weighted w_i = t_c(X_i) / (M p_c(X_i)) with the cell target t_c, and Y is
//   selected in proportion to w_i;
// - the slot stores Y and its unbiased contribution weight W = sum(w_i) / t_c(Y) (RTXDI's ReGIR convention: RTXDI's
//   shading reads it as `invSourcePdf`). For every light l with t_c(l) > 0 and p_c(l) > 0, E[W 1{Y = l}] = 1;
// - the source is disjoint by construction: inside the M4.1 grid, the cell's padded list (uniform) and the wide pool
//   (in proportion to the cell target when it has at most wideTargetLimit lights, else its alias table) as a one-sample
//   mixture, wide with the realized probability T / 2^32 (T the uint32 threshold of
//   `wideFraction`); outside the grid, the global distribution (every finite light);
// - coverage: every light that reaches a point whose jittered lookup selects c has p_c > 0 (it is in the padded list,
//   the wide pool, or the global distribution) and t_c > 0 (t_c is positive exactly when the light reaches the cell's
//   lookup region). The lookup region is the cell box widened by samplingJitter / 2 cells per side, which the padded
//   lists cover when samplingJitter <= 2 * lookupJitterMargin.

#include "Renderer/ShaderData.h"

#include <cstdint>
#include <vector>

struct LightSamplingData;

struct ReGIRSettings
{
    // The window of M4.1 cells around the camera (RTXDI Grid mode cellsX/Y/Z).
    std::uint32_t windowCells[3] = {16, 16, 16};
    // Slots per cell (RTXDI lightsPerCell) and RIS candidates per slot (numRegirBuildSamples).
    std::uint32_t lightsPerCell = 64;
    std::uint32_t buildSamples = 4;
    // RTXDI samplingJitter, in cells; at most 2 * LightSamplingSettings::lookupJitterMargin.
    float samplingJitter = 1.0f;
    // Probability of drawing a build candidate from the wide pool rather than the cell's padded list, where both are
    // non-empty (the M4.2C moving lights live in the wide pool).
    float wideFraction = 0.125f;
    // A wide pool of at most this many lights is sampled in proportion to the cell target (exactly, over its members)
    // instead of its power alias table: the M4.2C moving lights are wide while they move, and a power-proportional draw
    // finds a weak light next to the cell almost never. 0 always uses the alias table.
    std::uint32_t wideTargetLimit = 64;
    // M4.3.5: ReGIR runs only above this many RTXDI local lights (CountReGIRLocalLights); at or below it the uniform
    // candidates already cover the local lights well.
    std::uint32_t localLightThreshold = 64;
};

// The window in M4.1 cell coordinates (it may extend beyond the M4.1 grid) and RTXDI's view of it.
struct ReGIRWindow
{
    bool valid = false;
    std::int64_t originCell[3]{};
    std::uint32_t cells[3]{};
    // The M4.1 grid (as the shader receives it) and the builder's float-precision guard.
    double gridOrigin[3]{};
    std::int64_t gridDimensions[3]{};
    double cellSize = 1.0;
    double precisionGuard = 0.0;
    float samplingJitter = 1.0f;
    // ReGIR_CommonParameters centerX/Y/Z: RTXDI derives its grid origin as center - cells * cellSize / 2.
    float center[3]{};

    std::uint32_t CellCount() const noexcept
    {
        return cells[0] * cells[1] * cells[2];
    }
};

// The window centred on `camera` (invalid when `data` has no published structure or the settings are invalid).
ReGIRWindow MakeReGIRWindow(const LightSamplingData& data, const float camera[3], const ReGIRSettings& settings);

// The window cell RTXDI's Grid mode selects for `position` (RTXDI_ReGIR_WorldPosToCellIndex in float), or -1 outside.
std::int32_t ReGIRWindowCellAt(const ReGIRWindow& window, const float position[3]);

// A window cell's source distribution.
struct ReGIRCellSource
{
    // The M4.1 cell (-1 outside the M4.1 grid: the global distribution is the source).
    std::int64_t gridCell = -1;
    std::uint32_t listOffset = 0;
    std::uint32_t paddedCount = 0;
    std::uint32_t wideCount = 0;
    std::uint32_t globalCount = 0;
    // A uint32 random below wideThreshold draws from the wide pool (0: never; 2^32 - 1 and paddedCount 0: always).
    std::uint32_t wideThreshold = 0;
    // The lookup region (cell box widened by samplingJitter / 2 cells and the precision guard per side).
    // In float, exactly as the presampling shader computes it.
    float regionLow[3]{};
    float regionHigh[3]{};
};

ReGIRCellSource ReGIRCellSourceAt(const LightSamplingData& data, const ReGIRWindow& window, std::uint32_t windowCell,
    const ReGIRSettings& settings);

// The cell target: an upper bound of the light's unshadowed irradiance luminance over the region, luminance(radiance) x
// window(d) / max(d^2, (cellSize / 2)^2) with d the distance from the light to the region (Engine's range window,
// SceneLights.hlsli). Positive exactly when the light reaches some point of the region; 0 for directional lights.
// Computed in float in the shader's order of operations.
float ReGIRCellTarget(const GPULight& light, const float regionLow[3], const float regionHigh[3], float cellSize);

// PCG32 step shared with the presampling shader.
std::uint32_t ReGIRNextRandom(std::uint32_t& state) noexcept;
// The random state of slot `slot` in frame `frameSeed`.
std::uint32_t ReGIRSlotSeed(std::uint32_t slot, std::uint32_t frameSeed) noexcept;

// One slot: the selected scene-table light (0xFFFFFFFF and weight 0 when no candidate has a positive weight) and its
// unbiased contribution weight W.
struct ReGIRSlot
{
    std::uint32_t light = 0xFFFFFFFFu;
    float weight = 0.0f;
};

// One candidate of a slot, for verification: the scene-table light (0xFFFFFFFF when the draw found none), its source
// (0 padded list, 1 wide alias, 2 wide by target, 3 global), its realized probability and its cell target.
struct ReGIRCandidateTrace
{
    std::uint32_t light = 0xFFFFFFFFu;
    std::uint32_t source = 0;
    float probability = 0.0f;
    float target = 0.0f;
};

// The reference presampling of one slot (the GPU pass must produce the same words). `trace`, when given, receives
// settings.buildSamples candidates.
ReGIRSlot ReGIRPresampleSlot(const LightSamplingData& data, const SceneLightTable& table, const ReGIRCellSource& source,
    const ReGIRSettings& settings, std::uint32_t& random, ReGIRCandidateTrace* trace = nullptr);

// Scene-table index -> RTXDI local-light index (the light's rank among the table's non-directional lights, the order
// BuildRtxdiLightSet adds them in); 0xFFFFFFFF for directional lights.
void BuildReGIRLocalIndexTranslation(const SceneLightTable& table, std::vector<std::uint32_t>& translation);

// The table's RTXDI local lights (every light except directional ones), the count ReGIR's threshold applies to.
std::uint32_t CountReGIRLocalLights(const SceneLightTable& table);

// ReGIRPresample.hlsl's constant buffer (same layout).
struct ReGIRPresampleConstants
{
    std::uint32_t lightSamplingIndex = 0xFFFFFFFFu;
    std::uint32_t sceneLightBufferIndex = 0xFFFFFFFFu;
    std::uint32_t sceneLightCount = 0;
    std::uint32_t reservedIndex = 0xFFFFFFFFu;

    std::uint32_t outputIndex = 0xFFFFFFFFu;
    std::uint32_t outputOffset = 0;
    std::uint32_t lightsPerCell = 0;
    std::uint32_t buildSamples = 0;

    std::int32_t originCell[3]{};
    std::uint32_t frameSeed = 0;
    std::uint32_t windowCells[3]{};
    std::uint32_t wideThreshold = 0;

    std::uint32_t wideTargetLimit = 0;
    float samplingJitter = 0.0f;
    float precisionGuard = 0.0f;
    std::uint32_t debugIndex = 0xFFFFFFFFu;

    float windowCenter[3]{};
    std::uint32_t headerPadding = 0;
};

static_assert(sizeof(ReGIRPresampleConstants) == 96, "ReGIRPresampleConstants must match ReGIRPresample.hlsl.");

// The presampling output: a 64-byte header (ReGIRPresample.hlsl), then 8 bytes per slot.
inline constexpr std::uint32_t ReGIRSlotHeaderBytes = 64;

// The constants of one presampling dispatch for `window` (descriptor indices left for the caller). The wide threshold is
// the one ReGIRCellSourceAt uses.
ReGIRPresampleConstants MakeReGIRPresampleConstants(const ReGIRWindow& window, const ReGIRSettings& settings,
    std::uint32_t sceneLightCount, std::uint32_t frameSeed);
