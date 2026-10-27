# R.9H — indirect bright-pixel audit

Date: 2026-10-27. Investigation only. References describe the current, already modified working tree.

## 1. Executive summary

The leading explanation is **large finite indirect samples combined with finite-run accumulation or a held reconstructed image**. The persistence mechanism is directly supported by source; the specific high-energy source is not established. Secondary point-light illumination, including narrow glossy responses and proximity attenuation, is a stronger initial lead than a demonstrated PDF defect. This review did not identify a proven incorrect estimator weight.

Without effective DLSS or NRD, both Off and Engine stop tracing at the configured sample target. Off presents the progressive mean. Engine normally reconstructs separate per-frame diffuse/specular signals, then holds its final reconstructed output when tracing stops. These are different histories sharing a stopping policy. NRD instead forces fresh samples every frame and denoises frame-only extractions.

The depth experiment strongly implicates secondary-surface evaluation or cache contributions first reachable there. Camera movement invalidates progressive accumulation and starts another sampling run. NRD can suppress outliers without correcting their source. DLSS is not a necessary origin because the user sees the artifacts with it disabled.

Exactly one next experiment is specified in section 10: observe the accumulation counter and speckles through a controlled restart using the existing sample-target control. No production code, shader, setting, scene, denoiser, build, staging or commit was changed. No renderer launch or GPU capture was performed. User observations are accepted as reported; the screenshot itself was not available for independent pixel inspection.

## 2. Verified indirect-lighting pipeline

Shader paths in this report are relative to `assets/Shaders/`; C++ paths are explicit. Line numbers refer to this working tree.

| Stage | Exact source and behavior |
|---|---|
| Primary and secondary hits | `PathTraceReference.hlsl:974–1130`, `PathTraceTracePath`: camera primaries trace geometry; raster primaries decode the G-buffer at vertex 0. Secondary position is ray origin + direction × hit distance; `PathTraceBuildSurface` evaluates the secondary material (`:1074–1078,1119–1124`). |
| Surface | `PathTraceReference.hlsl:394–457`: two-sided facing, LOD-0 material textures, base color, roughness/metallic and finite checks. `PathTraceBSDF.hlsli:228–279` validates normals/view, corrects the shading horizon, clamps roughness/metallic and establishes lobe probabilities. |
| Vertex lighting | `PathTraceReference.hlsl:1130–1156`: split ReSTIR vertex 0 loads its RTXDI result; later vertices call conventional `PathTraceDirectLightingLobes`. Radiance receives throughput × direct. |
| Continuation | `PathTraceReference.hlsl:1161–1189`: sample BSDF, terminate invalid events, multiply throughput by sample weight, retain PDF/delta state. |
| Roulette/ray | `PathTraceReference.hlsl:1191–1221`: survival=min(max throughput component,0.95); surviving full and diffuse throughput divide by survival. Nonfinite/nonpositive throughput terminates. Continuation uses geometric-normal origin offset, TMin=0 and distant TMax. |
| Miss | `PathTraceReference.hlsl:464–480,1045–1068,1108–1117`: sun/environment with MIS, except split primary non-delta misses omitted because RTXDI already estimates them. |
| Diffuse extraction | `PathTraceReference.hlsl:1152–1182`: vertex-zero direct diffuse plus later lighting carried through the primary diffuse share. All secondary direct lobes, including secondary specular highlights, can enter primary diffuse output. Misses also use diffuse throughput (`:1063`). |
| Output | `PathTraceReference.hlsl:1376–1495`: sample rejection, progressive mean, and separate frame-only diffuse/specular means plus hit distances. |
| Presentation | `src/Renderer/Renderer.cpp:6957–7107`: reconstruction can replace path output. `Passes/PathTraceReferencePass.cpp:1951–1960` copies output to scene color. Optional upscaling/presentation follows (`Renderer.cpp:10551–10754`), then tone mapping (`ToneMap.hlsl:347–357`). |

No surface-emission term is added in this path loop or BSDF. Evaluated sources here are table lights, sun/environment and optional cache reconstruction. An emissive-looking asset does not establish an additional emissive-light estimator in this implementation.

## 3. Depth 1 versus depth 2

The loop runs through `vertex <= maxPathDepth` (`PathTraceReference.hlsl:986`). A surface hit at the maximum vertex exits **before** shading (`:1070–1073,1114–1117`); the final ray exists to pair distant-light misses with light sampling.

- **Depth 1:** x0 is shaded and its BSDF sampled, but an x1 surface is not illuminated. A continuation miss can still contribute sun/environment, subject to the split RTXDI exclusion. “Direct only” fits the reported point-only test; it does not mean no continuation ray runs.
- **Depth 2:** x1 is now built and shaded. `W0 * Ldirect(x1)` first enters radiance. The x1 BSDF, its continuation/miss and render-cache substitution also become reachable.
- **Higher depths:** more surface terms, weight products, roulette opportunities and visibility events enter. Increasing speck counts fit additional opportunities for outliers, without proving erroneous weights.

Default roulette starts at 3 (`src/Renderer/RendererLightingMode.h:48`), so it does not amplify throughput arriving at x1 in the depth-2 test. The runtime setting is unknown; the UI permits start=1 (`src/Debug/DebugUI.cpp:1328–1329`). Early roulette must remain a conditional possibility.

## 4. Secondary-light sampling and throughput analysis

### Secondary lighting is separate from RTXDI reservoirs

`PathTraceDirectLightingLobes` (`PathTraceReference.hlsl:530–573`) samples sun/environment and iterates **every** table light (`:549–556`). There is no secondary RTXDI reservoir, random table-light selection distribution or light-selection PDF division. Primary reservoir evaluation is separate (`:752–832`) and the split result is consumed only at vertex 0 (`:1134–1141`).

`PathTraceShadeLightSampleLobes` (`:487–523`) evaluates f × cosine × incident light, visibility and, for positive solid-angle light PDF, power-heuristic MIS / light PDF. Table lights are delta (`PathTraceLights.hlsli:228–243`): PDF=0 and weight remains 1. This is appropriate because BSDF rays cannot hit the point emitter as geometry. There is no zero-PDF division or inappropriate area-light conversion here. Environment/sun misses have complementary MIS (`PathTraceReference.hlsl:464–480`). A missing point-light MIS competitor was not identified.

### One point light can still produce large indirect samples

The random primary continuation chooses x1 position, normal, view, material and visibility. A single fixed point source does not make those quantities deterministic across samples. `SceneLights.hlsli:26–66` uses inverse-square attenuation, range window and a 0.01 m distance floor. Its pre-BRDF bound is up to 10,000 × authored light radiance, subject to the window. This is a formula bound, **not a measured scene value**. Rare secondary hits close to the point can be large and finite.

Secondary GGX can have a narrow directional peak (`PathTraceBSDF.hlsli:291–322`); perceptual roughness is floored at 0.045 for non-delta GGX (`:31–35,108–112`). That avoids zero-width singularity but does not bound directional radiance to a small value. Sampling x1 geometry from x0 does not importance-sample every x1-to-point glossy alignment. A primary diffuse pixel can receive such a secondary highlight through diffuse extraction.

Near shadow boundaries, binary visibility discontinuities can increase variance; weak direct lighting also makes indirect light more prominent. The concentration near the box does not distinguish legitimate variance, self-intersection/leaks or a BRDF defect. The point itself has no randomly sampled emitter radius here.

### BSDF/PDF checks

`PathTraceBSDF.hlsli:401–411` forms a full VNDF/cosine mixture density. `PathTraceSampleBSDFLobes:459–516` uses that density and returns f×cos/pdf; the delta branch returns Fresnel / selection probability. When both strategies have positive estimates, selection probability lies in [0.1,0.9] (`:260–279`). There is no second selected-lobe-probability division for non-delta samples. For Lambertian cosine sampling the cosine cancels analytically; grazing directions alone do not demonstrate runaway diffuse weighting.

PDF must be positive/finite and weight finite; arbitrarily small positive PDFs and large finite weights remain admitted. Matching sampling/evaluation formulas argue against a simple duplicated/missing probability factor, but do not prove all numerical/material cases correct. Diffuse/multiple-scatter denominators have guards (`:160–185,218–224`). Secondary base color is lower-bounded, not upper-bounded (`PathTraceReference.hlsl:425`); the call site does not comprehensively validate IOR/conductor inputs or LUT contents. Runtime materials are unknown.

Visibility uses geometric-normal offsets (`PathTraceReference.hlsl:320–364`). No bad ray or intersection was captured. R.9G's packed-normal replay asymmetry belongs to confidence probes, not ordinary secondary illumination; it cannot by itself explain Off-mode bright samples.

## 5. Accumulation and camera-reset analysis

| Required question | Source-verified answer |
|---|---|
| 1. Progressive accumulation with all denoisers off? | **Yes.** Off disables Engine extraction/reconstruction (`Renderer.cpp:6193–6211`), not progressive accumulation. Batch selection is independent of Off versus Engine (`:6645–6680`). |
| 2. Which buffer? | `PathTraceReferencePass::m_accumulationTexture`, RGBA32F, named “Path Trace Accumulation” (`src/Renderer/Passes/PathTraceReferencePass.cpp:1114–1123`). Shader u0 is `accumulationTexture` (`PathTraceReference.hlsl:177`). A separate output texture receives displayable radiance and reconstruction writes. |
| 3. What resets it? | Pending renderer reset or accumulation-key mismatch sets sample count=0 (`Renderer.cpp:6623–6643`). Key fields include view, unjittered projection, primary mode, extent, all PT settings, sun/light table, TLAS address/recording identity, material revision, scene lifetime and environment descriptors/weight (`:5929–5951,6593–6610`). Resource/resize/mode reset sites include `:4024,4868,4949,6138,6224`; failed rendering requests reset (`:8927–8935`). |
| 4. Camera movement? | **Yes:** bitwise matrix comparison detects any representable view/projection change (`Renderer.cpp:5932–5934`). Jitter-only projection changes are intentionally excluded (`:6591–6598`). No reprojection of the progressive mean occurs. |
| 5. Can one sample persist? | **Yes:** one outlier E contributes E/N. It dilutes as N grows, potentially slowly, and remains unchanged indefinitely once tracing stops, until reset/output replacement. |
| 6. Maximum/history saturation? | Non-DLSS/non-NRD batch=min(samplesPerFrame,remaining), then zero at target (`Renderer.cpp:6645–6658`). Default target 65,536 (`RendererLightingMode.h:51`); UI maximum 65,536 (`DebugUI.cpp:1330`); validation allows up to 2^24 (`RendererLightingMode.h:123–124`). This is stopping, not a sliding-window update. DLSS/NRD override stopping and continue advancing the sample count. |
| 7. Engine preserves same accumulation? | Progressive storage still updates, but regular Engine presentation reconstructs **frame-only** diffuse/specular with separate histories (`Renderer.cpp:6957–6971,7181–7205`). At target those stages also stop; the last reconstructed output is held (`:6678,7095–7107`). Same scheduling, different displayed history. |
| 8. NRD difference? | REBLUR/RELAX/Hybrid force fresh samples every frame (`Renderer.cpp:6648–6656`). Successful NRD skips Engine reconstruction (`:6957`) and RELAX resolves into output (`:7038–7042`). The progressive mean is not RELAX's regular surface input. |

The shader computes `(sumNew + N*oldMean)/(N+s)` (`PathTraceReference.hlsl:1432–1442`). Ordinary resets do not physically clear the accumulation texture: count=0 omits the previous-buffer read and overwrites each pixel. This is logical invalidation, not blending stale memory. Rejected samples become zero and still count (`:1395–1405`), rather than being resampled or removed from the denominator. Successful render advances CPU count by pending samples (`Renderer.cpp:8939`); failure requests reset (`:8927–8935`). A live sample-counter fault was not demonstrated.

The “Reset PT denoiser history” button (`DebugUI.cpp:1394–1412`) invalidates reconstruction histories (`Renderer.cpp:6492–6499`), **not progressive accumulation**. At an already reached target it need not cause new path samples. It is not a suitable progressive-reset experiment.

## 6. SHARC participation

The user's configuration does not establish runtime activation. Actual dispatch gates require a usable cache, eligible raster primary, compatible extraction/sparse training and successful learning/query binding (`Renderer.cpp:6850–6879`). Active status depends on actual render participation (`:6501–6519,7001–7007`), not just an enabled checkbox.

Relevant render wrappers include `PathTraceReferenceRadianceCacheRender.hlsl` and its raw-distance variant, which use the shared path loop/query hooks. Sparse training entry `PathTraceRadianceCacheTrainingRayGeneration` (`PathTraceRadianceCacheTraining.hlsli:8–30`) traces training samples without directly writing the image.

Render substitution occurs at x1's Continue hook (`PathTraceRadianceCacheQuery.hlsli:584–592`). `PathTraceRadianceCacheSubstitute:390–507` requires raster/split-indirect primary, nonzero query mode, non-delta event, eligible material (non-delta surface, model<=1, metallic<=0.1), accepted binding, published entry and sample threshold; optional compile-time admission gates further restrict it. Reconstructed cache radiance must be finite and normally has negatives floored to zero. `radiance += throughput*incident` and `extraction.diffuse += diffuseThroughput*incident` (`:503–507`) inject the suffix before terminating it. No global finite-positive magnitude bound is imposed there.

Depth 2 can reach this x1 hook; the cached suffix may represent paths beyond uncached depth-2 surface truncation. Thus cache participation could complicate depth comparisons. Spatial cache error could be stable, but activation, query mode and values are unknown. `PathTraceSharcDiagnosticConfig.hlsli:8` defaults its display selector to zero; that does not prove SHARC is off or that the live binary matches source. Disabling SHARC as a correction is unsupported.

## 7. Numerical safeguards

| Risk | Existing protection and limitation |
|---|---|
| Normals/directions | Closest-hit checks squared lengths and normalizes/falls back (`PathTraceReference.hlsl:1723–1757,1789–1801`). Finalize rejects nonfinite normals/view and invalid hemispheres (`PathTraceBSDF.hlsli:228–250`). Sampling normalizes directions and rejects directions below either hemisphere (`:414–453,498–501`). Finite malformed geometry is not exhaustively excluded. |
| PDF/weight | Positive finite non-delta PDF and finite sample weight (`PathTraceBSDF.hlsli:504–515`); delta probability checked (`:478–485`). No minimum valid-positive PDF or maximum finite weight. |
| Throughput | Positive finite check after continuation/roulette (`PathTraceReference.hlsl:1212–1215`); survival positivity and 0.95 cap. No finite-magnitude cap. |
| NaN/Inf/negative radiance | Total path sample is zeroed/counted if nonfinite or negative (`PathTraceReference.hlsl:1395–1401`); nonfinite extracted diffuse is zeroed (`:1409`). These checks cover complete depth-2+ samples. Merely bright finite positive samples pass. |
| Singular attenuation/BRDF | Point-distance floor (`SceneLights.hlsli:28,55`), GGX roughness floor/stable D/denominator guards and half-vector PDF check (`PathTraceBSDF.hlsli:291–322,378–398`). These are not variance bounds. |
| Accumulated magnitude | RGBA32F progressive mean; output limited to 65,504 and extraction clamped to half range (`PathTraceReference.hlsl:1441–1442,1492–1495`). No post-sum/mean finite check. Finite individual samples do not mathematically exclude sum overflow; none was observed. |
| Materials/cache | Some inputs are clamped/validated as above; no universal upper base-color/IOR/cache-radiance check in this loop. Extreme runtime values are unverified. |

No new threshold or clamp is proposed. Existing half-range limits protect storage, not estimator correctness or firefly removal.

## 8. Denoiser-path comparison

**Off:** progressive mean followed by presentation/tone mapping, then held output at convergence. Finite outliers are averaged rather than identified/spatially removed. Off-mode artifacts rule out generated confidence or a denoiser as a necessary origin.

**Engine:** consumes frame extractions, demodulates diffuse and reprojects separate histories (`Renderer.cpp:7181–7205`; `PathTraceTemporal.hlsl:270–346`). Default diffuse/specular maximum history is 32 (`Passes/PathTraceTemporalPass.h:23–26`); specular reuse has roughness/view caps (`PathTraceTemporal.hlsl:432–448`). Optional extended diffuse mean is another history (`:456–469`; `Renderer.cpp:7233–7267`), not established active. A-trous geometry/luminance/variance weights (`PathTraceSpatialAtrous.hlsl:405–444`) can preserve a bright center. No guaranteed outlier removal follows from these weights. Shared target stopping can freeze the final reconstruction with residual noise.

**RELAX:** preparation reads frame diffuse/hit-distance signals (`NrdPathTracePrepare.hlsl:437–478`), then resolve remodulates/combines (`:523–558`). Engine config selects AREA_3X3 hit-distance reconstruction and five A-trous iterations (`Renderer.cpp:164–171`). Existing local pinned NRD 4.17.3 sources show temporal accumulation (`build/r9c/RELAX_TemporalAccumulation.cs.hlsl:595–633`), fast/slow history clamping (`build/r9d/RELAX_HistoryClamping.cs.hlsl:252–341`) and spatial filtering (`build/r9d/Relax.cpp:216–277`). Anti-firefly dispatch is conditional (`build/r9d/Relax.cpp:247`); its effective runtime state is not inferred from clean output. Continuing samples plus temporal/spatial filtering plausibly suppress outliers. Clean RELAX output does not identify which stage suppresses them or establish an upstream fix. Confidence strength 2 remains unchanged.

Effective DLSS also forces fresh path batches. Literal target-stop freezing therefore cannot explain all observations **while DLSS is actually active**. Slowly diluted progressive outliers, repeated source events or reconstruction history could still look stable. That observation limits a universal freeze explanation; it does not implicate DLSS as the original source.

## 9. Ranked hypotheses

Each rank separates possible sample generation from retention. The discriminators below identify needed evidence; section 10 selects exactly one next experiment.

### 1. Finite indirect outliers retained by finite-run accumulation/presentation

**Evidence:** x1 illumination first enters at depth 2 (`PathTraceReference.hlsl:1070–1078,1130–1186`), sharp GGX/proximity point incidence (section 4), finite-positive samples admitted (`:1395–1405`), target stopping (`Renderer.cpp:6645–6680`). **Depth fit:** strong; deeper paths add outlier opportunities. **Stationary/camera fit:** E/N can dilute slowly and freezes at target; camera resets the run. Engine can instead freeze its reconstructed output. **Against:** no correlation with the live counter; default target can take many frames; effective DLSS continues tracing. **Unknown:** actual large sample, material/light distances, target reached and presented buffer. **Smallest discriminator:** observe a controlled restart through increasing N and convergence. **Correction scope:** identify the term first; legitimate variance suggests targeted sampling/convergence-policy work, while a wrong finite contribution needs a local estimator fix. Persistence does not justify arbitrary global clamping.

### 2. Secondary visibility, normal, material or weighting defect

**Evidence:** new secondary hit/material/light evaluation at depth 2; binary visibility and origin offsets (`PathTraceReference.hlsl:320–364`), finite-input gaps (section 7). **Depth fit:** strong. **Stationary/camera fit:** a bad finite sample can be retained; view-dependent hits change with camera movement. **Against:** matched mixture PDF, delta handling, normalization, hemisphere checks and origin offsets already exist; no erroneous algebra or offending ray demonstrated. **Unknown:** per-path values, actual intersections/materials and repeatability in fresh samples. **Smallest discriminator:** inspect a speck's raw x1 position, light distance, visibility, f, cosine, PDF and throughput. **Correction scope:** repair the demonstrated local estimator/material/normal/visibility error and validate that case; no denoiser retuning.

### 3. SHARC reconstruction/cache contribution

**Evidence:** x1 substitution and diffuse injection (`PathTraceRadianceCacheQuery.hlsli:390–507,584–592`), admitted finite-positive cached values. **Depth fit:** reachable at depth 2. **Stationary/camera fit:** spatial cache plus retained history can be stable; camera changes directions/keys and progressive history. **Against:** runtime activation unknown; cache errors can be broader than isolated pixels; uncached estimator already admits variance. **Unknown:** active status, per-pixel substitution and cache RGB. **Smallest discriminator:** correlate an affected sample with substitution evidence, using a per-pixel cache-term diagnostic only if needed. **Correction scope:** cache reconstruction/admission/publication only if implicated, not speculative disabling.

### 4. Repeated samples or counter/history fault during active tracing

**Evidence:** deterministic pixel/sample/run-seed RNG (`PathTraceReference.hlsl:190–215,1378`); identical inputs repeat. **Depth fit:** requires an indirect outlier as an additional source. **Stationary/camera fit:** repeated indices could pin speckles; changed view changes hits. **Against:** source advances count on successful render (`Renderer.cpp:8939`); fixed seed does not mean fixed sample index. Held output needs no bug. **Unknown:** live count progression, submission failures and overrides. **Smallest discriminator:** counter progression during section 10; submitted-index telemetry only if needed afterward. **Correction scope:** dispatch/sample bookkeeping if a fault is demonstrated, not an unsupported RNG replacement.

### 5. Presentation/DLSS or denoiser-specific origin

**Evidence:** downstream reconstruction and tone mapping can alter highlight visibility; Engine and NRD use different histories/filtering. **Depth fit:** may expose indirect contrast but does not explain its upstream origin. **Stationary/camera fit:** temporal presentation can retain/sharpen highlights. **Against:** all-denoisers-off and DLSS-off observations rule these out as necessary causes. R.9G's contact speckles visible only after denoising are a different observation. **Unknown:** pre-presentation values/effective display path. **Smallest discriminator:** compare pre-presentation and displayed values after source/retention behavior is established. **Correction scope:** downstream reconstruction only if it creates or improperly retains a feature absent from input.

## 10. Recommended next experiment — one controlled restart through convergence

Use the existing scene, lighting and camera at **depth 2**, with the already tested **PT denoiser Off and DLSS disabled** configuration. Preserve all NRD confidence selectors, including strength 2. Do not change SHARC, jitter, roulette, exposure, sampling rate or scene content. Confirm status shows active path tracing.

1. Watch “Accumulated N / target samples per pixel” (`DebugUI.cpp:1446–1447`) and several identifiable speckles. Note whether N advances or has reached target and whether the pixels evolve.
2. Without moving the camera, change **PT sample target** once to another valid, preferably larger target (UI maximum 65,536). All PT settings belong to the accumulation key (`Renderer.cpp:5947–5949,6623–6643`), so this **restarts**, rather than extends, the mean. Choose a target above samples-per-frame so several batches are observable. Follow the counter and the same ROI from restart to target.
3. Record whether old speckles clear, new speckles vary/dilute as N rises, and the image becomes fixed when N stops. Do not substitute “Reset PT denoiser history.” No setting was changed by this audit.

**Discrimination:** clearing on restart, evolving during increasing N, then freezing with N supports source outliers plus finite-run retention. Speckles stable from the earliest fresh batches while N advances weaken a single retained-sample account and strengthen recurring/view-conditioned source or cache behavior. Failure of the counter to restart/advance calls for checking scheduling/effective runtime state. Recurring coordinates alone do not prove a bug: geometry and an explicit fixed seed can reproduce them. CPU status is not a numerical GPU sample trace.

This one experiment resolves the leading persistence uncertainty interactively without production changes or a capture tool. It cannot by itself distinguish legitimate variance from a bad finite estimator term. If needed afterward, the smallest diagnostic addition would be a **read-only selected-pixel sample trace** containing submitted sample index/run seed, sample radiance and progressive old/new mean, primary diffuse throughput, x1 position/normal/material, direct-light f/cosine/incidence/visibility/PDF and cache term/substitution flag. This is specified only, not implemented or recommended ahead of the existing-control experiment.

## 11. Remaining uncertainties and verification limits

Unknown effective state includes primary mode, deployed shader match, sample target/rate/count, DLSS, jitter/extended history, roulette, SHARC, material parameters and point placement. No live rendering, numerical raw samples or accessible screenshot pixels were inspected. Source establishes configured behavior, not that the user's process was at its target. No proven estimator defect, GPU counter fault or visual correction is claimed.

RNG state is local per sample. PCG emits [0,1) with 24-bit resolution (`PathTraceReference.hlsl:209–215`), seeded by pixel, global sample index and run seed. Reset normally chooses a run seed from submitted-frame sequence; an explicit override exists (`Renderer.cpp:6631–6641`; `Passes/PathTraceReferencePass.h:125–132`). Stationary active sampling is deterministic but ordinarily changes every batch. Camera primaries consume random sample jitter (`PathTraceReference.hlsl:1232`); raster modes consume/discard corresponding draws and use raster coverage (`:1246–1279`). Renderer Halton jitter has a separate phase (`Renderer.cpp:6450–6464`) excluded from progressive reset identity. These facts predict changing raw samples during active tracing, not automatic stationary repeated samples.

Reviewed R.9D, R.9F and R.9G reports alongside current source; their confidence/contact-shadow findings were not treated as proof of this artifact's cause. Existing local pinned NRD sources supplied internal filtering evidence. Validation was source/report consistency review; no build or rendering reproduction was performed. This report is the sole new artifact. Existing working-tree changes were preserved. Nothing was staged or committed. Investigation stops here.

