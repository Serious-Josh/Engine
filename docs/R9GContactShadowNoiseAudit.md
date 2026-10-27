# R.9G — contact-shadow edge noise audit

Date: 2026-10-27. Read-only investigation; no implementation, settings, shader or scene edits.

## 1. Executive summary

The leading explanation is **localized input variance or visibility/coverage discontinuities becoming more visible when confidence reduces temporal averaging**, potentially reinforced by fluctuating half-resolution confidence near contact boundaries. The strength-2/4 observations support this mechanism: stronger reduction brings little more responsiveness but exposes noticeably more edge noise, while constant 0.1 exposes noise everywhere. This is consistent with correct confidence generation; it does not establish a generator defect.

Strength 2 should remain the experimental reference. There is no evidence supporting a global confidence increase, stronger exponent, NRD retuning, or filter change as the next correction. The smallest safe next action is the existing-view comparison in section 9, which separates noisy lighting input from unexplained confidence activity at a stationary contact edge.

A specific source-level numerical asymmetry deserves attention if that comparison shows confidence events after the entire scene is still: **initial probe visibility uses the original normalized geometric normal, but replay uses a packed-and-decoded normal**. The ray-origin offset depends on that normal, so the paired rays need not be bit-identical. This is a verified asymmetry, not proof that it causes the observed speckles. If confirmed, canonicalizing the initial probe ray's normal to exactly the representation replay uses would be a much smaller correction than changing confidence strength or spatial weights.

Probe rotation is important for coverage, but a common explanation is incorrect: the shader does not subtract visibility at this frame's new representative from visibility at the preceding representative. It retraces the **same stored surface/light experiment**, then creates a separate new record. Rotation alone does not create false transitions in an otherwise identical, deterministic visibility experiment. It can still cause missed/phase-dependent change evidence, invalid/valid alternation and changing confidence footprints in mixed blocks.

No captures or interactive tests were performed for this audit. The reported improvements/noise are accepted as user observations. Screenshot pixels were not available for independent inspection in the accessible attachment; this report does not infer artifact coordinates, frequency or material properties from unseen images. Only this report was added; existing working-tree changes were preserved.

## 2. Verified rendering and confidence pipeline

Locations refer to the current working tree. NRD internal source references use the exact v4.17.3 sources previously retrieved under ignored `build/r9c/` and `build/r9d/`, matching the vendored version in `external/NRD/Include/NRD.h:30–32`.

| Stage | Verified behavior and source |
|---|---|
| Point-light sample | `assets/Shaders/RtxdiLights.hlsli:105–119` delegates table lights to `PathTraceSampleTableLight`; `PathTraceLights.hlsli:228–243` treats a point as a delta light with UV-independent direction, distance and incident irradiance. There is no sampled finite-emitter radius in this path. |
| Direct sampling/reuse | `PathTraceReference.hlsl:752–832`, `PathTraceRtxdiDirectLighting`: initial reservoir selection; optional fused spatiotemporal reuse for the reservoir-publishing sample; current contribution evaluation and final visibility. Reuse is not a direct reuse of yesterday's displayed shadow. |
| Shadow visibility | `PathTraceReference.hlsl:320–364`, `PathTraceOffsetRayOrigin`/`PathTraceVisible`: normal-based origin offset, TMin=0, positional-light TMax shortened by 1e−4. DXR/RayQuery variants return binary occlusion. Current selected-sample visibility is tested at `:805–808`. |
| Direct diffuse extraction | Split direct ray generation stores the same sample's direct diffuse at `PathTraceReference.hlsl:1560–1591`. The indirect/extraction path loads it through `PathTraceSplitDirectDiffuse:655–659` and adds vertex-zero direct diffuse at `:1137–1155`. |
| Indirect diffuse | Later-vertex lighting is carried by primary diffuse throughput (`PathTraceReference.hlsl:1155–1182`); miss contributions enter it at `:1052–1063`. If enabled, SHARC adds a cached incident suffix to diffuse extraction (`PathTraceRadianceCacheQuery.hlsli:493–507`). This can remain stochastic or temporally influenced despite a single direct point light. |
| Per-frame averaging | Extracted diffuse is this frame's `diffuseSum/frameSamples` (`PathTraceReference.hlsl:1455–1458`), separate from the presentation accumulation. It does not automatically inherit that accumulator's multi-frame mean. |
| RELAX preparation | `NrdPathTracePrepare.hlsl:437–478`, `CSPrepareRelax`: copies motion, obtains normal/roughness/view Z, reads combined diffuse and raw diffuse hit distance, demodulates/scales and packs RELAX input. Thus confidence affects the combined diffuse signal, not just primary direct lighting. |
| Probe generation | `NrdHistoryConfidence.hlsli:306–454`, `CSProbe`: evaluates stored old position/light/UV/visibility through current TLAS; writes `(T*dV,T)`, old-record geometry and state; then records one current representative per 2×2 block for the next update. Both visibility transition directions are detected. |
| Probe filter | `NrdHistoryConfidence.hlsli:487–565`, `CSFilter`; `NrdHistoryConfidencePass.cpp:245–287`: three 5×5 passes, strides 1/2/3, separate numerator/denominator filtering with plane/normal weights; original mapping has floor 0.1 and sensitivity 2 (`NrdHistoryConfidencePass.h:90–94`). |
| R.9F strength | `NrdHistoryConfidence.hlsli:567–575`: after original mapping, strength>1 raises the result to the configured exponent and reapplies the same floor. Strength 1 skips pow. Same R16F half-resolution output; no added temporal history. |
| NRD binding | `Renderer.cpp:7887–7913,7982–7994`; `Passes/NrdDenoiser.cpp:321–324,446–465`: existing frame flag and resource snapshot bind generated diffuse confidence and constant-one specular; absent completed generation uses neutral diffuse fallback. |
| Temporal accumulation | Exact NRD `RELAX_TemporalAccumulation.cs.hlsl:595–633`: bilinear confidence at prevUVSMB scales main/fast maximum frame counts and therefore current-frame blend weights; confidence does not persistently shorten stored history age. |
| Clamping/spatial stages | Exact `RELAX_HistoryClamping.cs.hlsl:252–341`: slow result is constrained by a spatial fast-history color box, with further acceleration/reset settings. `RELAX_Atrous.cs.hlsl:205–225` uses geometry/normal/material and luminance/variance-related spatial weights. Dispatch order is in `Source/Relax.cpp:216–277`. |
| Presentation/upscaling | RELAX resolve combines remodulated diffuse and specular (`NrdPathTracePrepare.hlsl:523–558`); signal views can replace it (`Renderer.cpp:8050–8096`). Optional DLSS/presentation resolution occurs at `Renderer.cpp:10551–10714`; tone-map selection at `:10745–10754` leads to exposure/AgX/sRGB (`ToneMap.hlsl:347–357`). |

The separate Engine temporal reconstruction is skipped when RELAX successfully handles the frame (`Renderer.cpp:6957`). It is not an unavoidable second pre-NRD diffuse denoiser. Optional SHARC before NRD and optional temporal upscaling after NRD are different mechanisms; their effective state in the user's run is unknown.

## 3. Rotating-probe analysis

The representative offsets are `(frameIndex&1, (frameIndex>>1)&1)`, giving (0,0), (1,0), (0,1), (1,1) over four submitted probe updates (`NrdHistoryConfidence.hlsli:378–379`). The old record is first loaded from the same half-resolution index (`:325–346`), evaluated at its stored world position and stored light UV, and compared against its stored visibility. Only after signal/state output does the shader choose and store the new experiment (`:372–454`).

**Narrow shadows and subpixel features:** one selected point may miss a thin moving visibility-change region while another offset sees it on another update. The resulting evidence can be intermittent even when the full-resolution lighting edge moves smoothly. A fast occluder can traverse a feature between representative samples; higher exponent cannot restore missing evidence. This fits residual trailing during fast movement, but has not been measured at the reported box edges.

**Silhouettes/mixed blocks:** the representative may belong to box, floor, another surface, or background on different updates. Record validity, position, normal, T and filter membership can change. The same confidence texel is later sampled by several full-resolution pixels, not all necessarily belonging to that represented surface. Alternating red/gray state in View 6 can reveal validity/representation changes. Different light visibility across two different surfaces is not itself a paired transition: each record's comparison remains at its own old position.

**Moving occluder with stationary receiver:** changed visibility at an old receiver point is a true event, even if rotation makes events spatially intermittent. With everything stationary, fixed lighting/TLAS and exactly reproducible initial/retrace rays, every valid record must have dV=0 regardless of which pixel is selected next. Rotation can move invalid/neutral regions or sample different input surfaces, but cannot alone generate repeated green/magenta paired changes. Persistent such changes after settling warrant checking actual scene motion and numerical reproducibility.

### A replay asymmetry relevant to tight contacts

Initial visibility is traced with `geometricNormal` derived from the current G-buffer and normalized at `:407–416`; `:451` uses it directly. The record then packs that normal at `:453`. Replay unpacks it at `:333` and passes the decoded normal to visibility at `:346`. Octahedral 16+16-bit normal encoding/decoding at `:144–160` need not reproduce the original normal exactly.

`NrdConfidenceOffsetRayOrigin:183–192` uses `int3(256*normal)` for ordinary coordinates and `normal/65536` near the origin. Quantization can change an integer offset component at a truncation threshold, or change a near-origin floating offset. As a mathematical illustration, encoding/decoding a normal near (0.5,√0.75,0) can move its x component just below 0.5, changing trunc(256*x) from 128 to 127. This is illustrative arithmetic, not a GPU reproduction or the user's actual normal.

Thus the stored surface/light UV is matched, but ray origins are not guaranteed bit-identical. A very small origin difference normally has no visible consequence; at nearly touching geometry or a grazing occlusion boundary it can potentially flip binary visibility and produce false confidence activity. The current source proves the asymmetry, **not** that it flips any rays in this scene. On well-separated planar receivers the effect may be negligible. This is separate from R.9E's multi-frame correspondence issue and requires no temporal hold to investigate.

## 4. Spatial filter and strength analysis

The Gaussian factor is exp(−(x²+y²)/2) on the 5×5 tap coordinates. Sample positions are separated by the pass stride, so composed support reaches ±12 probes per axis. The additional plane term is `saturate(1-|N·(Psample−Pcenter)|/(0.02*cameraDistance))`; the normal term is `saturate(dot(Ncenter,Nsample))²` (`NrdHistoryConfidence.hlsli:508–541`). Invalid-geometry neighbors are skipped.

This is geometry-aware but not shadow-aware. Coplanar regions with different visibility or material appearance can mix; there is no shadow-edge, material-ID or surface-connectivity test in this filter. Box/floor orthogonal normals should reject one another, which is evidence against unrestricted bleeding across every contact. Nearly parallel surfaces, bevels, tolerated plane differences and same-floor lit/shadow regions can still share evidence. Mixed blocks and asymmetric rejection/T weighting can shift an evidence centroid; a uniform planar symmetric kernel does not inherently shift it. Filtering can both spread a reduction into nearby unchanged pixels and dilute a narrow transition at its source.

The output carries the ratio of filtered T*dV and T into the original mapping. Changed low-T samples can be outweighed by high-T unchanged neighbors. Scaling a lone light uniformly is not itself a stochastic source and largely cancels in this ratio. Near a contact, coverage changes, grazing incidence and range/distance variation can alter relative T/validity, while small absolute lighting differences can have large perceptual contrast against a dark region.

NRD linearly upsamples the **mapped** R16F confidence. Its bilinear footprint can cross the coarse representation of a geometry/visibility boundary and mix low with neutral values. The exponent happens before interpolation, so pow(interpolated original confidence) is not the operation performed. No spatial or temporal discontinuity is added at the floor—max is continuous—but slope and the spatial region hitting the floor change.

For mature valid diffuse history at the unoverridden H=30, temporal alpha is approximately `1/(1+30*c_test)`:

| Original filtered c | c at strength 2 | c at strength 4 | Main alpha, strength 2 | Main alpha, strength 4 |
|---:|---:|---:|---:|---:|
| 0.95 | 0.9025 | 0.8145 | 0.0356 | 0.0393 |
| 0.80 | 0.6400 | 0.4096 | 0.0495 | 0.0753 |
| 0.60 | 0.3600 | 0.1296 | 0.0847 | 0.2046 |
| 0.40 | 0.1600 | 0.1000 | 0.1724 | 0.2500 |

Strength 4 reaches the floor for original c≤0.56234, versus c≤0.31623 at strength 2. It can therefore move a broader contact neighborhood into aggressive updates without adding visibility information. At original c≈0.6, its main update weight is about 2.4 times strength 2's. This explains why an experiment can gain little trailing improvement but lose noise stability.

The transformation's derivative before the floor is s*c^(s−1). Strength 2 can amplify some confidence fluctuations near neutral (derivative up to 2); strength 4 can do so more strongly near neutral (up to 4), but neither exponent amplifies every fluctuation everywhere. At the floor, further reductions are clipped. Also, exponentiating perfectly neutral 1 gives exactly 1: strength cannot generate low-confidence noise from an everywhere-neutral input.

## 5. Underlying lighting variance analysis

With one eligible point light, probe candidates all address the same delta source; random UV does not sample different emitter positions. Likewise, an ideal fixed receiver ray to that point has deterministic binary visibility. It would be incorrect to call stationary direct-edge speckles inevitable soft-shadow Monte Carlo sampling noise. Actual direct instability could instead involve sample weight/history behavior, raster/jitter coverage, geometry/material discontinuities, self-intersection or ray-origin issues. The code's current final visibility trace argues against simply displaying an old reservoir's visibility.

Indirect diffuse remains sampled even with HDRI None: the point can illuminate secondary surfaces. It contributes to the same RELAX diffuse input. In a dark direct shadow it may constitute a larger fraction of the visible result. Lower temporal averaging can reveal its variance, particularly where spatial filtering protects a high-contrast boundary. If SHARC is active, its cached suffix also enters that input; its effect cannot be assumed from the material or scene name. No runtime cache state was supplied.

Low direct contribution alone does not prove instability. Point attenuation and incidence are deterministic; low contribution can make indirect variance relatively more prominent, reduce valid probe evidence, or alter spatial weighting. Similarly, a static material boundary is not random by itself, but can limit available valid temporal/spatial samples and make noisy input harder to suppress.

Views 1,2,3 distinguish primary direct diffuse, combined diffuse input and denoised diffuse output. Noise already present in View 1 directs attention upstream of RELAX; stable View 1 with noisy View 2 suggests indirect/input composition; much stronger fluctuations in View 3 than View 2, together with changing confidence, support confidence/denoiser exposure. They cannot uniquely isolate a particular internal reservoir, SHARC lookup or NRD subpass, and tone-mapped screenshots cannot be directly subtracted to obtain indirect radiance.

## 6. RELAX temporal and spatial behavior

NRD 4.17.3 samples confidence at prevUVSMB and scales **both** main H and fast F limits. With successful reprojection, its weights are `max(1/(H*c+1),1/historyLength)` and `max(1/(F*c+1),1/historyLength)` (`RELAX_TemporalAccumulation.cs.hlsl:595–619`). Vendored defaults are H=30,F=6 (`NRDSettings.h:370–376`); Engine defaults disable separate antilag acceleration/reset and use five A-trous iterations (`Renderer.cpp:164–171`). Existing startup overrides may change effective values; they were not inspected in a running process.

Consequences of a correct local confidence reduction include faster shadow response, less averaging of current input noise, faster fluctuation in responsive history, and a different slow/fast relationship used for clamping. HistoryClamping builds a neighborhood box around fast history and includes the fast center (`RELAX_HistoryClamping.cs.hlsl:252–289`). Faster or noisier fast history can change both the box and the slow result, especially around high contrast. Ordinary clamping remains active even though the optional additional acceleration/reset amounts default to zero.

A-trous uses geometry, normal and material compatibility plus luminance/variance-dependent terms (`RELAX_Atrous.cs.hlsl:205–225`). At contact boundaries, incompatible samples are rejected and luminance edges reduce cross-edge smoothing. This protects the shadow but can leave fewer useful neighbors to average an isolated noisy pixel. It does not guarantee all edge variance is removed. Confidence-driven A-trous relaxation is zero in current defaults (`NRDSettings.h:429–431`), so lower confidence does not automatically introduce stronger spatial smoothing to replace lost temporal averaging. The upstream current-UV confidence reads in those optional terms do not establish an active coordinate bug here.

The confidence input is diffuse-only; specular confidence stays 1. View 3 is useful to confirm that the observed speckles belong to diffuse. If absent there but present in the normal image, investigate specular/presentation separately rather than automatically blaming diffuse confidence. Optional DLSS can modify signal-view presentation too, so keep its state unchanged and recorded throughout comparisons.

## 7. Existing diagnostic views

The caller selects views in `Renderer.cpp:8050–8077`. They replace presentation **after RELAX still runs**; switching views does not bypass its histories. The startup selector requires restarting to change view. All comparisons must keep generated confidence enabled, strength 2 and other settings fixed, with equal settling time after restarts.

| View | Shader location (`NrdPathTracePrepare.hlsl`) | Useful evidence | Limitations |
|---|---|---|---|
| 1 | `:616–629` | Per-frame mean primary RTXDI direct diffuse from the split buffer: direct-edge noise/coverage. | Material-modulated; includes actual estimator/reuse behavior, not a reservoir diagnostic. Requires split direct diffuse availability. |
| 2 | `:630–635` | Unpacked/remodulated combined diffuse entering RELAX: direct plus indirect input variance. | Before NRD's internal prepass; does not separate indirect/cache contributions. |
| 3 | `:636–641` | Remodulated denoised diffuse: output speckles and temporal behavior. | Does not expose main/fast histories or intermediate spatial passes. |
| 4 | `:651–658` | Nearest-probe raw dV ratio; red denotes no valid measurement. | A valid point probe's raw event is binary; no filtered support/consumer strength shown. |
| 5 | `:675–681` | Nearest-probe final filtered **strengthened** confidence at strength 2. | Not original strength-1 c, not NRD's bilinear value, and not a numerical display. |
| 6 | `:659–673` | Previous-record validity/type and transition direction: red invalid; gray unchanged table light; magenta visible→occluded; green occluded→visible. | Category, not T-weighted magnitude; unchanged gray includes unchanged lit and unchanged occluded samples. Not a full-resolution surface or motion-ID visualization. |

Views 4–6 map through pixel UV minus G-buffer motion (`:644–650`), then use integer loads; NRD uses linear clamp. Views are subsequently subject to the normal presentation/tone-map path (`Renderer.cpp:10745–10754`; `ToneMap.hlsl:347–357`). Faint fluctuations can be visually compressed, and a dark shade does not identify an exact confidence. View 5 cannot establish a consumer-confidence variance spectrum. These limitations do not prevent a qualitative stationary-versus-moving localization test.

## 8. Ranked hypotheses and likely correction scope

### 1. Locally reduced averaging exposes existing diffuse input variance

**Source evidence:** combined direct/indirect input, higher main/fast alpha with exponent, and geometry/luminance-aware spatial filtering. **Fit to observations:** strong; strength 2 improves lag/boiling overall, strength 4 adds little responsiveness while exposing more local noise, and constant 0.1 exposes it broadly. **Against:** input variance at these exact pixels has not been observed; a stable input with erroneous confidence/visibility could instead be responsible. **Unknown:** which component fluctuates and whether artifacts persist after complete settling. **Smallest test:** compare Views 2 and 3 on the stationary contact ROI; use View 1 only if View 2 is noisy. **Correction scope if confirmed:** a localized correction to the identified noisy estimator/input or a narrowly justified boundary confidence policy; do not preselect another global exponent or change global samples/NRD settings. No exact input fix is established yet.

### 2. Mixed-block/phase-dependent confidence coverage modulates valid noisy input

**Source evidence:** rotating representative, one geometry per 2×2 block, asymmetric geometry/T filtering, half-resolution bilinear consumer sampling. **Fit:** localized silhouettes/contacts are more vulnerable than broad planar shadows, and stronger exponent can make confidence modulation more consequential. **Against:** rotation does not itself create false paired dV in a reproducible static scene; orthogonal box/floor normal rejection limits bleed. **Unknown:** actual state/validity fluctuations and spatial correlation with the speckles. **Smallest test:** after moving then fully stopping, inspect Views 6 and 5 at the same ROI; look separately for validity alternation and true transition categories. **Correction scope if confirmed:** conservative mixed-surface/invalid-footprint handling within the diagnostic; changing representative selection or spatial weights would require separate evidence/approval. Do not implement a same-texel temporal hold.

### 3. Numerical visibility mismatch at contact, including probe normal round-trip

**Source evidence:** first and replay probe rays use different normal representations, and origin offset depends on that normal; direct rays also operate near discontinuous occlusion boundaries. **Fit:** tight contacts and isolated artifacts are compatible; amplified false reductions could worsen at strength 4. **Against:** normal error is small and often immaterial; no stationary false event was captured; one static point and planar normal can be highly stable. **Unknown:** whether ray origins actually cross an occlusion decision in this scene. **Smallest test:** hold everything still and inspect View 6 after settling; persistent green/magenta is stronger evidence of changing geometry or non-reproducible paired visibility than of representative rotation alone. View 1 distinguishes a broader lighting visibility problem from probe-only activity. **Correction scope if confirmed as probe-origin mismatch:** compute one packed normal, use its decoded value for the initial probe visibility ray, and store that same packed code for replay. Keep contribution selection/weights and production shadow rays unchanged. This is the smallest concrete candidate correction identified, but is not recommended for implementation before confirming stationary activity and ruling out genuine scene motion. No bias increase or global ray-offset retuning is justified.

### 4. Indirect/cache variance or high-contrast clamping/spatial neighborhoods dominate

**Source evidence:** later-bounce/optional SHARC diffuse and fast-history clamping/spatial edge stopping. **Fit:** complexity-dependent localized noise can survive stronger temporal responsiveness, while a simple scene stays smooth. **Against:** actual indirect fraction/cache state are unknown; the contact may be dominated by direct visibility. **Unknown:** View 1 versus View 2 stability and whether errors arise before or inside RELAX. **Smallest test:** when View 2 is noisy, check View 1 at the same settled framing/motion pattern; stable direct with fluctuating combined input supports this group. **Correction scope if confirmed:** isolate the responsible indirect/cache input before any local reconstruction or estimator change. The present audit cannot distinguish SHARC from ordinary indirect Monte Carlo variance and does not recommend disabling or modifying either as a presumed fix.

### 5. Specular, raster coverage or optional upscaling/presentation contributes

**Source evidence:** normal output includes specular; optional jitter/DLSS/resolution processing follows or influences sampling; views share presentation. **Fit:** narrow screen-space edges can be especially sensitive. **Against:** dependence on diffuse strength and constant-low noise strongly favors a diffuse temporal tradeoff over a wholly independent display defect. **Unknown:** active upscale/jitter settings and whether View 3 contains the exact artifact. **Smallest test:** compare normal output with View 3 without altering settings; if speckles are absent from diffuse output, localize the remaining path before changing confidence. **Correction scope if confirmed:** the demonstrated raster/specular/upscale boundary path only, not a global confidence change.

## 9. One recommended next experiment

**A staged, stationary-versus-move-and-stop comparison of the same small contact ROI using existing signal views, always at strength 2.** No GPU debugger or implementation change is required.

Keep the confirmed one-point/None/no-directional setup, regular RELAX, camera framing and all other rendering settings identical. Record effective strength/selector logs and whether SHARC, jitter and DLSS are active; do not change those variables during this experiment. For each startup-selected view, use the same initial placement and box movement, stop completely, and observe for several seconds (comfortably beyond the nominal 30-update history window). Include a stationary period before motion as well. Avoid comparing screenshots from different camera distances or restarting before histories have settled.

1. Begin with **Views 2 and 3**: combined input versus denoised diffuse. Observe whether the contact speckles already exist in input, whether they grow or disappear in output, and whether they remain after stopping. If View 3 lacks the normal image's speckles, investigate non-diffuse/presentation attribution instead.
2. Inspect **View 6** for the same stationary and moving ROI. Distinguish expected green/magenta during motion from persistent transitions after settling, and distinguish red/gray validity alternation from visibility transitions. Consult **View 5** if needed to see whether strengthened reductions/neutral patches visibly change in the same region. View 4 is only a useful confirmation of raw evidence if View 6 shows unexpected events; it is not a required extra run.
3. **Only if View 2 is noisy**, inspect **View 1** with the same protocol. This separates primary direct noise from fluctuations appearing with indirect/composed diffuse, without changing depth, samples, cache settings or strength.

Interpret the joint result:

| Result | Next conclusion |
|---|---|
| View 2 noisy, View 6 settles unchanged, View 5 broadly neutral/stable | Input variance is supported; persistent generator instability is weakened. Use conditional View 1 to localize direct versus other diffuse. |
| View 2 noisy only during motion; View 3 speckles decay with settling | Local responsiveness/noise tradeoff is favored over a stationary generator defect. |
| Persistent View 6 transition colors with the entire scene still | Check actual scene updates and paired-ray reproducibility; prioritize the normal/offset asymmetry over blaming rotation alone. |
| View 6 alternates invalid/unchanged near the silhouette; View 5 coverage shifts | Mixed-block/validity footprint deserves targeted examination; this is not automatically false visibility detection. |
| View 2 appears stable, View 3 fluctuates with View 5 activity | Confidence-modulated temporal/spatial processing is supported, but visually compressed input noise is not ruled out. |
| View 1 stable, View 2 noisy | Indirect/composed input becomes the stronger lead; this does not uniquely implicate SHARC. |

These are sequential visual comparisons, not simultaneous numerical correlation; startup restarts change random sample histories. Repeat a short movement if needed rather than demanding identical individual speckles. Tone mapping and nearest-versus-bilinear differences limit confidence attribution. If this experiment remains ambiguous, the smallest later diagnostic would be an explicitly enabled synchronized **in-app contact-ROI view** showing existing A/B/C plus nearest and actual bilinear confidence/state together, with numerical range labels. It would only inspect existing signals, add no sampling/history/tuning, and would be proposed separately. It is not needed before trying the existing views and was not implemented.

## 10. Remaining uncertainties and stopping point

No effective runtime settings, screenshots accessible for pixel inspection, numerical traces, exact complex-scene asset path, or repeated stationary observations were supplied to this audit. In particular, “stationary image generally smooth” does not establish that the contact speckles are stationary, transient, phase-locked, or exclusively diffuse. The active geometry/material configuration and degree of indirect illumination remain unknown.

The most likely mechanism is a local temporal-averaging/input-variance tradeoff, with coarse confidence coverage as a plausible contributor. The smallest concrete code correction candidate is paired-normal canonicalization **if** stationary raw events and ray-origin sensitivity are confirmed. Neither finding justifies sacrificing the demonstrated strength-2 improvements through broad tuning. No correction, renderer setting, scene edit, build or rendering test was performed.

Primary NRD source references: [temporal accumulation](https://github.com/NVIDIA-RTX/NRD/blob/v4.17.3/Shaders/RELAX_TemporalAccumulation.cs.hlsl), [history clamping](https://github.com/NVIDIA-RTX/NRD/blob/v4.17.3/Shaders/RELAX_HistoryClamping.cs.hlsl), [A-trous](https://github.com/NVIDIA-RTX/NRD/blob/v4.17.3/Shaders/RELAX_Atrous.cs.hlsl), [dispatch implementation](https://github.com/NVIDIA-RTX/NRD/blob/v4.17.3/Source/Relax.cpp).

Only `docs/R9GContactShadowNoiseAudit.md` was added outside ignored scratch. All 761 existing inventoried files retained their contents. Nothing was staged or committed. Investigation stops here.
