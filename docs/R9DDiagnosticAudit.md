# R.9D — RELAX generated-confidence effectiveness audit

Date: 2026-10-27. Investigation only. No source, diagnostic shader, configuration, or build-file changes.

## 1. Executive summary

The strongest explanation is **insufficient confidence reduction over the complete lifetime and footprint of stale history**, rather than a demonstrated R.9C binding or previous-UV defect. The generator measures a one-frame change in direct visibility, averages that evidence over a fairly wide neighborhood, and then maps the averaged change to confidence. RELAX consumes that confidence for the current temporal update; it does not permanently reduce its stored history length. These mechanisms can work well on broad, coherent transitions in the controlled scene while leaving a thinner or less coherent transition in the complex scene insufficiently accelerated.

The source supports this explanation quantitatively. On an ideal flat surface with equal positive contributions, the existing three filter passes turn an isolated changed probe into approximately **0.951 confidence**, and an infinitely long, one-probe-wide changed stripe into approximately **0.602 confidence** at its strongest point. A three-probe-wide stripe reaches approximately 0.133; four probes can reach the 0.1 floor. These are hypothetical distributions, not measurements of either scene. Reaching the floor requires a filtered contribution-weighted change fraction of at least **0.341886**.

The user's constant-0.1 result is decisive evidence that faster diffuse temporal updates can remove most of the observed trail. It does **not** show that generated confidence achieves 0.1 at the same pixels, for the same number of frames, or over the same spatial neighborhoods. Even a genuine local 0.1 sample does not make the complete spatially varying run equivalent to globally constant 0.1.

Two additional findings matter:

- View 5 is not a numerical display of the exact value sampled by NRD. It loads the nearest confidence texel, while NRD samples bilinearly. The signal-view output also follows the normal AgX/display path, so gray intensity is nonlinear and confidence 1 is not displayed as white. Faint bands cannot establish a numerical confidence range.
- RELAX receives direct **plus indirect** diffuse illumination. The probes measure direct-light binary visibility at the primary surface. Indirect changes and any SHARC cache effects are outside that measurement. Their contribution to the reported trail needs runtime isolation.

The confirmed one-active-point-light configuration is accepted throughout this report. Multiple local lights are **not** assumed. With exactly one eligible point-light record, the generator's four candidates all select that same delta light and UV has no effect. Random light selection is consequently a weak explanation for that configuration. Unchanged neighboring point-light probes still contribute to the filter denominator and can dilute transition evidence.

**Recommended next experiment:** one matched move-and-stop capture experiment using existing GPU resources, measuring the actual bilinearly sampled confidence and raw/denoised diffuse at the same trail pixels over time in generated and constant-0.1 runs. Section 8 defines the protocol and discriminating outcomes. No new diagnostic is necessary if an existing D3D12 capture/resource-export tool can inspect those resources.

### Evidence limits and read-only method

Interactive results below are the user's observations, not independently repeated measurements. This audit inspected current working-tree source, R.9C's report, vendored NRD headers/integration, and exact upstream v4.17.3 shaders. It ran only in-memory arithmetic for hypothetical filter distributions and read-only repository/hash inspections. It did not launch Engine, run a build, change selectors, edit scenes, or run an executable rendering test. This avoids interfering with the user's current work or causing application-generated saves/logs.

Exact upstream follow-on sources were downloaded only into the ignored `build/r9d/` scratch directory, after confirming the directory is ignored. The previously downloaded temporal shader in `build/r9c/` was reused. This report is the only new artifact outside ignored scratch. Existing file contents were protected with a SHA-256 inventory, checked again after the investigation. Nothing was staged or committed.

## 2. Verified implementation facts

References use repository paths and current working-tree line numbers. Upstream references use the pinned **v4.17.3** tag, not current master. These sources match the vendored version declaration in `external/NRD/Include/NRD.h:32`; the internal shaders are not present in the vendored SDK. This is source verification against that exact release, not disassembly of the shipped DLL.

### End-to-end path

| Stage | Exact implementation and relevant locations | Verified behavior |
|---|---|---|
| Light eligibility | `src/Scene/SceneLighting.cpp:151`, `BuildSceneLightingSnapshot`; `src/Renderer/Rtxdi/RtxdiLightBridge.cpp:78`, `BuildRtxdiLightSet` | Disabled authored lights are excluded from the snapshot. Current table lights and energy-bearing sun/environment records form the probe-addressable RTXDI light set. |
| Record surface | `assets/Shaders/NrdHistoryConfidence.hlsli:379`, `CSProbe` | One pixel per 2x2 block, cycling offsets (0,0), (1,0), (0,1), (1,1); reconstructs a world position from current depth and jittered inverse VP, obtains shading/geometric normals and faces them toward the viewer. |
| Candidate selection and record write | Same file:420–454, `CSProbe`; `NrdConfidenceContribution:275` | Four uniform light-index candidates; RIS selection proportional to unshadowed T. Quantized UV is sampled before storage. A selected valid sample stores world position, geometric normal, light index, UV, T and visibility in a 32-byte record. |
| Paired retrace | Same file:306–377, `CSProbe`; `NrdConfidenceVisible:198`; `NrdConfidenceSampleLight:239` | Reads the previous record at the same probe index, samples that light/UV at the stored world position, and retraces against the current TLAS. Writes `(T*abs(Vnow-Vold), T)`; detects both directions of binary visibility change. T is the stored previous contribution, not a newly estimated irradiance difference. |
| Raw output and next record | Same file:372–386 and 453–454 | Signal, old-record geometry and state are written before the thread replaces its own record with this frame's experiment. No other probe reads that record. View 6 is categorical old-record state, not weighted evidence strength. |
| Filtering and mapping | Same file:486–568, `CSFilter`; `src/Renderer/Passes/NrdHistoryConfidencePass.h:89`; `.cpp:243–285`, `Execute` | Three 5x5 passes, strides 1,2,3, signal→A→B→A; separate numerator/denominator averaging. Last pass writes `max(0.1,(1-saturate(2*r))^2)` or neutral 1 without valid evidence. No temporal accumulation of confidence. |
| Half-resolution resource | `src/Renderer/Passes/NrdHistoryConfidencePass.cpp:110–138`, `EnsureResources` | Native extent ceil(W/2)×ceil(H/2), R16_FLOAT, UAV and SRV. Same existing pass owns output through NRD consumption. |
| Frame sequencing | `src/Renderer/Renderer.cpp:6871–6875`, `RecordNrdConfidenceProbes:7608–7714`; `CommandContext.cpp:97` | Generator runs once, after path-tracing dispatch and before RELAX consumption, on the same command-list object as the DXR interface. Only a fully successful generation sets this frame's dispatch flag. |
| Confidence enablement/binding | `Renderer.cpp:2975–3009`, `BeginNrdRelaxFrame:7856–7885`, `RecordNrdRelax:7933–7983`; `src/Renderer/Passes/NrdDenoiser.cpp:321–324,446–465` | Generated mode takes precedence over R.9A with a warning; enables the existing generator and confidence flag. Current output becomes IN_DIFF_CONFIDENCE; IN_SPEC_CONFIDENCE is 1. If no completed generation, diffuse also binds 1. No second confidence system. |
| Native NRD wrapping | `external/NRD/Integration/NRDIntegration.hpp:645–675,787–800`; `NrdDenoiser.cpp:396` | D3D12 wraps the supplied native resource/format; views use its texture description. Snapshot restores input states. Vendored `NRDDescs.h` explicitly permits lower-resolution confidence and R8-or-better precision. |
| NRD previous-UV sampling | Upstream `Shaders/RELAX_TemporalAccumulation.cs.hlsl:402–418,595–605` | Screen motion yields `prevUVSMB`; diffuse confidence is sampled with `gLinearClamp` at that previous UV, mip zero. Engine copies G-buffer motion and supplies scale (-1,-1,1). |
| Temporal use | Same upstream shader:590–633 | Confidence linearly scales both main and fast maximum frame counts; the resulting update weights are reciprocal functions of those counts. The shared history-length output is not multiplied by confidence. |

The R.9C report's principal generation, binding, previous-space, resolution and fallback claims agree with the current source. Its validation section explicitly limits its GPU claims to source review; it is not evidence of measured confidence at the trail. The broader claim “NRD reads confidence in previous-frame space” specifically describes **temporal accumulation**. The A-trous shaders have separate current-UV reads, discussed below, whose effects are disabled by current settings.

### Resets and neutral behavior

`RecordNrdConfidenceProbes` builds identity from scene-resource lifetime, lighting/path temporal key, environment descriptors/weight and render extent (`Renderer.cpp:7624–7668`). Recreated resources, missing immediately preceding probe frame or identity mismatch set previousValid=0. `CSProbe:320–325` then writes no change/no geometry, and `CSFilter:555–566` writes 1. Submission commits identity and camera only after queue submission (`Renderer.cpp:10965–10974`); the current-frame dispatch flag resets before recording (`Renderer.cpp:9656`). A successful current generator output, not a retained old output, gates binding (`Renderer.cpp:7960–7966`).

Persistent reset/fallback would suppress useful confidence, but recognizable transitions contradict a generator that is always neutral. Intermittent resets remain unmeasured. No obvious stale-resource consumption or duplicate dispatch defect was identified in this audit.

## 3. Confidence-strength analysis

### What is averaged

For valid probe j, let `g_j=T_j*dV_j`, `w_j=T_j`, where dV is 0 or 1. The filter carries both channels, then divides their filtered values only in the final mapping. On a uniform planar region this is:

`r = sum(K_j*T_j*dV_j) / sum(K_j*T_j)`.

An unchanged valid probe has `(0,T)`: it **actively dilutes** changed evidence. An invalid probe has `(0,0)` and no geometry, so it is not equivalent to an unchanged valid probe. For spatially varying geometry, each pass has its own normalized operator; the result is their composition applied separately to numerator and denominator. It is not simply one global Gaussian with one global normalization.

The spatial factor is `exp(-2*(length(x,y)*0.5)^2) = exp(-(x*x+y*y)/2)` for offsets -2…2. Axial weights are 1, 0.606531, 0.135335 before normalization. The center starts with weight 1. Further factors are:

- `saturate(1-abs(dot(centerNormal, samplePosition-centerPosition))/(0.02*distanceToRecordingCamera))`;
- `saturate(dot(centerNormal,sampleNormal))^2`.

This is a plane-distance test, not a fixed screen-depth threshold. A coplanar floor passes even across lighting or material boundaries; no albedo, material ID, direct-light contrast or visibility edge is tested. Orthogonal normals reject, opposite normals reject, and invalid-geometry neighbors are skipped. Greater recording-camera distance tolerates more plane separation. Rejection can preserve a narrow change by removing unchanged neighbors, or exclude useful changed neighbors; it does not inherently make confidence weaker in complex geometry.

### Hypothetical strengths, not scene measurements

For an interior flat, fully valid grid with constant T and all geometry weights one, the kernel is separable. Its 1D base normalization is 2.483731886. Convolve normalized taps at strides 1,2,3; the resulting center coefficient is 0.111928415 in 1D and 0.012527970 in 2D. Support extends ±12 probes per axis (about ±24 render pixels); standard deviation is about 3.60 probes (7.19 render pixels), not a hard seven-pixel boundary.

| Changed-probe pattern; all other probes valid and unchanged | Strongest r after all passes | Mapped confidence |
|---|---:|---:|
| One isolated probe | 0.012528 | 0.950516 |
| Infinite stripe, width 1 probe (~2 render pixels) | 0.111928 | 0.602398 |
| Stripe, width 2 probes (~4 pixels) | 0.214948 | 0.325018 |
| Stripe, width 3 probes (~6 pixels) | 0.317968 | 0.132543 |
| Stripe, width 4 probes (~8 pixels) | 0.411324 | 0.100000 |
| Entire neighborhood changed | 1 | 0.100000 |

Stripe entries give the maximum over integer probe positions, away from image boundaries; actual orientation, block phase, T variation and geometric rejection change them. They show that the mapping **can** reach 0.1, but sparse categorical transitions do not imply it does. View 6 can look vivid even when their weighted support is too small.

| Filtered r | c | Mature-history main alpha, H=30 | Fast alpha, F=6 |
|---:|---:|---:|---:|
| 0 | 1 | 0.03226 | 0.14286 |
| 0.025 | 0.9025 | 0.03562 | 0.15588 |
| 0.05 | 0.81 | 0.03953 | 0.17065 |
| 0.10 | 0.64 | 0.04950 | 0.20661 |
| 0.20 | 0.36 | 0.08475 | 0.31646 |
| 0.25 | 0.25 | 0.11765 | 0.40000 |
| 0.30 | 0.16 | 0.17241 | 0.51020 |
| ≥0.341886 | 0.1 | 0.25000 | 0.62500 |

Values use the unoverridden default frame limits, a valid mature history, no checkerboarding and only the temporal equations. They are not final-output weights after clamping/spatial processing. Uniformly scaling the sole point light's intensity scales numerator and denominator together and should cancel, except numerical validity/precision extremes. Spatial differences in distance, incidence angle and range attenuation do not cancel: high-T unchanged neighbors can dominate low-T changed probes. As a simple single-neighborhood example, one changed sample of T=0.1 mixed equally with nine unchanged samples of T=1 gives r≈0.010989 and c≈0.95653. Reversing those contributions gives r≈0.526316 and c=0.1.

### Half-resolution interpolation and View 5

NRD samples the **already mapped confidence texture**, not the raw numerator and denominator. Bilinear interpolation with surrounding neutral texels can raise a local minimum. At equal four-tap weights, one 0.1 texel and three 1 texels yield 0.775 at the consumer. Other UV phases give other weights. A spatially uniform constant 0.1 remains 0.1 at every phase.

`CSResolveRelaxSignalView` maps pixels through current-minus-motion to previous UV (`NrdPathTracePrepare.hlsl:643–679`) but uses integer `Load` of a single texel in View 5. It does not show NRD's bilinear result. Its output is written to the normal presentation texture (`:683–688`). `RecordNrdRelax` does not set `m_pathTraceScalarViewLastFrame` for these views (`Renderer.cpp:8028–8050`); the tone-map selection at `Renderer.cpp:10717–10726` consequently uses the normal path unless another scalar diagnostic is independently active. `ToneMap.hlsl:347–358` applies exposure, AgX and sRGB encoding. The source comments describing displayed black/white endpoints are therefore not a reliable numerical display contract for the ordinary signal-view path.

At exposure 1, neutral grayscale through the actual AgX polynomial gives approximate display values: c=1→0.788, 0.95→0.780, 0.81→0.757, 0.602→0.712, 0.325→0.608, 0.133→0.445, 0.1→0.394. These are shader arithmetic values before monitor/UI effects; reduced-resolution presentation or DLSS can add another difference. Thus “most of the image light gray” is consistent even with neutral confidence. Faint dark bands suggest relative differences, but cannot establish exact confidence or consumer coverage.

## 4. Temporal and spatial alignment analysis

### Correct space is not complete history coverage

The previous-space convention is verified, not inferred from comments. G-buffer motion is currentUV−previousUV (`assets/Shaders/Triangle.hlsl:648–676`); RELAX preparation copies it (`NrdPathTracePrepare.hlsl:450–452`); `BeginNrdRelaxFrame` supplies (-1,-1,1) (`Renderer.cpp:7850–7852`). NRD forms previous UV by adding the scaled motion (`RELAX_TemporalAccumulation.cs.hlsl:402–418`). Stored probe positions reconstruct the preceding frame's G-buffer sample; retrace output stays at that probe's old grid location. Additional reprojection of the confidence output would apply motion twice.

A moving shadow on a stationary receiver has receiver motion near zero: the shadow moves through illumination, not through the G-buffer surface motion. The generator responds only where a particular surface-to-light visibility changed between consecutive probe frames. A shadow can move several pixels while this leaves a narrow changed strip near its boundaries; the persistently lit/occluded interior has dV=0. After an occluder stops and consecutive records see the same visibility, confidence returns toward 1 even if accumulated radiance remains stale. Cycling one of four pixels per block further makes thin/subpixel transitions phase-dependent. Stationary camera and receiver do not eliminate that spatial undersampling.

In a simplified temporal-only step response, one frame at c=0.1 leaves 75% of old main-history error. Thirty subsequent mature updates at c=1 leave approximately **28.0%** of the original error, whereas 31 updates continuously at c=0.1 leave approximately **0.0134%**. Baseline 31 updates leave approximately 36.2%. This intentionally ignores fast-history clamping, prepass/spatial filtering, variance and disocclusion: it demonstrates why a brief confidence pulse is not a persistent reset, not a prediction of visible trail duration. The fast path can strongly mitigate this main-history residual, which is one reason controlled-scene success remains compatible with the mechanism.

### What RELAX actually does

In exact v4.17.3 temporal accumulation, with a successful reprojection:

`alpha_main=max(1/(H*c+1),1/historyLength)`

`alpha_fast=max(1/(F*c+1),1/historyLength)`.

Both slow radiance/second moment and fast radiance lerp toward the current input (`:595–619`). Confidence itself is saturated but otherwise used linearly to multiply H and F; its effect on alpha is nonlinear. Confidence 0.1 is neither zero history nor a guaranteed reset. Stored `historyLength` is written independently (`:633`), so the next frame can immediately use a large age again when c returns to 1. Engine defaults are H=30,F=6 (`NRDSettings.h:370–376`); regular RELAX disables antilag acceleration and reset by default (`Renderer.cpp:164–171`). Startup overrides exist (`Renderer.cpp:2887–2955`); their actual runtime values must be recorded rather than assumed.

NRD's subsequent HistoryFix runs for short-history/disocclusion pixels, not merely because confidence is low (`RELAX_HistoryFix.cs.hlsl:37–42`). HistoryClamping still constrains slow history against a neighborhood of fast history, expanded to include the fast center (`RELAX_HistoryClamping.cs.hlsl:252–289`). Its separate acceleration/reset terms are multiplied by settings that Engine defaults to zero (`:292–330`). This does not disable ordinary fast-history clamping. A coherent low-noise floor can clamp effectively; mixed indirect/noisy neighborhoods can plausibly produce looser or already contaminated bounds. The latter is a hypothesis, not a measured cause here.

The dispatch chain includes hit-distance reconstruction, prepass, temporal accumulation, history fix, clamping, optional anti-firefly and five A-trous iterations (`Source/Relax.cpp:216–277`; Engine `Renderer.cpp:167–170`). Prepass can spatially modify the input before confidence-weighted temporal accumulation. A-trous can mix results from surrounding pixels whose temporal updates had higher confidence. Constant 0.1 updates those neighbors too; one local 0.1 texel does not reproduce that history field.

There is a real upstream qualification: A-trous and A-trousSmem sample confidence at **current pixelUv** and carry a TODO about previous UV (`RELAX_Atrous.cs.hlsl:107–118`, `RELAX_AtrousSmem.cs.hlsl:239–250`). However, their confidence-driven relaxation multiplier and normal/luminance relaxation settings are all **zero** in vendored defaults (`NRDSettings.h:429–431`) and Engine's regular RELAX settings do not change them. Those reads therefore do not change the spatial weights in this configuration. This is not evidence of an active R.9C coordinate bug or a reason to reproject its output.

### Boundaries and moving surfaces

RELAX history reprojection uses a validated bilinear/bicubic footprint, previous depth/material compatibility and normal rejection (`RELAX_TemporalAccumulation.cs.hlsl:80–157,163–210`). Confidence sampling is ordinary bilinear sampling of a coarser grid, without that same per-tap history validity selection. Each probe represents only one old surface in its block, not all full-resolution receiver surfaces. Invalid centers map to neutral confidence; edge interpolation can mix neutral or unrelated-surface confidence. The filter has plane/normal rejection but no material/connectivity test; its increasing strides can connect compatible surfaces across gaps. These are representational differences, not proof that a particular trail is misaligned.

For a static receiving floor the old world position remains physically appropriate. For a moving/deforming receiving surface, the probe retains an old world position and has no object identity/barycentrics or motion-based world-position update; it does not necessarily retrace the material point whose history NRD reprojects. Geometry disocclusion can reject history independently. Camera motion, jitter, odd render dimensions and boundary coverage deserve inspection if failure concentrates there. They are lower-priority explanations for a stationary-camera shadow on a stationary floor. No blanket correspondence guarantee is claimed for moving receivers.

## 5. Direct-versus-indirect signal analysis

### Composition entering RELAX

`PathTraceRtxdiDirectLighting` performs initial sampling and optional spatiotemporal reservoir reuse, then explicitly evaluates current visibility and produces diffuse/specular contributions (`PathTraceReference.hlsl:752–832`). Split-direct extraction writes per-sample direct diffuse (`:1549–1591`). The indirect path loads that direct diffuse at vertex zero (`:1137–1142`) and adds it to extraction; later-vertex lighting contributes through the primary diffuse throughput (`:1152–1182`). Miss/environment contributions also enter that throughput (`:1052–1063`). Therefore primary diffuse extraction includes direct RTXDI lighting plus indirect transport assigned to the primary diffuse lobe, not just Lambertian direct shadow visibility.

Extraction diffuse is averaged over **this frame's** samples (`PathTraceReference.hlsl:1408,1459–1464`). The separate presentation accumulator can average across frames (`:1433–1443`), but it is not the extracted RGB consumed by `CSPrepareRelax`. Preparation reads raw diffuse and divides by material factor/radiance scale, then packs RELAX radiance (`NrdPathTracePrepare.hlsl:468–478`). The same combined diffuse signal is passed as `inputs.diffuseRadianceHitDistance` (`Renderer.cpp:7926`). Diffuse confidence applies to that full diffuse temporal accumulation, not just a direct component. Specular confidence remains 1; constant-low mode does likewise, so the successful constant-low comparison points toward diffuse history but does not isolate its direct/indirect parts.

If active, SHARC substitution adds its cached incident suffix into `extraction.diffuse` (`PathTraceRadianceCacheQuery.hlsli:497–508`); sparse SHARC is allowed on Raster ReSTIR DI (`Renderer.cpp:6832–6850`). Its effective runtime state is unknown here. Engine's separate path-trace temporal/spatial reconstruction is excluded when a regular RELAX frame succeeds (`Renderer.cpp:6928–6939`); it is not an unavoidable extra pre-NRD temporal filter. SHARC's separate Engine history-prior mechanism must not be confused with this NRD input.

### What direct visibility does not measure

The probes do not test changing bounce illumination, changing secondary-path visibility, changing material response, cache publication/lag, or the indirect radiance carried by the primary diffuse throughput. One point light can still illuminate a complex scene through many indirect paths. Direct visibility may remain unchanged on a receiver while its indirect component changes. Conversely, a direct visibility transition may be a small fraction of the combined diffuse energy.

RTXDI is an additional temporal reuse mechanism **before** NRD, but source inspection also shows a fresh final visibility ray for the selected sample (`PathTraceReference.hlsl:800–809`). In the one-delta-point case there is no alternative light direction to sample, which weakens a simple “old reservoir selects an old light/shadow” explanation. Reservoir weights/history validity and indirect/cache behavior are not thereby proved irrelevant. View 1 (direct), View 2 (combined prepared input), View 3 (denoised diffuse), or direct GPU-resource inspection can distinguish input lag from lag introduced after preparation. Views 1 and 2 must be compared in linear resource values for quantitative subtraction; display screenshots are tone mapped separately.

The complex scene may have more indirect energy, more geometric/material boundaries or different projected shadow speed/width than the controlled scene. Those facts were not supplied or measured and are hypotheses to test, not assumed scene properties. No source-only calculation can assign a direct-versus-indirect percentage to the observed trail.

There can also be temporal processing **after** NRD: the renderer's optional DLSS path consumes the final scene color (`Renderer.cpp:10522–10660`). Its active state/render scale is unknown. It can affect both trail presentation and signal views, but the constant-confidence success under otherwise identical settings makes it a secondary confound, not the leading cause. Capture before upscaling to separate it.

## 6. Probe sampling and weighting analysis

### Eligible set with one active point light

`BuildSceneLightingSnapshot` skips `enabled=false` (`SceneLighting.cpp:170–173`). `BuildRtxdiLightSet` includes all lights already in the GPU table, groups locals before directionals, then conditionally adds the sun and environment (`RtxdiLightBridge.cpp:84–154`). A None environment's zero total weight fails its positive-weight gate (`:24–30`); a zero-energy sun fails its energy gate (`:8–22`). `FrameLighting.cpp:77–99` removes the procedural compatibility sun from authoritative file scenes when no directional owner is supplied.

An **enabled but zero-intensity/black** table light is not culled by `BuildRtxdiLightSet`; nor is a table light culled globally because this receiver lies outside its range or faces away. Such a candidate yields no positive T at this receiver and cannot be chosen over a positive-T candidate. If all four candidates fail, the record becomes invalid/red and later neutral, not an unchanged gray local-light record. These are implementation facts, not a claim that extra enabled lights exist in the user's scene.

Accepting the confirmed single active point and absence of other eligible records, all candidates use light index zero. Point/spot/table-directional sampling ignores UV (`assets/Shaders/RtxdiLights.hlsli:4–8,105–119`), and table points are delta lights (`PathTraceLights.hlsli:228–243`). Four candidates do not yield four independent shadow rays or point-light area samples: the pass traces one selected visibility when recording and one previous visibility when evaluating. There is no finite point-emitter radius or penumbra sample in this path. The current roughness/shadow appearance does not change that light model.

Without a capture of the effective light-record buffer, source inspection cannot independently enumerate the user's live scene. The report does not downgrade the user's confirmation or treat multiple-light dilution as the cause of the single-point result. Multi-light/sun/HDRI selection explains the initial diagnostic distribution, but is insufficient for the decisive one-point comparison.

### T and validity

`NrdConfidenceContribution` computes `luminance(max(Le,0))*max(Ns·L,0)/pdf` for non-delta samples; for a point pdf=0, so no division occurs. It also requires positive geometric-normal incidence. Material albedo/pi is omitted. `SceneLights.hlsli:34–66` supplies inverse-square attenuation clamped at 1 cm, multiplied by `(1-(d/range)^4)^2` inside range; outside range is zero. These are shared lighting definitions, not a separate probe attenuation model.

For the same static point/receiver, stored T remains representative of its unshadowed direct contribution. Uniform intensity changes invalidate history through the light identity; between stable runs a uniform intensity scale largely cancels out of r. Differences in distance, orientation and range over the filter footprint change relative T and therefore response. Weakly illuminated changed probes can be outweighed by bright unchanged neighbors. Geometry can change the apparent transition's width in screen pixels even with the same delta light. None of this requires a second active light.

The four-candidate RIS procedure preferentially records high-T samples but stores the chosen T, not a reservoir-normalized estimate of all-light gradient energy. With several eligible lights this is a heuristic confidence signal, not a proven unbiased estimator of combined diffuse change. With one delta point all valid candidates at a surface have the same T, so this distinction is not a leading explanation. Invalid records from out-of-range/negative-incidence surfaces, mixed surface blocks and finite/zero checks remain possible; their frequency is unmeasured. The reported recognizable transition bands demonstrate successful records in at least those regions.

## 7. Ranked hypotheses and discriminating tests

The first two mechanisms can act together. Rankings reflect source support and compatibility with **both** scenes, not measured causal attribution.

### 1. Spatial averaging and interpolation leave consumer confidence too high

**Evidence:** the exact filter/mapping yields c≈0.951 for isolated evidence and c≈0.602 for a thin stripe in the ideal case; unchanged neighbors add denominator; T variation and bilinear upsampling can weaken minima. **Observations:** clear View 6 bands plus only modest-looking View 5 changes and a large advantage for constant 0.1 are compatible. Controlled success is compatible with broader/coherent transitions or favorable rejection, but those differences are unverified. **Unknown:** actual c at the affected full-resolution pixels.

**Smallest test:** inspect the generated R16F resource and reconstruct the actual previous-UV bilinear sample at trail pixels in a captured frame. **Expected:** this hypothesis predicts c materially above 0.1 over much of the residual trail despite categorical changes. Consistently c≈0.1 across the affected footprint would weaken it and favor duration, indirect or spatial-history explanations.

### 2. One-frame change evidence ends before stale history is removed

**Evidence:** paired dV compares only consecutive frames; no temporal persistence in confidence; RELAX scales this frame's weights without persisting a shortened age. **Observations:** constant-low's sustained response explains its advantage and stationary-noise cost. Controlled success is possible when fast history/clamping resolves error within the available pulse. **Unknown:** pulse duration versus actual output-error decay in either scene.

**Smallest test:** hold camera and receiver still, move the occluder once and stop; track confidence and residual error at the same floor pixels through stop+1,+2,+4,+8,+16,+32 frames. **Expected:** confidence recovers near 1 while denoised error persists if duration is limiting. Persistently low c throughout the trail's decay weakens this explanation and shifts attention to input mismatch or later spatial stages.

### 3. Combined diffuse changes exceed what primary direct visibility represents

**Evidence:** primary diffuse includes later-bounce and optional SHARC suffix radiance; the probe signal measures only one primary direct visibility. **Observations:** a simple direct-dominated scene can succeed while a complex scene fails; that energy difference has not been measured. Constant-low affects both direct and indirect diffuse, fitting the comparison. **Unknown:** the trail's indirect fraction and effective SHARC state.

**Smallest test:** compare existing direct-diffuse A, prepared combined-diffuse B and denoised-diffuse C resources through the same move-and-stop sequence; remodulate B before subtracting A. **Expected:** a delayed/changing B−A component without corresponding dV supports mismatch; clean A and B with lag only in C shifts responsibility to NRD coverage/weighting. A already lagging points upstream of RELAX. No new separation buffers are required for this first distinction.

### 4. Spatial/history-footprint differences dilute a locally successful temporal update

**Evidence:** coarse confidence does not share NRD's validated full-resolution reprojection footprint; boundary blocks can represent another surface; prepass/clamping/A-trous mix neighborhoods; global 0.1 accelerates those neighbors too. Confidence-driven A-trous relaxation is disabled, so low confidence does not explicitly open a new spatial response mode. **Observations:** scene-dependent geometric/noise differences could explain both results; these differences are unverified. **Unknown:** whether residuals concentrate near boundaries or appear only after a particular NRD stage.

**Smallest test:** inspect temporal main/fast outputs and final denoised output in one captured affected frame, sampling a flat interior ROI and a boundary ROI. **Expected:** a corrected temporal center followed by contaminated/lagged final neighborhoods supports spatial propagation; residual already in temporal output supports strength/duration instead. Do not modify iteration counts or weights for this audit.

### 5. Surface correspondence, jitter or mixed-block coverage fails locally

**Evidence:** no motion transport of old probe world position; one old surface per 2x2 block; confidence and radiance use different sampling footprints. **Observations:** coherent transitions and controlled success argue against a global sign/double-reprojection bug. The user's description does not establish camera/receiver motion or a boundary-only failure. **Unknown:** actual prevUV, motion, selected block surface and history rejection at bad pixels.

**Smallest test:** capture with stationary camera and static receiver, inspect motion and probe geometry at flat-floor trail pixels. **Expected:** consistent correspondence with near-zero receiver motion weakens this hypothesis; divergence confined to moving receivers/boundaries or varying with subpixel phase supports it. An additional reprojection must not be presumed corrective.

### 6. Upstream reuse, intermittent neutral fallback, or downstream upscaling contributes

**Evidence:** RTXDI reuses reservoirs but retraces final visibility; optional SHARC precedes NRD; reset/fallback can produce neutral confidence; optional DLSS follows NRD. **Observations:** effective generated response and clear transitions oppose permanent fallback; constant-low largely curing the trail opposes a wholly independent upstream/downstream defect. With one point, light-direction selection ambiguity is especially weak. **Unknown:** effective switches, reset log events, raw input timing and post-upscale contribution.

**Smallest test:** inspect actual dispatch bindings/reset logs, A/B input resources, pre-upscale C and displayed result in the same capture sequence. **Expected:** stale A/B implicates upstream input; clean C plus trailing displayed output implicates presentation/upscaling; neutral bindings or repeated reset frames implicate missing evidence. Correct bindings and clean upstream/presentation narrow the cause to the first four hypotheses.

## 8. One recommended next experiment

### Matched numerical move-and-stop capture, using existing resources

Choose a static receiving-floor ROI where the complex scene trails. Keep the confirmed single point, HDRI None and disabled directionals. Hold camera, receiver, render extent, sample count, path depth, effective RELAX overrides, SHARC and upscale settings fixed and record those effective values. Repeat one clearly specified occluder move and stop after equal warm-up for **generated** and **constant-low** runs; restart for startup selectors and explicitly unset the competing confidence selector. Obtain a stationary reference after settling with the occluder at its final position. No scene save or code/parameter tuning is needed.

Use an already available D3D12 GPU capture/resource-export tool if possible. Capture the motion interval and stop+1,+2,+4,+8,+16,+32 submissions, identifying frames by actual submitted frame index rather than UI refresh. Inspect/export existing resources at their relevant dispatch points:

1. R.9B raw `(TdV,T)`, final R16F confidence and probe state; previous-record geometry when checking boundary correspondence.
2. The bound IN_DIFF_CONFIDENCE identity/extent, G-buffer motion and RELAX frame constants. Reconstruct c at `prevUVSMB` with linear clamp, and compare it with View 5's nearest texel value.
3. Split-direct diffuse A; prepared diffuse B, remodulated into A's units; temporal main/fast diffuse outputs; final RELAX diffuse C before upscale/tone mapping. Existing intermediate NRD resources suffice for capture inspection; no production export path is proposed.

For the same floor pixels, record numerical c and the fraction of the ROI with sampled c≤0.15, 0.15<c≤0.3, and c>0.3; how many consecutive updates receive low c; linear A, B−A and C error against the settled reference; and where a residual first appears in the stage chain. Account for noisy inputs with a small flat ROI and repeated matching passes, not a single random pixel. Separate lit→shadow and shadow→lit cases. A GPU debugger's resource inspection/export should be used without editing shader values or replay parameters.

This one experiment distinguishes the leading alternatives:

| Captured result | Interpretation |
|---|---|
| Sampled c remains high at the trail | Strength/coverage dilution is directly supported. |
| c reaches low values briefly, then returns high while C remains stale | Temporal-duration mismatch is supported. |
| Direct A changes promptly, indirect B−A changes/lags without dV | Direct-versus-indirect representation mismatch is supported. |
| A/B timely; temporal outputs already stale | Concentrate on confidence strength, duration and actual history weights. |
| Temporal center timely; C becomes contaminated | Concentrate on clamping/spatial neighborhoods and full confidence footprint. |
| C timely before upscale, final display stale | Investigate the downstream temporal presentation path separately. |
| Wrong/neutral bound resource or repeated reset despite expected generation | Revisit integration/history validity with actual frame evidence. |

Existing Views 1–6 can guide ROI choice and provide a qualitative version of the experiment, but cannot supply exact bilinear consumer c or simultaneous stage timing. If no capture tool can inspect/export the required intermediates, the narrow future diagnostic would be a **readback-only ROI telemetry path**, enabled explicitly, exporting submitted frame ID, bound confidence source/extent, prevUV, nearest and bilinear c, `(TdV,T)`, and linear A/B/C ROI values. Its sole purpose would be temporal correlation; it would not modify confidence, probe sampling, histories, NRD settings or presentation. That diagnostic is a proposal only and was not implemented.

## 9. Remaining uncertainties and conclusion

No numerical confidence captures, images, active selector logs, render-scale state, SHARC state or matched motion sequences from the complex scene were supplied. The exact live scene file was not identified, so no unrelated scene was substituted for it. The controlled project points to `Scenes/ControlledPointOnly.scene`; its on-disk None environment and one enabled point are consistent with the stated setup, but on-disk data is not proof of a live renderer's settings.

The audit establishes plausible mechanisms and rules out neither a localized integration issue nor a runtime-only problem. It does establish that categorical visibility bands and faint confidence bands are insufficient evidence of a sustained low confidence at the NRD consumer; that one-frame and constant-low confidence are materially different interventions; and that direct visibility is narrower than the denoised signal. No production defect, visual improvement, optimal mapping or tuning change is claimed.

Primary NRD references: [temporal accumulation](https://github.com/NVIDIA-RTX/NRD/blob/v4.17.3/Shaders/RELAX_TemporalAccumulation.cs.hlsl), [history clamping](https://github.com/NVIDIA-RTX/NRD/blob/v4.17.3/Shaders/RELAX_HistoryClamping.cs.hlsl), [history fix](https://github.com/NVIDIA-RTX/NRD/blob/v4.17.3/Shaders/RELAX_HistoryFix.cs.hlsl), [A-trous](https://github.com/NVIDIA-RTX/NRD/blob/v4.17.3/Shaders/RELAX_Atrous.cs.hlsl), [A-trous shared-memory variant](https://github.com/NVIDIA-RTX/NRD/blob/v4.17.3/Shaders/RELAX_AtrousSmem.cs.hlsl), [dispatch/settings implementation](https://github.com/NVIDIA-RTX/NRD/blob/v4.17.3/Source/Relax.cpp).

Investigation stops here. No R.9D code changes, tuning, staging or commit were performed.
