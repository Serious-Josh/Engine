#pragma once

#include "Renderer/Buffer.h"
#include "Renderer/ComputePipeline.h"
#include "Renderer/RayTracing/TopLevelAccelerationStructure.h"

#include <d3d12.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class DescriptorHeap;
class ShaderLibrary;
class UploadBuffer;

// M5.1 ReSTIR PT Enhanced (configuration B, Editor-only, experimental): what a frame of the unified initial path sampling
// does. The defaults are the published configuration (Documentation/Rendering/ReSTIR_PT_Enhanced_M5_Implementation_
// Specification.md); the other values exist for the M5.1 correctness gates and are never selected by a project.
struct ReSTIRPTSettings
{
    // The SHARC radiance cache at x3 (Documentation/Rendering/SHARC_ReSTIR_PT_Investigation.md): every path ends at x3 with
    // NVIDIA SHARC's multi-bounce radiance where SHARC admits x3 (no further light where it refuses), except mirror chains
    // seen from the camera; cache endings are not reused. Off: conventional depth-limited paths. Needs NVIDIA SHARC
    // (SharcCache::CapabilityFailure) and a path depth of at least 3; the Editor's switch resets ReSTIR PT's history.
    bool sharc = true;
    // M5.1-2: RIS over light-tile candidates (32 at the primary hit, 32 / j^2 at bounce j); off: one candidate.
    bool ris = true;
    // M5.1-3: one candidate per tree in the reservoir; off: every candidate accumulated (plain lobe-indexed path tracing).
    bool reservoir = true;
    // Q-2 gate only: the original code's 1/M normalization (biased by construction for disjoint candidate domains).
    bool normalizationOneOverM = false;
    // Gate isolation: direct (k <= 2) or indirect (k >= 3) candidates only.
    bool directOnly = false;
    bool indirectOnly = false;
    std::uint32_t primaryCandidates = 32;
    std::uint32_t tileCount = 128;
    std::uint32_t tileSize = 1024;
    // The paper vertex index from which Russian roulette follows a continuation. 0 follows the frame's production setting
    // (russianRouletteDepth, the same path space as configuration A); NeverRoulette disables it.
    static constexpr std::uint32_t NeverRoulette = 0xFFFFFFFFu;
    std::uint32_t rouletteFromVertex = 0;
    // 1: the selected candidate's length k as a colour in the output texture (the accumulation is unchanged).
    std::uint32_t debugView = 0;
    // Records the GPU counters (rays, candidates, selected lengths, would-clamp contributions).
    bool counters = false;
    // M5.1 T-1.8 test only (unpackaged diagnostics): NaN injected at the pixels with x % 61 == 7 and y % 53 == 5, into their
    // x1 table-light NEE candidate (the stream must drop it) or into each sample's result (the sample must be rejected).
    enum class Inject : std::uint32_t
    {
        None,
        CandidateNaN,
        ResultNaN
    };
    Inject inject = Inject::None;
    // M5.1-4 T-2.1 test only (unpackaged diagnostics, ENGINE_RESTIR_PT_SELF_SHIFT=1): shift each selected sample into its
    // own pixel (prefix replay, reconnection from the stored vertex) and count identities (counters 10-15).
    bool selfShift = false;
    // T-2.1 diagnostic: shift from the in-register (unquantized) reconnection data, not the stored words.
    bool selfShiftUnquantized = false;
    // T-2.1 round 3 quantization decomposition (stored words): the in-register rcWi, or barycentrics, in place of the
    // stored field.
    bool selfShiftExactDirection = false;
    bool selfShiftExactBarycentrics = false;
    // M5.2 C0 performance baseline only (unpackaged diagnostics, ENGINE_RESTIR_PT_VISIBILITY_BENCH=excluding|plain): after
    // each sample's path tree, one reconnection-type visibility ray from x1 to the selected sample's hit reconnection
    // vertex, either excluding that vertex's own triangle (candidate inspection, as shifts trace it) or as a plain opaque
    // ray. The image is unchanged; the ray count is the only effect.
    enum class VisibilityBench
    {
        None,
        Excluding,
        Plain
    };
    VisibilityBench visibilityBench = VisibilityBench::None;
    // M5.1-6 F3 reconstruction experiment (unpackaged diagnostics, ENGINE_RESTIR_PT_OUTPUT=sum): while there is no reuse,
    // the image and the NRD/RR signals take the path tree's full sum instead of the published f(Y) W. The reservoir is
    // selected and stored unchanged. Not the published estimator's output; never a default.
    bool outputSum = false;
    // M5.2 temporal reuse (published algorithm, specification section 6.1). Off until the M5.2 C3 gates pass; history is
    // maintained either way.
    bool temporal = false;
    // The temporal reservoir's confidence cap (Cap = 20 [P 3]).
    std::uint32_t temporalConfidenceCap = 20;
    // M5.2 C4 test only (diagnostics variant): shift each forward-shifted temporal sample back into the temporal domain and
    // count consistency by sample kind (counters 14144-14159).
    bool temporalRoundTrip = false;
    // M5.3 paired spatial reuse (specification section 6.2): off until reviewed. It needs one sample per frame (the
    // paper's one path tree per pixel); otherwise the renderer skips it.
    bool spatial = false;
    // Colour-noise reduction in the spatial resampling's shading [P 6.3] (the published configuration); off: F(Y) W.
    bool spatialColourNoiseReduction = true;
    // M5.3 test only (diagnostics variant): shift each pre-pass result back into its pixel (counters 14164-14167).
    bool spatialRoundTrip = false;
    // M5.4 C1: temporal reuse in its own pass (RestirPtTemporalCS) at one sample per frame; off, or at more samples per
    // frame, the fused M5.2 kernel (initial sampling and temporal reuse in one dispatch).
    bool temporalPass = true;
    // M5.4 C4: the published reconnection criteria (Eq. 5 dual ray footprint, c = 0.02, + single-vertex roughness
    // alpha_min = 0.2) in place of the original ones (d_min, both vertices rough). neeInverseProxy (test only, A'): NEE
    // leaving x_k takes the inverse footprint with the BSDF density toward the light (the default, Option B, skips it).
    bool footprintCriteria = true;
    bool neeInverseProxy = false;
    // M5.4 C5: replay compaction in the temporal pass (pixels whose shifts trace a replay run in a second dispatch over a
    // compacted list). Applies with the separate temporal pass only.
    bool replayCompaction = true;
    // M5.4 C6: dual motion vectors in the temporal lookup (Zeng et al. 2021), with ReSTIR PT's own instance history.
    bool dualMotion = true;
    // M5.4 C7 duplication map [P 9-10]: Off; Measure (computed, not applied: the estimator is unchanged); Apply (temporal
    // reuse's cap reduced by it: biased). Off by default pending a separate decision.
    enum class Duplication
    {
        Off,
        Measure,
        Apply
    };
    Duplication duplication = Duplication::Off;
    // M5.5-B experiment (NVIDIA RTXDI final-shading decorrelation, "Uniform" mode only): with this constant probability a
    // pixel's final pass shows the initial kernel's independent estimate f(Y_init) W_init (from the extraction textures)
    // instead of the reused one. Both are unbiased for the same integral and the choice is independent of both, so the
    // mixture is unbiased. Shading only: reservoirs, history, MIS and shifts are untouched. Extraction (denoiser) frames
    // only; 0 is off and leaves every output bit-identical. Default 0.35 (user request 2026-11-22, for visual evaluation
    // after Checkpoint B: RR flicker -37%, Tri-Test gates pass); ENGINE_RESTIR_PT_DECORRELATE=0 turns it off.
    float decorrelation = 0.35f;
    // M5.5 temporal correctness, Stage 1: shifts into the temporal domain are evaluated in the history scene (the committed
    // frame's instance transforms over the same BLASes), as GRIS requires. On by default (user-approved A2, 2026-11-23);
    // ENGINE_RESTIR_PT_HISTORY_SCENE=0 turns it off for comparisons.
    bool historyScene = true;
    // Optimization A5 (ENGINE_RESTIR_PT_SPATIAL_PAIRS=1..3, or the Editor's "Spatial pairs"): spatial partners per pixel.
    // Default 3, the published option (user decision 2026-11-24, with the direct-light reservoir, whose cheaper reuse
    // passes pay for it; before: 1 from 2026-11-23, 2 earlier that day).
    std::uint32_t spatialPairs = 3;
    // Optimization experiment B1a (ENGINE_RESTIR_PT_DECORRELATION_MODE=stagnancy, or the Editor's "Decorrelation"):
    // NVIDIA RTXDI's Stagnancy mode in place of Uniform. p = saturate(4 * stagnancyFactor * sqrt(smoothed age / 40)),
    // with the age carried in the reservoir and smoothed by an EMA over the reprojected map (RTXDI defaults). Biased (the
    // probability depends on sample history). Default Stagnancy (user decision 2026-11-23);
    // ENGINE_RESTIR_PT_DECORRELATION_MODE=uniform restores Uniform.
    enum class DecorrelationMode
    {
        Uniform,
        Stagnancy
    };
    DecorrelationMode decorrelationMode = DecorrelationMode::Stagnancy;
    float stagnancyFactor = 0.4f;
    // Optimization B1b (user-approved default 2026-11-23; ENGINE_RESTIR_PT_FIREFLY=0 or the Editor's "Firefly replacement"
    // turns it off): NVIDIA RTXDI's firefly replacement. The resample pass flags pixels whose shaded luminance exceeds
    // 5.29x their 8 x 8 group's average nonzero luminance (strength 0.7) and shows their initial estimate instead (bound 15
    // for new fireflies). Needs temporal + spatial reuse and extraction (denoiser frames). Accepted bias: final-shading
    // only, about -0.44% energy (Tri-Test); reservoirs and history unchanged.
    bool fireflyReplacement = true;
    // M5.6 O0 (ENGINE_RESTIR_PT_REGIR=0 or the Editor's "ReGIR light candidates" turns it off; ENGINE_REGIR=0 disables
    // ReGIR for both estimators): above ReGIRSettings::localLightThreshold local lights, the table-light NEE candidates
    // come from the shared ReGIR cells (Renderer::PrepareReGIRFrame) instead of the global light tiles. Proposal only:
    // the candidate's integrand keeps p_1 = the light's global realized probability (RestirPtTableLightNee), so history,
    // shifts and MIS are unchanged and the switch is not part of the history signature.
    bool regir = true;
    // M5.6 O0.1: ReSTIR PT's own ReGIR window, in cells per axis (ReSTIR DI keeps ReGIRSettings::windowCells; one shared
    // slot buffer is sized for either). 16 (64 m across at 4 m cells) was the O0 default; 32 (128 m) is the default since
    // the user decision of 2026-11-23. ENGINE_RESTIR_PT_REGIR_WINDOW or the Editor's "ReGIR grid" (16 or 32). Proposal
    // only: not in the history signature.
    std::uint32_t regirWindow = 32;
    // M5.6 O0.1: the spatial partners used on frames whose candidates come from ReGIR (0: spatialPairs, as on other frames).
    // ENGINE_RESTIR_PT_REGIR_SPATIAL_PAIRS or the Editor's "Spatial pairs with ReGIR".
    std::uint32_t regirSpatialPairs = 0;
    // M5.6 O0.2 (user-approved defaults 2026-11-23: both on wherever ReGIR is active; ReSTIR PT builds only, ReSTIR DI's
    // slots are unchanged; ENGINE_RESTIR_PT_REGIR_CONE=0 / _DIRECT=0 or the Editor turn either off):
    //   regirCone: the ReGIR cell target includes an upper bound of a spot light's cone factor over the cell's lookup region
    //     (ENGINE_RESTIR_PT_REGIR_CONE=1, Editor "ReGIR spot-cone bound"). Changes the proposal only;
    //   regirDirectFetch: slots hold scene-table indices, so a candidate needs no index map, and p_1 is read once for the
    //     selected light (ENGINE_RESTIR_PT_REGIR_DIRECT=1, Editor "ReGIR direct fetch"). Same distribution as the map path.
    bool regirCone = true;
    bool regirDirectFetch = true;
    // M5.6 flashing fix (user-approved 2026-11-23): ReSTIR PT's ReGIR slots per cell (ReSTIR DI keeps
    // ReGIRSettings::lightsPerCell, 64). Every pixel of a cell draws from the same per-frame slots, so with 64 slots a
    // cell's estimate brightens or darkens coherently frame to frame and RR shows it as flashing; 256 removed the still-
    // camera flashing (ManyLight_1024: 3% -> 0.07% of tiles) for about +1 ms at a 32-cell window. Proposal only (each slot
    // keeps its unbiased W). Default 512 (user choice after visual testing: no flashing still or moving);
    // ENGINE_RESTIR_PT_REGIR_SLOTS or the Editor's "ReGIR slots per
    // cell" slider (up to Renderer::ReGIRMaximumSlotsPerCell, the buffer's capacity). Measured (32-cell window): 64 -> 256
    // -> 1024 slots: still flashing 3% -> 0.07% -> 0%, grid build 0.26 -> 1.0 -> 4.3 ms.
    std::uint32_t regirSlots = 512;
    // Point-light noise fix #1 (user-approved 2026-11-23): ReSTIR PT's ReGIR slots are drawn exactly in proportion to the
    // cell target over the cell's padded list and small wide pool, with W = 1 / q, instead of by 4-candidate RIS, whose
    // W only estimates 1 / q (ReGIRPresample.hlsl mainExact; cells outside the M4.1 grid or with a large wide pool keep the
    // RIS draw). Changes the proposal: q is the cell target itself. ENGINE_RESTIR_PT_REGIR_PROPOSAL=ris or the Editor's
    // "ReGIR exact slot proposal" restores the RIS draw.
    bool regirExactProposal = true;
    // Point-light noise fix #2 (user-approved 2026-11-23): a spot light's cone factor in the ReGIR cell target is its mean
    // over a 3 x 3 x 3 grid of the cell's lookup region (at least 0.05 of the conservative bound, so coverage is unchanged)
    // instead of the bound alone, which let spots crowd point lights out of the slots where their cones barely reach.
    // Proposal only. ENGINE_RESTIR_PT_REGIR_SPOT_TARGET=bound or the Editor's "ReGIR representative spot target" restores
    // the bound (with regirCone).
    bool regirSpotTarget = true;
    // The adaptive light sampler (on by default, user decision 2026-11-23): ReSTIR PT's ReGIR kernel draws its candidates
    // from per-receiver proposals over the light grid's lists (the adaptive walk at x1, uniform members deeper) instead of
    // the ReGIR slots. The Editor's "Adaptive x1 light sampling" or ENGINE_RESTIR_PT_LIST_START=off switches it off at run
    // time; ENGINE_RESTIR_PT_LIST_PROTOTYPE=0 compiles it out (then this is ignored).
    bool regirListPrototype = true;
    // Residual boiling P1 (on by default, user decision 2026-11-24; ENGINE_RESTIR_PT_MIRROR_X1=0 or the Editor turns it off):
    // the first non-delta vertex of a camera mirror chain (x2 behind a delta x1, or x3 behind delta x1 and x2) shades what the
    // camera sees in the mirror, so its table-light NEE uses x1's light sampler and candidate count instead of the
    // deep-vertex ones. Proposal only (each candidate keeps its unbiased weight and p_1), so it is not part of the history
    // signature. Documentation/Rendering/ReSTIR_PT_Residual_Boiling_Investigation.md.
    bool mirrorChainX1Sampling = true;
    // Residual boiling, research report §5d option 2 (on by default, user decision 2026-11-24;
    // ENGINE_RESTIR_PT_DIRECT_RESERVOIR=0 or the Editor turns it off): on denoiser frames (extraction), the reservoir holds
    // direct-light candidates only (path length k <= 2) and is
    // reused as usual; the path tree's bounce candidates (k >= 3) are summed fresh each frame and added at final shading.
    // No decorrelation swap then; firefly replacement stays and shows the fresh direct estimate in place of a reused
    // direct firefly (biased as before). Each part is an unbiased estimator of its own light. In the history signature
    // (reservoirs change meaning).
    bool directReservoir = true;
    // Wall boiling (experimental, on by default, user decision 2026-11-24; ENGINE_RESTIR_PT_LIGHT_BOUNCE=0 or the Editor's
    // "Bounce light: light-traced connections" turns it off): with the direct-light reservoir active, each primary surface
    // also gets one light-traced bounce sample (a light chosen near x1 by power over squared distance, one ray from it, its
    // hit x2 connected to x1) for the paths x1 -> x2 -> point or spot light, balance-heuristic MIS against the BSDF bounce's
    // NEE at x2. Unbiased; it removes the single-frame spikes from small bright lit patches that BSDF bounces rarely hit.
    // Compiled into its own ReGIR kernel variant (RESTIR_PT_LIGHT_BOUNCE), so it needs the ReGIR kernel's adaptive x1
    // sampler (above 64 lights); elsewhere it is inactive. Bounce light is never reused, so it is not part of the history
    // signature.
    // Documentation/Rendering/ReSTIR_PT_Wall_Boiling_Investigation.md.
    bool lightTracedBounce = true;
};

// M5.2: what the previous frame's reservoirs were computed against. History is reused only when every field matches
// (and a committed frame and its previous surfaces exist). Conservative by design (user M5.2 section 4): any lighting or
// path-space settings change drops history, since stored samples refer to light indices, light-selection densities and the
// sun and environment sampling distributions of their own frame.
struct ReSTIRPTHistorySignature
{
    std::uint64_t sceneResourceLifetime = 0;
    std::uint64_t topLevelAddress = 0;
    std::uint64_t topLevelGeometry = 0;
    std::uint64_t materialRevision = 0;
    // The frame's lights: table records, sun and environment (Renderer::HashReSTIRPTLighting).
    std::uint64_t lightingHash = 0;
    // The path-space settings: samples per frame, depth, roulette, candidates and the estimator flags.
    std::uint64_t settingsHash = 0;
    unsigned int width = 0;
    unsigned int height = 0;

    bool operator==(const ReSTIRPTHistorySignature&) const = default;
};

// M5.2: why the most recent BeginFrame did or did not reuse history (the first failing condition, in this order).
enum class ReSTIRPTHistoryDecision : std::uint8_t
{
    Reused,
    NoCommittedFrame,
    PreviousSurfacesUnavailable,
    Extent,
    SceneResources,
    TopLevel,
    Geometry,
    Material,
    Lighting,
    Settings
};

const char* ReSTIRPTHistoryDecisionName(ReSTIRPTHistoryDecision decision);

// M5.2: one recording's reservoir sets. The initial pass writes writeSet; previousSet (valid only with historyValid) holds
// the committed previous frame's reservoirs.
struct ReSTIRPTFrame
{
    std::uint32_t writeSet = 0;
    std::uint32_t previousSet = 0;
    bool historyValid = false;
    ReSTIRPTHistoryDecision decision = ReSTIRPTHistoryDecision::NoCommittedFrame;
};

// The local lighting discontinuity regions one frame can carry (RESTIR_PT_MAX_DISCONTINUITY_REGIONS in the shader).
inline constexpr std::uint32_t ReSTIRPTMaxDiscontinuityRegions = 16;

// GPU layout of RestirPtConstants (register b2, assets/Shaders/ReSTIRPT/RestirPtInitialBody.hlsli).
struct ReSTIRPTConstants
{
    std::uint32_t reservoirIndex = 0xFFFFFFFFu;
    std::uint32_t lightTileIndex = 0xFFFFFFFFu;
    std::uint32_t lightSamplingIndex = 0xFFFFFFFFu;
    std::uint32_t flags = 0;

    std::uint32_t tileCount = 0;
    std::uint32_t tileSize = 0;
    std::uint32_t primaryCandidates = 0;
    std::uint32_t rouletteFromVertex = 0;

    std::uint32_t frameIndex = 0;
    std::uint32_t counterIndex = 0xFFFFFFFFu;
    std::uint32_t debugView = 0;
    std::uint32_t tableLightCount = 0;

    // M5.1-4: the original reconnection criteria's minimum distance (2% of the smallest scene dimension), the roughness
    // threshold alpha_min, and the count of per-instance transforms bound at t1 (RestirPtInstanceTransform).
    float reconnectionMinDistance = 0.0f;
    float reconnectionRoughness = 0.2f;
    std::uint32_t transformCount = 0;
    // M5.5 Stage 1: the history scene's TLAS descriptor (0xFFFFFFFF without one).
    std::uint32_t historySceneIndex = 0xFFFFFFFFu;

    // M5.2 temporal reuse: the previous frame's reservoirs (raw UAV, 0xFFFFFFFF without history), whether history may be
    // read, the G-buffer motion texture (current UV - previous UV, view-depth change), and the temporal confidence cap.
    std::uint32_t previousReservoirIndex = 0xFFFFFFFFu;
    std::uint32_t historyValid = 0;
    std::uint32_t motionIndex = 0xFFFFFFFFu;
    std::uint32_t temporalConfidenceCap = 20;

    // The previous frame's G-buffer (PreviousGBufferView), valid only with historyValid.
    std::uint32_t previousDepthIndex = 0;
    std::uint32_t previousSurfaceIndex = 0;
    std::uint32_t previousGeometricNormalIndex = 0;
    std::uint32_t previousMaterialIndex = 0;

    // The previous camera (position, forward) and the current camera's forward: linear depth is dot(forward, p - camera).
    float previousCameraPosition[3]{};
    // M5.4 C5: the replay list (count, then packed pixels) and which temporal-pass dispatch (0 all pixels, 1 the list).
    std::uint32_t replayListIndex = 0xFFFFFFFFu;
    float previousCameraForward[3]{};
    std::uint32_t replayPass = 0;
    float cameraForward[3]{};
    // Optimization experiment B1a: the stagnancy maps (written this frame, read from the previous frame).
    std::uint32_t stagnancyOutputIndex = 0xFFFFFFFFu;

    // The previous camera-relative inverse view-projection (row-major, as PreviousGBufferCamera holds it).
    float previousInverseCameraRelativeViewProjection[16]{};

    // M5.3 spatial reuse (0xFFFFFFFF while it is off): the initial/temporal kernel's reservoirs, the final reservoirs,
    // the pair records, the pairing textures, and this frame's pairing transforms (uint4 per texture: size | flags << 16,
    // offset x, offset y, first texel).
    std::uint32_t spatialScratchIndex = 0xFFFFFFFFu;
    std::uint32_t spatialOutputIndex = 0xFFFFFFFFu;
    std::uint32_t pairsIndex = 0xFFFFFFFFu;
    std::uint32_t pairingIndex = 0xFFFFFFFFu;
    std::uint32_t pairingTransform[12]{};
    // M5.4 C6 dual motion vectors: the current camera-relative view-projection (row-major), the G-buffer raster identity,
    // and ReSTIR PT's instance history (last frame's, read; this frame's, written).
    float cameraRelativeViewProjection[16]{};
    std::uint32_t identityIndex = 0xFFFFFFFFu;
    std::uint32_t previousInstanceIndex = 0xFFFFFFFFu;
    std::uint32_t instanceOutputIndex = 0xFFFFFFFFu;
    // M5.5-B: the final-shading decorrelation probability (ReSTIRPTSettings::decorrelation).
    float decorrelation = 0.0f;
    // M5.4 C7: this frame's duplication map (written) and last frame's (read by temporal reuse).
    std::uint32_t duplicationOutputIndex = 0xFFFFFFFFu;
    std::uint32_t previousDuplicationIndex = 0xFFFFFFFFu;
    // Optimization experiment A5: spatial partners used (ReSTIRPTSettings::spatialPairs).
    std::uint32_t spatialPairCount = 3;
    std::uint32_t stagnancyPreviousIndex = 0xFFFFFFFFu;

    // M5.6 O0: this frame's ReGIR slots (raw UAV: 64-byte header, then uint2(RTXDI local index, W) per slot; 0xFFFFFFFF:
    // the light tiles) and, per RTXDI local light, uint2(table index, asuint(global realized probability)) (raw SRV).
    std::uint32_t regirSlotsIndex = 0xFFFFFFFFu;
    std::uint32_t regirLightsIndex = 0xFFFFFFFFu;
    std::uint32_t regirPadding[2]{};

    // M5.6 B4 cost attribution (diagnostic only, biased; ENGINE_RESTIR_PT_DIAG_SKIP_DISTANT): bit 0 skips the sun's NEE
    // stream, bit 1 the environment's. 0 in every rendering mode.
    std::uint32_t diagnosticFlags = 0;
    // M5.6 C7 reinvestigation: the duplication response cap' = lerp(cap, minCap, pow(saturate((D - threshold) /
    // (1 - threshold)), alpha)) as {alpha, minCap, threshold}; all 0 = the original M5.4 response (alpha 0.1, minCap 1,
    // threshold 0). ENGINE_RESTIR_PT_DUPLICATION_RESPONSE="alpha;minCap;threshold". Used only with Duplication::Apply.
    float duplicationResponse[3]{};

    // History through lighting changes: local lighting discontinuities since the committed frame (world-space spheres,
    // centre xyz and radius w; FrameLighting.h DetectLightingDiscontinuities). Temporal reuse is refused at surfaces inside
    // them this frame (RestirPtFindTemporalNeighbor); more regions than fit become a global drop.
    std::uint32_t discontinuityCount = 0;
    std::uint32_t discontinuityPadding[3]{};
    float discontinuityRegions[ReSTIRPTMaxDiscontinuityRegions][4]{};

    // The SHARC radiance cache at x3 (read only by kernels compiled with RESTIR_PT_SHARC): this frame's NVIDIA SHARC buffers
    // (raw UAV descriptors; sharcCapacity 0: no cache this frame, and paths run as without SHARC), the hash grid's camera
    // and scale.
    std::uint32_t sharcHashEntriesIndex = 0xFFFFFFFFu;
    std::uint32_t sharcAccumulationIndex = 0xFFFFFFFFu;
    std::uint32_t sharcResolvedIndex = 0xFFFFFFFFu;
    std::uint32_t sharcCapacity = 0;
    float sharcCameraPosition[3]{};
    float sharcSceneScale = 0.0f;
    float sharcRadianceScale = 0.0f;
    std::uint32_t sharcFlags = 0;
    std::uint32_t sharcPadding[2]{};
};

// M5.1-4 (M5.2 prerequisite): one render object's transforms, indexed by TLAS InstanceID (the render-object index, as PSR's
// transforms): the first three rows of its model matrix (world = M p) and of the inverse. Rebuilds a reconnection vertex
// from its stored instance, primitive and barycentrics without tracing (RestirPtRebuildVertex).
struct ReSTIRPTInstanceTransform
{
    float objectToWorld[12]{};
    float worldToObject[12]{};
    // M5.4 C6: previous-frame world space to this frame's for this instance's points (the identity when unmoved).
    float previousToCurrent[12]{};
    // M5.5 Stage 1: the previous frame's model matrix and its inverse (rows 0-2), for the history scene.
    float previousObjectToWorld[12]{};
    float previousWorldToObject[12]{};
};

// 448 bytes before the lighting discontinuity regions (16 + 16 x 16 bytes, ahead of the SHARC fields).
static_assert(sizeof(ReSTIRPTConstants) == 448 + 16 + ReSTIRPTMaxDiscontinuityRegions * 16,
    "ReSTIRPTConstants must match RestirPtConstants (b2).");
static_assert(offsetof(ReSTIRPTConstants, discontinuityRegions) % 16 == 0 &&
    offsetof(ReSTIRPTConstants, sharcHashEntriesIndex) % 16 == 0,
    "ReSTIRPTConstants: the discontinuity regions and the SHARC fields start HLSL registers.");

// GPU layout of RestirPtLightTileConstants (register b0, assets/Shaders/ReSTIRPT/RestirPtLightTiles.hlsl).
struct ReSTIRPTLightTileConstants
{
    std::uint32_t lightSamplingIndex = 0;
    std::uint32_t outputIndex = 0;
    std::uint32_t tileCount = 0;
    std::uint32_t tileSize = 0;

    std::uint32_t frameSeed = 0;
    std::uint32_t tableLightCount = 0;
    std::uint32_t padding[2] = {};
};

static_assert(sizeof(ReSTIRPTLightTileConstants) == 32, "ReSTIRPTLightTileConstants must match its HLSL layout.");

// Cooked-game port (2026-11-26): where ReSTIR PT's pipeline preparation reads its shaders. The Editor compiles from the loose
// shader root; a packaged game loads the cooked bootstrap (the renderer's shared copy, as the PSR auxiliary worker reads it).
// Exactly one is set.
struct ReSTIRPTShaderSource
{
    std::filesystem::path shaderRoot;
    std::shared_ptr<const std::vector<std::byte>> packagedShaders;
};

// The material tiers of ReSTIR PT's production kernels, one bit per feature compiled in (ShaderVariantTable.inl): the
// OpenPBR layered lobes (coat, fuzz, thin film, anisotropy) and, POM B2, the relief's own shadow at parallax occlusion
// mapping pixels. The base tier compiles both out; each other tier is a pipeline set of its own, prepared only once a
// published material needs it. The value is also the tier's index.
enum class ReSTIRPTMaterialTier : std::uint32_t
{
    Base = 0,
    Layered = 1,
    Relief = 2,
    LayeredRelief = 3
};

constexpr std::size_t ReSTIRPTMaterialTierCount = 4;

// The tier a material table needs.
constexpr ReSTIRPTMaterialTier ReSTIRPTMaterialTierFor(bool layeredLobes, bool parallax)
{
    return static_cast<ReSTIRPTMaterialTier>((layeredLobes ? 1u : 0u) | (parallax ? 2u : 0u));
}

// The suffix the variant table gives the tier's kernels ("Layered", "Relief", "LayeredRelief"; empty for the base
// tier), and a description for the log.
const char* ReSTIRPTMaterialTierSuffix(ReSTIRPTMaterialTier tier);
const char* ReSTIRPTMaterialTierDescription(ReSTIRPTMaterialTier tier);

// ReSTIR PT's compute pipelines and its pairing textures' texels, prepared apart from the pass (ReSTIRPTPass::
// PreparePipelines), so activating ReSTIR PT creates no pipeline on the render thread: the GPU driver takes 0.1-0.5 s per
// kernel with a warm shader cache and seconds per kernel after a shader change. Holds only immutable device objects and CPU
// data that no GPU work has used. ReSTIRPTPass::Initialize adopts the pipelines (adding references); Shutdown releases this
// set's own references.
// Startup work (4.2): the test-only and off-by-default kernels, in groups a development switch needs together. The
// Editor prepares the production kernels at startup (with the groups the saved settings already use) and a group on first
// use; a packaged game never cooks or prepares any group.
enum ReSTIRPTKernelGroup : std::uint32_t
{
    // The T-2.1 self-shift and the temporal and spatial round trips (RestirPtInitialDiagnostics.hlsl).
    ReSTIRPTKernelGroupDiagnostics = 1u << 0,
    // The M5.2 C0 visibility bench.
    ReSTIRPTKernelGroupVisibilityBench = 1u << 1,
    // Temporal reuse: the fused temporal kernel and the separate temporal pass (and a material tier's).
    ReSTIRPTKernelGroupTemporal = 1u << 2,
    // The M5.4 C7 duplication map.
    ReSTIRPTKernelGroupDuplication = 1u << 3,
    ReSTIRPTKernelGroupAll = (1u << 4) - 1u,
};

struct ReSTIRPTPreparedPipelines
{
    ComputePipeline initial;
    ComputePipeline temporal;
    ComputePipeline initialReGIR;
    ComputePipeline initialReGIRLightBounce;
    // Another material tier's production kernels, filled only in that tier's own set (`tier` below); the members above
    // are the base tier.
    ComputePipeline initialTier;
    ComputePipeline temporalTier;
    ComputePipeline initialReGIRTier;
    ComputePipeline initialReGIRLightBounceTier;
    ComputePipeline spatialPairsTier;
    ComputePipeline spatialResampleTier;
    ComputePipeline temporalPassTier;
    ComputePipeline diagnostics;
    ComputePipeline visibilityBench;
    ComputePipeline spatialPairs;
    ComputePipeline spatialPairsDiagnostics;
    ComputePipeline spatialResample;
    ComputePipeline temporalPass;
    ComputePipeline temporalPassDiagnostics;
    ComputePipeline duplication;
    ComputePipeline lightTiles;
    ComputePipeline regirLights;
    std::vector<std::uint32_t> pairingTexels;
    // The measured standard deviation of each pairing texture, for the log.
    std::string pairingSummary;
    // Whether the kernels are the SHARC twins (ShaderVariantTable.inl, the SHARC radiance cache at x3 compiled in), set by
    // the caller before preparing from NVIDIA SHARC's capability; a set prepared for the other choice is not adopted.
    bool sharc = false;
    // Set by the caller: Base for the pass's own set; another tier for that tier's set (only its *Tier members, no
    // pairing textures), prepared once a published material needs it and adopted by ReSTIRPTPass::AdoptTierPipelines.
    ReSTIRPTMaterialTier tier = ReSTIRPTMaterialTier::Base;
    // Startup work (4.2), set by the caller: whether the set holds its tier's production kernels (false for a set that only
    // adds kernel groups to an adopted one), and which ReSTIRPTKernelGroup bits it adds. Packaged sources ignore the groups.
    bool production = true;
    std::uint32_t groups = ReSTIRPTKernelGroupAll;
    // The first failure (a shader that did not compile, a pipeline that could not be created, a failed pairing check).
    std::string failure;

    void Shutdown();
};

// M5.1: configuration B's resources and pipelines. The initial-sampling pipeline has the path tracer's root signature plus
// RestirPtConstants at b2; PathTraceReferencePass::ExecuteReSTIRPT binds its targets and dispatches it, so the
// accumulation, output, statistics and extraction textures are the path tracer's own (NRD, RR and tone mapping read them
// unchanged). This pass owns:
//   the light tiles (tileCount * tileSize entries of 8 bytes, rebuilt every frame; raw UAV + SRV);
//   the reservoirs (ReservoirBytes per render pixel, two sets for temporal reuse (M5.2): one written per frame, the other
//     the committed previous frame's; raw UAV; render-extent sized);
//   the counters (CounterCount 32-bit words, raw UAV; written only when ReSTIRPTSettings::counters).
// Created on first use (Initialize, then EnsureReservoirs at the extent), released by ReleaseTextures (extent resources) and
// Shutdown. Lifetime follows the WaitForGPU model: the caller guarantees the GPU no longer uses what it releases.
// History follows RtxdiResources (R4): BeginFrame prepares a pending frame from the committed state; only CommitFrame,
// called after the command list was submitted, makes it the previous frame. A recording that fails or is never submitted
// is abandoned and changes nothing; InvalidateHistory drops both.
class ReSTIRPTPass
{
    public:
    static constexpr std::uint64_t ReservoirBytes = 64;
    // 64 counters, then the T-2.1 round 3 diagnostics (test only, self-shift runs): histograms, energy sums and dumps (see
    // RESTIR_PT_COUNTER_GEOMETRY .. RESTIR_PT_COUNTER_DUMPS in RestirPtInitialBody.hlsli). M5.6 Stage 0: 80 words from 14208,
    // the zero-contribution classification (RESTIR_PT_COUNTER_ZERO); M5.6 O0 investigation: 64 words from 14288
    // (RESTIR_PT_COUNTER_ZERO2); M5.6 B5: 32 words from 14352 (RESTIR_PT_COUNTER_SHIFTS). Point-light noise investigation
    // (temporary): 128 x 68 records of 20 words from 14384 (RESTIR_PT_COUNTER_PNC_LOG).
    static constexpr std::uint32_t CounterCount = 14384 + 128 * 68 * 20;
    // M5.3: pair records per pixel (three partners x 32 B) and the pairing textures' sides [P 3.2]. M5.4 C3 measured 20 B
    // records 1-2.6 ms slower in the pre-pass, so the record stays 32 B (RESTIR_PT_PAIR_BYTES).
    static constexpr std::uint64_t PairRecordBytes = 32;
    static constexpr std::uint32_t SpatialNeighbours = 3;
    static constexpr std::uint32_t PairingSizes[SpatialNeighbours] = {254, 230, 210};
    static constexpr std::uint64_t LightTileEntryBytes = 8;

    ReSTIRPTPass() = default;
    ReSTIRPTPass(const ReSTIRPTPass&) = delete;
    ReSTIRPTPass& operator=(const ReSTIRPTPass&) = delete;

    // Compiles and creates every pipeline into prepared and builds the pairing texels: the pipelines in parallel on a few
    // threads of their own, each with a private ShaderLibrary rooted at the source's shader root (DXC and the driver compile
    // one shader per call on the calling thread), or, packaged, sharing one library over the cooked bootstrap (loading is a
    // lookup there; only the driver's compile runs in parallel). A packaged set holds the production pipelines only: the
    // test-only and off-by-default kernels are never cooked, and nothing selects them without their development switches.
    // Touches no pass state and calls only free-threaded device methods, so it may run on any thread; it returns when
    // everything is created. False, with prepared.failure, when anything failed.
    static bool PreparePipelines(ID3D12Device* device, const ReSTIRPTShaderSource& source,
        ReSTIRPTPreparedPipelines& prepared);
    // Prepares the pipelines here, blocking (PreparePipelines with this SHARC choice), then initializes from them as below.
    // For callers without prepared pipelines.
    bool Initialize(ID3D12Device* device, const ReSTIRPTShaderSource& source, bool sharc, DescriptorHeap& descriptorHeap,
        ResourceUploader& uploader, const ReSTIRPTSettings& settings);
    // Adopts the prepared pipelines and pairing texels (a failed set fails the pass with its failure) and creates the
    // extent-independent buffers (sized for settings' tiles). Render thread. A failure is sticky and reported by
    // GetFailure. The caller still releases prepared (Shutdown).
    bool Initialize(ID3D12Device* device, DescriptorHeap& descriptorHeap, ResourceUploader& uploader,
        const ReSTIRPTSettings& settings, ReSTIRPTPreparedPipelines& prepared);
    // M5.3: the spatial scratch reservoirs and pair records for the current extent (created on first use).
    bool EnsureSpatialResources(ID3D12Device* device, DescriptorHeap& descriptorHeap);
    // M5.4 C5: the temporal pass's replay list (4 B per pixel + the count), created on first use at this resolution.
    bool EnsureReplayList(ID3D12Device* device, DescriptorHeap& descriptorHeap);
    // M5.4 C6: the two instance-history buffers (4 B per pixel each, paired with the reservoir sets).
    bool EnsureInstanceHistory(ID3D12Device* device, DescriptorHeap& descriptorHeap);
    // M5.4 C7: the two duplication maps (4 B per pixel each, paired with the reservoir sets).
    bool EnsureDuplicationMaps(ID3D12Device* device, DescriptorHeap& descriptorHeap);
    const ComputePipeline& GetDuplicationPipeline() const
    {
        return m_duplicationPipeline;
    }

    // M5.6 C7 reinvestigation (harness): the last committed frame's duplication map (duplicate count per pixel, 0..288).
    Buffer* GetCommittedDuplicationMap()
    {
        return m_committed && m_duplication[m_committedSet].GetResource() ? &m_duplication[m_committedSet] : nullptr;
    }
    // Creates the reservoirs for this render extent (recreated when it changes).
    bool EnsureReservoirs(ID3D12Device* device, DescriptorHeap& descriptorHeap, unsigned int width, unsigned int height);
    void ReleaseTextures();
    void Shutdown();

    bool IsInitialized() const
    {
        return m_initialized;
    }

    bool HasFailed() const
    {
        return m_failed;
    }

    const std::string& GetFailure() const
    {
        return m_failure;
    }

    // M5.2: prepares this recording's reservoir sets. History is usable only if a committed frame exists, its previous
    // surfaces are available, and its signature matches. Replaces any earlier pending frame.
    ReSTIRPTFrame BeginFrame(const ReSTIRPTHistorySignature& signature, bool previousSurfacesAvailable);
    // After the recording was submitted: its reservoirs become the previous frame's.
    void CommitFrame();
    // A recording that failed or was not submitted: nothing changes.
    void AbandonFrame();
    // Drops the committed and any pending frame.
    void InvalidateHistory();
    // Optimization experiment B1a: the two stagnancy maps (4 B per pixel each, paired with the reservoir sets).
    bool EnsureStagnancyMaps(ID3D12Device* device, DescriptorHeap& descriptorHeap);
    // M5.6 O0.1 pulsing diagnostics (debug view 3, harness only): per render pixel, float4(selected path length, sample age
    // + 100 when firefly replacement acted, reservoir shading luminance, shaded luminance), written by the final resample
    // pass. Extent-sized; released with the textures.
    bool EnsureDebugPixels(ID3D12Device* device, DescriptorHeap& descriptorHeap);

    Buffer& GetDebugPixels()
    {
        return m_debugPixels;
    }

    // M5.5 Stage 1: this recording's TLAS instances and geometry identity, committed and abandoned with its history.
    void SetPendingScene(const std::vector<RayTracingInstanceDescription>& instances, std::uint64_t geometryIdentity);
    // The history scene for this frame: a TLAS of the committed frame's instances over the current BLASes, rebuilt in this
    // frame slot's own TLAS (the slot's previous use is fence-complete when the frame index is acquired). Valid only while
    // the committed frame's geometry identity equals the current one (history is invalid otherwise). Returns its SRV
    // descriptor index, or 0xFFFFFFFF.
    std::uint32_t PrepareHistoryScene(ID3D12Device5* device, ID3D12GraphicsCommandList4* commandList, unsigned int frameIndex,
        DescriptorHeap& descriptorHeap, std::uint64_t geometryIdentity,
        const std::vector<const BottomLevelAccelerationStructure*>& bottomLevels);

    // Records this frame's light tiles from the published light sampling (lightSamplingIndex, of a table with
    // tableLightCount lights). Leaves the tiles readable by the initial pass. Returns false when nothing was recorded.
    bool RecordLightTiles(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer,
        std::uint32_t lightSamplingIndex, std::uint32_t tableLightCount, std::uint32_t frameSeed);
    // M5.6 O0: this frame's RTXDI local light -> (table index, global realized probability) map for the ReGIR candidates
    // (RestirPtReGIRLights.hlsl, from the published light sampling's translation and global alias table). Returns its SRV
    // index, or 0xFFFFFFFF (more than ReGIRLightCapacity table lights, or not initialized: the light tiles are used).
    std::uint32_t RecordReGIRLights(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer,
        std::uint32_t lightSamplingIndex, std::uint32_t tableLightCount);
    static constexpr std::uint32_t ReGIRLightCapacity = 1u << 17;
    // This frame's RestirPtConstants: the settings' flags, the buffers' descriptors and the frame inputs. extract and
    // rawDiffuseHitDistance select the signal outputs (RESTIR_PT_FLAG_EXTRACT / _RAW_DIFFUSE_HIT_DISTANCE).
    ReSTIRPTConstants MakeConstants(const ReSTIRPTSettings& settings, bool extract, bool rawDiffuseHitDistance,
        std::uint32_t lightSamplingIndex, std::uint32_t tableLightCount, std::uint32_t frameIndex,
        std::uint32_t productionRouletteDepth, const ReSTIRPTFrame& frame = {}, bool spatial = false,
        bool temporalPass = false) const;
    // Before the initial pass: reservoirs and counters writable.
    void PrepareInitialPass(ID3D12GraphicsCommandList* commandList);

    // The initial-sampling kernel for these settings: production, or a test-only variant when a diagnostic is on (the
    // T-2.1 self-shift, or the M5.2 C0 visibility bench). temporalPass: temporal reuse runs in its own pass this frame
    // (GetTemporalPassPipeline), so the production kernel samples and stores the canonical.
    const ComputePipeline& GetInitialPipeline(const ReSTIRPTSettings& settings, bool temporalPass = false) const
    {
        if(settings.selfShift || settings.temporalRoundTrip)
        {
            return m_diagnosticsPipeline;
        }

        if(settings.visibilityBench != ReSTIRPTSettings::VisibilityBench::None)
        {
            return m_visibilityBenchPipeline;
        }

        // M5.2: the fused temporal kernel only while temporal reuse is on without its own pass (the production kernel
        // carries none of its code).
        const TierPipelines* tier = ActiveTierPipelines();

        if(settings.reservoir && settings.temporal && !temporalPass)
        {
            return tier ? tier->temporal : m_temporalPipeline;
        }

        // M5.6 O0: the ReGIR variant only on frames whose ReGIR light map was recorded (RecordReGIRLights); its
        // light-traced bounce variant while that option and the direct-light reservoir are on.
        if(m_regirThisFrame)
        {
            const bool lightBounce = settings.lightTracedBounce && settings.directReservoir;

            if(lightBounce)
            {
                return tier ? tier->initialReGIRLightBounce : m_initialReGIRLightBouncePipeline;
            }

            return tier ? tier->initialReGIR : m_initialReGIRPipeline;
        }

        return tier ? tier->initial : m_initialPipeline;
    }

    // M5.4 C1: the temporal reuse pass (its diagnostics variant with the temporal round trip).
    const ComputePipeline& GetTemporalPassPipeline(const ReSTIRPTSettings& settings) const
    {
        if(settings.temporalRoundTrip)
        {
            return m_temporalPassDiagnosticsPipeline;
        }

        const TierPipelines* tier = ActiveTierPipelines();
        return tier ? tier->temporalPass : m_temporalPassPipeline;
    }

    // M5.3 spatial reuse's pre-pass (the diagnostics variant for the round trip) and resampling pass.
    const ComputePipeline& GetSpatialPairsPipeline(const ReSTIRPTSettings& settings) const
    {
        if(settings.spatialRoundTrip)
        {
            return m_spatialPairsDiagnosticsPipeline;
        }

        const TierPipelines* tier = ActiveTierPipelines();
        return tier ? tier->spatialPairs : m_spatialPairsPipeline;
    }

    const ComputePipeline& GetSpatialResamplePipeline() const
    {
        const TierPipelines* tier = ActiveTierPipelines();
        return tier ? tier->spatialResample : m_spatialResamplePipeline;
    }

    // Material tiers: the tier this frame's material table needs (ReSTIRPTMaterialTierFor), so the getters above return
    // that tier's kernels once its set was adopted. Until then they return the richest adopted tier it contains, or the
    // base tier, and the features that tier compiles out are not rendered. Set before each dispatch.
    void SetMaterialTier(ReSTIRPTMaterialTier tier)
    {
        m_materialTier = tier;
    }

    // Adopts another tier's set (ReSTIRPTPreparedPipelines::tier) into the initialized pass, once per tier; false when
    // the set failed or the tier already has one.
    bool AdoptTierPipelines(const ReSTIRPTPreparedPipelines& prepared);

    // Whether the tier's kernels can run (the base tier's always can).
    bool HasTierPipelines(ReSTIRPTMaterialTier tier) const
    {
        return tier == ReSTIRPTMaterialTier::Base || m_tierPipelines[static_cast<std::size_t>(tier)].ready;
    }

    // Startup work (4.2): the kernel groups these settings select (ReSTIRPTKernelGroup bits), as the getters below do.
    static std::uint32_t KernelGroupsFor(const ReSTIRPTSettings& settings);
    // The groups adopted for the base tier, and for the tier the getters return (the temporal group only; a tier set has no
    // other group).
    std::uint32_t GetAdoptedKernelGroups() const;
    std::uint32_t GetAdoptedBaseKernelGroups() const
    {
        return m_kernelGroups;
    }

    bool HasTierTemporalKernels(ReSTIRPTMaterialTier tier) const
    {
        return tier == ReSTIRPTMaterialTier::Base || m_tierPipelines[static_cast<std::size_t>(tier)].temporalKernels;
    }
    // Settings with every switch whose kernels are not adopted turned off, so a frame records the production kernels while
    // a group is prepared. Returns the groups that were missing.
    std::uint32_t RestrictToAdoptedKernels(ReSTIRPTSettings& settings) const;
    // Adopts a set prepared with production = false: its groups join the base tier, or, for a tier's set, that tier (which
    // must be adopted). False when nothing could be adopted.
    bool AdoptKernelGroups(const ReSTIRPTPreparedPipelines& prepared);

    // The tier the getters above return.
    ReSTIRPTMaterialTier GetActiveMaterialTier() const
    {
        const TierPipelines* tier = ActiveTierPipelines();
        return tier ? tier->tier : ReSTIRPTMaterialTier::Base;
    }

    // M5.3 test only: the pairing textures (packed int8 deltas, 254^2 + 230^2 + 210^2 texels) and their measured
    // standard deviation per axis.
    const std::vector<std::uint32_t>& GetPairingTexels() const
    {
        return m_pairingTexels;
    }

    // The most recently written reservoir set (the harness reads it after a frame).
    // M5.4 C6 test only: the committed frame's instance history (null without a committed frame or the history).
    Buffer* GetCommittedInstanceHistory()
    {
        return m_committed && m_instanceHistory[m_committedSet].GetResource() ? &m_instanceHistory[m_committedSet] : nullptr;
    }

    Buffer& GetReservoirs()
    {
        return m_reservoirs[m_lastWriteSet];
    }

    Buffer& GetCounters()
    {
        return m_counters;
    }

    Buffer& GetLightTiles()
    {
        return m_lightTiles;
    }

    unsigned int GetWidth() const
    {
        return m_width;
    }

    unsigned int GetHeight() const
    {
        return m_height;
    }

    // Bytes of GPU memory owned (light tiles, counters, reservoirs).
    std::uint64_t GetResourceBytes() const;

    private:
    bool Fail(const std::string& failure);
    void AdoptPipelines(const ReSTIRPTPreparedPipelines& prepared);
    bool UploadPairingTextures(ID3D12Device* device, DescriptorHeap& descriptorHeap, ResourceUploader& uploader,
        const std::string& pairingSummary);

    ComputePipeline m_initialPipeline;
    ComputePipeline m_temporalPipeline;
    ComputePipeline m_diagnosticsPipeline;
    ComputePipeline m_visibilityBenchPipeline;
    ComputePipeline m_lightTilePipeline;
    Buffer m_lightTiles;
    // M5.6 O0: the ReGIR light map (ReGIRLightCapacity x 8 bytes, then O0.2's table-indexed p_1, 4 bytes each; raw UAV +
    // SRV, rebuilt on frames that use ReGIR).
    ComputePipeline m_regirLightsPipeline;
    Buffer m_regirLights;
    // M5.6 O0: the production initial kernel with RESTIR_PT_REGIR, and whether this frame recorded the ReGIR light map
    // (reset by RecordLightTiles, which every ReSTIR PT frame records first).
    ComputePipeline m_initialReGIRPipeline;
    // Wall boiling (experimental): the ReGIR kernel with light-traced bounce connections (RESTIR_PT_LIGHT_BOUNCE).
    ComputePipeline m_initialReGIRLightBouncePipeline;
    bool m_regirThisFrame = false;
    // Material tiers: the production kernels of each tier but the base (indexed by tier; entry 0 is unused, the
    // members above are the base tier). A tier's set is adopted once and kept until Shutdown, so no recorded frame can
    // still use a replaced pipeline.
    struct TierPipelines
    {
        ReSTIRPTMaterialTier tier = ReSTIRPTMaterialTier::Base;
        ComputePipeline initial;
        ComputePipeline temporal;
        ComputePipeline initialReGIR;
        ComputePipeline initialReGIRLightBounce;
        ComputePipeline spatialPairs;
        ComputePipeline spatialResample;
        ComputePipeline temporalPass;
        bool ready = false;
        // Startup work (4.2): whether temporal and temporalPass were created (the temporal kernel group).
        bool temporalKernels = false;
    };

    std::array<TierPipelines, ReSTIRPTMaterialTierCount> m_tierPipelines;
    // Startup work (4.2): the ReSTIRPTKernelGroup bits adopted for the base tier.
    std::uint32_t m_kernelGroups = 0;
    ReSTIRPTMaterialTier m_materialTier = ReSTIRPTMaterialTier::Base;

    // The desired tier's set, else the richest adopted tier whose features it contains (its bits are a subset), else
    // nullptr for the base tier.
    const TierPipelines* ActiveTierPipelines() const
    {
        const std::uint32_t desired = static_cast<std::uint32_t>(m_materialTier);

        for(std::uint32_t tier = desired; tier != 0u; tier = (tier - 1u) & desired)
        {
            if(m_tierPipelines[tier].ready)
            {
                return &m_tierPipelines[tier];
            }
        }

        return nullptr;
    }
    Buffer m_counters;
    Buffer m_reservoirs[2];
    // M5.3: spatial pipelines, the scratch reservoirs (the initial/temporal kernel's output while spatial reuse runs), the
    // pair records and the pairing textures.
    ComputePipeline m_spatialPairsPipeline;
    ComputePipeline m_spatialPairsDiagnosticsPipeline;
    ComputePipeline m_spatialResamplePipeline;
    ComputePipeline m_temporalPassPipeline;
    ComputePipeline m_temporalPassDiagnosticsPipeline;
    Buffer m_spatialScratch;
    Buffer m_pairs;
    Buffer m_replayList;
    Buffer m_instanceHistory[2];
    Buffer m_duplication[2];
    // Optimization experiment B1a: the smoothed stagnancy maps (paired with the reservoir sets).
    Buffer m_stagnancy[2];
    Buffer m_debugPixels;
    ComputePipeline m_duplicationPipeline;
    Buffer m_pairing;
    std::vector<std::uint32_t> m_pairingTexels;
    // M5.2 history: the committed set and its signature; the pending recording's set and signature.
    bool m_committed = false;
    std::uint32_t m_committedSet = 0;
    ReSTIRPTHistorySignature m_committedSignature;
    bool m_pending = false;
    // M5.5 Stage 1: the pending and committed frames' TLAS instances and geometry identities, and one history TLAS per
    // frame slot (with its SRV descriptor).
    std::vector<RayTracingInstanceDescription> m_pendingSceneInstances;
    std::vector<RayTracingInstanceDescription> m_committedSceneInstances;
    std::uint64_t m_pendingSceneIdentity = 0;
    std::uint64_t m_committedSceneIdentity = 0;
    struct HistoryScene
    {
        TopLevelAccelerationStructure topLevel;
        std::uint32_t descriptor = 0xFFFFFFFFu;
        std::uint64_t geometryIdentity = 0;
        std::size_t instanceCount = 0;
        D3D12_GPU_VIRTUAL_ADDRESS address = 0;
        bool initialized = false;
    };
    static constexpr unsigned int HistorySceneSlots = 2;
    HistoryScene m_historyScenes[HistorySceneSlots];
    DescriptorHeap* m_historySceneHeap = nullptr;
    std::uint32_t m_pendingSet = 0;
    ReSTIRPTHistorySignature m_pendingSignature;
    std::uint32_t m_lastWriteSet = 0;
    std::uint32_t m_tileCount = 0;
    std::uint32_t m_tileSize = 0;
    unsigned int m_width = 0;
    unsigned int m_height = 0;
    bool m_initialized = false;
    bool m_failed = false;
    std::string m_failure;
};
