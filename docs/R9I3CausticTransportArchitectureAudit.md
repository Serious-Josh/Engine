# R.9I.3 — caustic transport architecture audit

2026-10-27. Read-only architecture investigation. No renderer or shader source was modified and Engine was not launched.

Scratch models live in the ignored folder `build/r9i3/` (`photon_budget.py`/`.txt`) and reuse `build/r9i2/floor_caustic.py`.

## 1. Executive summary

**Target path:** Point Light (5) → glossy metal Cube (19) → red diffuse floor → camera. In light-first notation this is **L G D E**. It is now supported by runtime evidence: the patch follows the cube.

**Recommended architecture:** a **reflector-targeted light-tracing pass** with image-plane splatting. Light paths are emitted from point lights toward known glossy and specular reflector instances, reflected by GGX VNDF sampling (or by delta reflection), and connected from the diffuse receiver hit to the camera. The pass is combined with the existing camera-path tracer through an **exact, disjoint path-space partition**, not through MIS.

- The camera-path tracer stops estimating exactly the L G D E paths whose reflector and receiver satisfy a fixed material-class test.
- The light tracer estimates exactly those paths.
- Both use the same class predicate, so no path is counted twice or dropped.

**Why light tracing is the right sampler:**

1. **It samples the dominant factors directly.** It follows light emission and the reflector's GGX lobe, the two factors the camera path cannot target.
2. **It is unbiased.** There is no density-estimation radius, so edges and falloff are preserved exactly in expectation.
3. **It handles the perfect-mirror case.** Cube (20)'s point-light caustic, which Engine cannot render today at any sample count, comes out for free.

**Expected payoff (models, not GPU measurements):**

- The camera-path estimator has per-sample relative standard deviation of **126–163** on the patch at roughness 0.1 (R.9I.2).
- Even isotropic emission at **1 M light paths per frame** lands about 0.27 receiver splats per patch pixel per frame.
- Targeting reflectors raises the useful fraction from about **0.7 % to near 100 %** of emitted paths. That gives tens of splats per patch pixel per frame and a per-frame relative error of about 0.15–0.3.
- On the patch this is **4–6 orders of magnitude** of variance reduction at a cost of roughly 3 M extra rays per frame (about +30 % over the current depth-2 Raster Primary frame; this is an assumption, see §6).
- Two independent models of the patch radiance agree, which validates the energy target:
  - camera-path model: 0.1029 (r = 0.1) and 0.0758 (r = 0.2);
  - photon density model: 0.099–0.101 and 0.075–0.078.

**Strongest alternative:** stochastic progressive photon mapping (SPPM) restricted to caustic photons, gathered at the G-buffer vertex. It becomes preferable if NRD and trace-every-frame modes are the primary target, or if many lights and receivers make camera splatting impractical. Its price is a radius-dependent blur bias at the trapezoid's edges.

**Rejected as primary solutions:** clamping, roughening, suppression and blurring (by requirement); full BDPT and VCM (scope); MNEE (geometry generality); diffuse ReSTIR GI (wrong estimator class).

## 2. Engine architecture map (source-grounded)

| Component | Location | Relevance |
|---|---|---|
| Path-trace pass dispatch, one `DispatchRays(width, height)` per image pipeline; root parameters 0 TLAS, 1–3 accumulation/output/statistics UAV tables, 4 path constants, 5 light constants, 6 cache constants | `src/Renderer/Passes/PathTraceReferencePass.cpp:1185–1266` (`Execute`); `:1837–1949` (`ExecuteReSTIRDISplit`, two dispatches at `:1918`/`:1941`); training `:1268–1295` | A light-tracing dispatch fits the existing pattern: a separate pipeline in the same pass with the same root parameters. |
| Frame recording and ordering (constants, ReSTIR DI preparation, SHARC preparation, then the path dispatch, diagnostics copy, NRD) | `src/Renderer/Renderer.cpp:6576` (`RecordPathTraceReference`), dispatch `:6906–6916` | The light pass would be recorded **before** the camera dispatch in the same command list (UAV barrier between), so the camera raygen can fold the splats into the progressive mean. |
| Camera path loop and depth semantics | `assets/Shaders/PathTraceReference.hlsl:990–1240` (`PathTraceTracePath`); NEE `:1158–1176`; BSDF sampling `:1192`, throughput `:1217`, roulette `:1234` | The partition test goes in the vertex-1 NEE (§5.2). |
| Point-light NEE and shading | `PathTraceReference.hlsl:542–592` (`PathTraceDirectLightingLobes`, all lights, deterministic), `:498–535` (`PathTraceShadeLightSampleLobes`); `PathTraceLights.hlsli:228–242` (`PathTraceSampleTableLight`, pdf 0 = delta); `SceneLights.hlsli:34–67` (intensity × window² / d²) | The light tracer must emit with exactly this intensity, window and range so energy matches. |
| BSDF evaluation, sampling, PDF (one shared definition) | `assets/Shaders/PathTraceBSDF.hlsli:228–280` (finalize; delta when r ≤ 0.001 at `:251`), `:302–353` (evaluate), `:401` (`PathTracePdf`), `:414` (`PathTraceSampleVisibleNormal`), `:459–526` (`PathTraceSampleBSDFLobes`) | Reusable unchanged at the reflector (VNDF sampling and weight) and receiver (evaluation toward the camera). The BSDF is documented as reciprocal except for the horizon correction (`PathTraceBSDF.hlsli:22`). |
| Surface construction from a hit | `PathTraceReference.hlsl:401–468` (`PathTraceBuildSurface`, material resolution `:447–460`); closest hit `:1749` | Reusable for light-path hits. The surface needs a "view" direction; for light paths it is the incoming light direction, which the reciprocal BSDF makes valid (§5.1). |
| Visibility rays | `PathTraceReference.hlsl:343` (`PathTraceVisible`) | Reusable for the receiver-to-camera connection. |
| Camera model (inverse view-projection only; ray from pixel and jitter) | `PathTraceReference.hlsl:594–610` (`PathTraceCameraRay`); constants `cameraPosition`, `inverseCameraRelativeViewProjection` | Splatting needs the **forward** view-projection, which is **not** in the path constants today. New constant fields are needed. |
| Raster first vertex (G-buffer) for the Raster Primary modes | `PathTraceReference.hlsl` (`PathTraceDecodeRasterFirstVertex`, `rasterFirstVertex` use in `PathTraceRunRayGeneration` `:1365`) | Vertex 0 is the **pixel-centre** surface, so Raster Primary estimates point radiance per pixel (§5.4). |
| Progressive accumulation | `PathTraceReference.hlsl:1475–1486` | The splat estimate must enter the same per-frame mean (§8). |
| RNG (PCG, keyed by pixel, sample index, seed) | `PathTraceReference.hlsl:213–218` | The light pass needs its own stream, keyed by light-path index, frame and seed, so camera RNG sequences stay unchanged. |
| Instance data (vertex/index buffers, material index per instance and slot) | `assets/Shaders/RayTracingInstanceData.hlsli:4–19`; `src/Renderer/RayTracing/RayTracingSceneResources.h:49–163` (TLAS, instance-data descriptor) | The TLAS is reused as is. The reflector list (world bounds of instances whose material is glossy or specular) needs **new** CPU code in or beside `BuildInstanceData` (`:328`). |
| SHARC spatial hash, 64-bit fixed-point atomics, record buffers | `assets/Shaders/RadianceCacheD6.hlsli:298–410` (keys, buckets, tags), `:469–480` (`RadianceCacheD6AtomicAdd64`); `src/Renderer/RadianceCache/RadianceCacheD6.h` | **Infrastructure** to reuse: the fixed-point 64-bit atomic add pattern (for splat accumulation), and the hash grid if photon mapping is chosen. **The estimator itself is not reusable**: SHARC caches incident radiance at path vertices. |
| ReSTIR DI split (vertex-0 direct from RTXDI; vertex-1 direct via the conventional loop) | `PathTraceReference.hlsl:1167` (vertex 0), `:1176` (others); `PathTraceReferencePass.cpp:1837` | Unaffected: the partition only touches vertex-1 point-light NEE. |
| NRD routing (prepare passes read extraction diffuse/specular signals) | `Renderer.cpp:7289–7346` (`MakeNrdPathTracePrepareConstants` and prepare), `:7360`, `:7469`, `:7848` | The caustic splat is a diffuse-receiver signal. A later NRD integration would add it to the diffuse extraction (§6). Not part of the prototype. |
| Diagnostics (R.9I.1/1D) | `src/Renderer/PathTraceSampleDiagnostic.h`, `assets/Shaders/PathTraceSampleDiagnostic.hlsli` | Useful for validation: with the partition on, captured L G D E fireflies should disappear from the camera estimator. |

**Not found in Engine:** light-path or photon dispatches, image-plane splatting, a forward camera matrix in the path constants, reflector or caustic-caster lists, BDPT/VCM infrastructure, or float atomics on UAV buffers. Integer and fixed-point atomic patterns do exist.

## 3. Candidate algorithms

### A. Light tracing with camera connections

**Path generation** (light-first vertices y₀ = point light, y₁ = reflector, y₂ = receiver):

1. Choose a light (one active here; in general pick by power) and a direction ω₀ with density p(ω₀) (solid angle). §3.D's targeted emission makes p concentrate on reflector bounds.
2. Trace to y₁. **Keep only if y₁ is in the reflector class R** (the shared predicate in §5.2); otherwise the path contributes 0 to this estimator.
3. At y₁, sample ω₁ with the reflector's own sampler (GGX VNDF, or the delta mirror), using `PathTraceSampleBSDFLobes` with the incoming light direction as "view". The throughput weight is f·cos/p (or F/P for delta).
4. Trace to y₂. **Keep only if y₂ is in the receiver class D.**
5. Connect y₂ to the camera: compute the image position, trace a visibility ray from y₂ to the camera position, and evaluate the receiver BSDF f₂(ω₁ → toward camera).

**Contribution and density accounting** (pinhole, image-plane area A at unit distance, P pixels, N light paths per frame):

```
β₁  = I · window(r₀₁)² / p(ω₀)                 (point light: intensity over direction density; the 1/r₀₁²
                                                of the light's incidence cancels with the solid-angle measure)
β₂  = β₁ · f₁(−ω₀, ω₁)·|cos θ₁| / p₁(ω₁)        (= β₁ · VNDF weight)
C_j += β₂ · f₂(−ω₁, ω_c) · |cos θ₂| / d_c² · W(ω_c) · V(y₂, cam) · [pixel(y₂) = j]
W(ω_c) = P / (A · cos³ θ_c)                     (pinhole importance × the camera-side cos, per-pixel box filter)
```

`d_c` is the distance from y₂ to the camera, θ_c is the angle to the view axis, and θ₂ is the angle at the receiver normal. The `1/d_c²` and the camera `cos θ_c` together turn solid angle at the camera into image-plane area. Combined with `W`, this gives the standard unbiased pinhole light-tracing estimator (Veach 1997 §10; PBRT-v3 §16.1). To match the camera estimator's energy, the range window and the `SCENE_LIGHT_MINIMUM_DISTANCE` clamp of `EvaluateSceneLightIncidence` must be applied at **y₁** exactly as written there. The clamp replaces the cancelled 1/r² only when r₀₁ is below the minimum distance, so it contributes a factor r₀₁² / max(r₀₁², r_min²).

**Practical requirements:**

- **Camera projection and splatting:** needed. The forward view-projection is a new constant.
- **Visibility to the camera:** a shadow ray from y₂ to the camera origin, reusing `PathTraceVisible`. Comparing against the G-buffer depth is an alternative, but it is approximate at silhouettes. Prefer the ray.
- **Accumulation of splats:** atomic adds into a per-pixel RGB buffer. With no float atomics, use 64-bit fixed point (the `RadianceCacheD6AtomicAdd64` pattern) with a fixed scale and saturation detection. No rasterization is needed.
- **Pixel measurement:** light tracing estimates the **box-filtered** pixel radiance. Raster Primary evaluates the pixel-**centre** surface. Inside the patch they agree. At the trapezoid edges and silhouettes the light-traced part is an anti-aliased version of the same signal. This mismatch is why §5.3 uses a partition rather than MIS. Path Traced Reference mode jitters camera rays (`PathTraceCameraRay` with jitter, `:594`), which is the same box-filter measurement and therefore the exact validation mode.

### B. Photon mapping family

| Method | Behaviour | Bias and convergence | GPU fit |
|---|---|---|---|
| Classic caustic photon map | Store photons on receivers after ≥1 G/S bounce; gather k-nearest or within radius r at x₀ | Biased (kernel blur, boundary bias); fixed r is inconsistent | Needs a spatial structure (the SHARC hash-grid pattern) |
| PPM (Hachisuka 2008) | Per-pixel radius and flux statistics, radius shrinks over passes | Consistent | Per-pixel stats (radius, N, τ) about 32 B/pixel ≈ 100 MB at 3.05 MP |
| **SPPM** | Like PPM with stochastic visible points (jitter or raster), per-pixel radius statistics | **Consistent**; bias ∝ r², which shrinks as passes accumulate | The best photon variant for progressive Off mode. Fixed-r in trace-every-frame modes leaves a fixed blur. |
| VCM/UPS (Georgiev 2012) | MIS between light-tracing connections and merging | Consistent, robust | Needs BDPT-style subpath storage. Too broad for Engine now. |

- **Budget in this scene** (`photon_budget.txt`): with isotropic emission, a 1 M-photon frame deposits about 5.5 / 22.5 / 141 photons within r = 1 / 2 / 5 cm of the patch centre. The density estimate matches the camera-path mean.
- **Edge bias:** with r = 2 cm the trapezoid's edge is convolved with a 2 cm kernel (several pixels at this camera distance). SPPM shrinks it over time.
- **Strength:** gathering happens at the G-buffer x₀ (the point measurement Raster Primary already uses), with no camera splatting and naturally per-pixel output.

### C. Limited bidirectional path tracing

For L G D E at depth 2, the only useful bidirectional strategies are:

- **(s = 1, t = 3)** — the current camera path with NEE at x₁;
- **(s = 3, t = 1)** — light tracing to the camera;
- **(s = 2, t = 2)** — light vertex on the reflector connected to the camera-visible x₀. The connection evaluates both BSDFs, including the reflector's glossy lobe toward x₀. This samples neither factor well, so it is a poor strategy for a narrow lobe.

So "limited BDPT" for this path reduces to **A plus the current tracer**. Full BDPT with MIS adds subpath storage and many connections, which is out of scope. A delta reflector (Cube (20)) supports only (s = 3, t = 1).

### D. Specialized caustic sampling

| Technique | Improves discovery? | Notes |
|---|---|---|
| **Reflector-targeted emission** (sample directions in the cones subtending glossy/specular instance bounds; a mixture over reflectors) | **Yes. The key efficiency lever for A or B.** It raises the useful fraction from 0.7 % to near 100 % | Correct only if the density accounts for overlapping cones (mixture PDF over every cone containing ω₀), the cones fully contain each reflector, and a light inside a bounding sphere falls back to sphere sampling |
| Receiver-conditioned reflector sampling (at x₀, sample a point on the reflector near the specular image of the light) | Yes for planar or analytic reflectors | Needs per-reflector geometry queries. Generalises poorly to meshes |
| MNEE / specular manifold sampling | Yes for S chains; extensible to glossy | Newton solves on mesh manifolds, robustness issues, high complexity |
| Reservoir reuse (ReSTIR GI / PT) | **No: it reuses rather than discovers** | Once a rare sample is found, reuse spreads it, but the shift must re-evaluate the glossy BRDF at x₁ toward the new x₀ (a hybrid/reconnection shift with Jacobian). Engine's diffuse-focused ReSTIR GI direction stores x₁ outgoing radiance and would be biased for a glossy x₁. |

### E. Existing systems

| System | Reusable infrastructure | Reusable estimator? |
|---|---|---|
| ReSTIR DI | Light records, RTXDI bridge | No. It is vertex-0 direct only; untouched by the partition. |
| SHARC (D6) | Hash grid, keys, 64-bit fixed-point atomics, record buffers, epoch/version handling | No. With SHARC on, cache substitution at x₁ would also carry reflected light, so a partition must also exclude caustic classes from the cache's contribution. **SHARC is unsupported in the prototype.** |
| NRD | Diffuse signal routing | A denoiser, not a sampler. After the heavy tail is removed, NRD diffuse would denoise the caustic well. |
| Planned diffuse ReSTIR GI | Reservoir and screen buffers | Not for G vertices (see D). |

## 4. Correctness requirements

### 4.1 Densities and BSDF consistency

- **Emission:** a point light is isotropic. A targeted density is p(ω₀) = Σ_k w_k·U_cone_k(ω₀), a mixture over reflector cones, with uniform sphere sampling as the fallback.
- **Reflector:** VNDF sampling and its weight F·G₂/G₁ come from `PathTraceSampleBSDFLobes` (with the mixture selection between specular and cosine lobes, exactly as for camera paths). A delta reflector returns weight F/P_delta.
- **Receiver:** f₂ is evaluated by `PathTraceEvaluateBSDF` with view = toward camera and light = toward the reflector.
- **Reciprocity:** the BSDF is reciprocal except for the horizon correction (`CorrectShadingNormalViewHorizon`, n·v < 0.02). Using the incoming light direction as "view" at the reflector changes which side is corrected. This causes no bias in the light-tracing estimator itself (it is a different but consistent BSDF definition), but the light-traced and camera-traced estimates of the same path differ slightly at grazing n·v < 0.02. That is an accepted, documented approximation; it does not double count.
- **Shading normals in light transport:** the adjoint BSDF with shading normals needs the Veach correction factor |ω·n_s||ω'·n_g| / (|ω·n_g||ω'·n_s|) at the receiver and reflector. Omitting it gives a small, measurable bias on normal-mapped receivers. The prototype applies it, or restricts to geometric = shading normal (true for the scene's cubes).

### 4.2 Delta and non-delta events

- **Delta reflector (Cube (20)):** light tracing handles it naturally. The camera path has zero contribution for it today (no NEE at a delta-only vertex, `PathTraceReference.hlsl:1158`), so **adding it is new energy, not double counting**.
- **Delta receiver:** not a receiver class (no camera connection through a delta, since its probability is zero).
- **The point light itself:** delta in position; emission is the only strategy at y₀.

### 4.3 Geometry and visibility

Visibility is tested on y₀→y₁ and y₁→y₂ by tracing (first hit), and on y₂→camera by a shadow ray. Points behind the camera or outside the frustum are discarded.

### 4.4 Energy and double counting: the partition

The predicate depends only on **surface properties at fixed path vertices**, never on sampling decisions:

```
caustic(path) := path is L x1 x0 E (one bounce),
                 light is a point/spot light,
                 x1 ∈ R  (reflector: model metallicRoughness with metallic = 1, or model 2 conductor, and roughness < τ_R)
                 x0 ∈ D  (receiver: not in R, roughness ≥ τ_D)
```

- **Camera estimator:** in the vertex-1 NEE, skip the point/spot-light term when (x1 ∈ R) and (x0 ∈ D). Everything else is unchanged.
- **Light estimator:** contributes only when y₁ ∈ R and y₂ ∈ D.
- **Union and intersection:** the union of the two estimators' supports is the full integrand and their intersection is empty, so the sum is unbiased. The predicate must be evaluated on identical resolved material values in both. Texture-driven metallic or roughness must be resolved the same way at the same UV (`PathTraceBuildSurface`).
- **Why not MIS:** MIS between the two would be more robust where the camera estimator is good, such as very rough reflectors. But it requires both strategies to estimate the same pixel measurement and to evaluate each other's PDFs. Under Raster Primary point sampling the light-tracer PDF of hitting a pixel centre is zero, so balance-heuristic MIS is ill-defined there. It is only feasible in Path Traced Reference (jittered box filter). The partition with a roughness threshold τ_R (for example 0.35) sends to light tracing only the reflectors where camera paths are hopeless.

### 4.5 Bias, convergence and temporal behaviour

- **Light tracing:** unbiased per frame, so the progressive average converges. No radius.
- **SPPM (alternative):** biased per pass (kernel), consistent with radius reduction. Fixed radius in non-accumulating modes means a permanent blur.
- **Temporal reuse:** none in the prototype. Each frame's light paths are independent, so moving lights and objects are handled by the existing accumulation reset. Any later temporal reuse must reproject splats; an invalid reuse would smear the caustic.
- **Moving the light or reflector:** the reflector list is rebuilt when the scene or TLAS changes (the same trigger as instance-data rebuilds).

## 5. Coexistence with the current estimator

- **Toggle off:** the camera library and accumulation are unchanged. The partition skip is a compile-time variant of the camera library, selected only when the toggle is on (per the project's shader-isolation rule: never branch both behind a runtime flag; verify DXIL identity).
- **Toggle on:** the light pass writes a per-frame splat buffer (cleared each frame). The camera raygen variant adds `splat(pixel)` to the frame's mean, as one more per-frame estimate weighted like the batch, and skips the partitioned NEE term.
- **ReSTIR DI:** untouched. **SHARC:** must be off (prototype guard). **NRD/DLSS:** unsupported in the prototype (guarded).

## 6. Performance and integration

Assumptions (labelled): 2868×1065 ≈ 3.05 MP render; Raster Primary depth 2. The camera path today costs about 3 rays per pixel (vertex-1 continuation + x₀ RTXDI visibility + x₁ shadow ray) ≈ 9 M rays per frame. Ray throughput was not measured in this phase.

| Item | Light tracing (A, recommended) | SPPM (B) | Limited BDPT/VCM (C) |
|---|---|---|---|
| Extra rays per frame | 3 per light path (to the reflector, to the receiver, camera visibility): **3 M at 1 M paths** (about +30 %) | 2 per photon, plus 0 for the gather (hash lookup): **2 M at 1 M photons** | Many, from subpath connections |
| Memory | Splat RGB 3 × 64-bit = 24 B/px ≈ **73 MB**, or 12 B/px at 32-bit fixed point ≈ 37 MB, plus a reflector buffer (< 1 KB) | Photon hash grid (for example 1–4 M entries × 32 B ≈ 32–128 MB) plus per-pixel SPPM statistics ≈ 100 MB | Subpath storage (hundreds of MB) |
| Acceleration structures | TLAS reused, no change | TLAS reused | TLAS reused |
| Dispatch and sync | 1 extra `DispatchRays` (N×1) before the camera dispatch, plus a UAV barrier, a clear (copy or compute), and a fold into the camera raygen | Photon dispatch + barrier + gather in the camera raygen + per-pixel statistics update | Several |
| Scaling with lights | Linear in the light-path budget; distribute paths by light power × reflector solid angle | Same | Worse |
| Scaling with scene | The reflector count bounds the emission mixture; fine for tens of reflectors (BVH over reflectors later) | Same, plus hash load factor | — |
| Progressive accumulation | Natural (per-frame unbiased estimate) | Needs per-pixel SPPM statistics instead of a plain mean | — |
| Denoising later | Feeds the NRD diffuse signal (demodulate by receiver albedo) | Same | — |
| Toggle isolation | Separate pipeline plus a camera variant library | Same | — |
| Interface changes | Path constants: a forward view-projection and splat-buffer descriptor (append after the R.9I.1D fields; 320 → 400+ bytes). Light-pass constants. CPU reflector list from materials. | Hash-grid binding, SPPM statistics | Large |
| Hybrid (Legacy) mode | Not applicable (the path-traced modes only) | Same | — |

## 7. Decision matrix

Scores run from 1 (worst) to 5 (best).

| Criterion | A. Targeted light tracing + partition | B. SPPM caustic photons | C. Limited BDPT/VCM | D. MNEE | Reuse-only (ReSTIR GI/PT) |
|---|---|---|---|---|---|
| Correctness and convergence | **5** (unbiased; the partition is exact) | 4 (consistent; finite-r bias) | 5 | 4 | 3 (exact only with correct glossy shifts) |
| Variance reduction on the patch | **5** | 5 | 5 | 4 | 2 (does not discover) |
| Preserves energy, edges and falloff | **5** | 3 (blurred edges until r shrinks) | 5 | 5 | 4 |
| Glossy and perfect mirror | **5 / 5** | 5 / 5 | 5 / 5 | 3 / 5 | 3 / 1 |
| Implementation complexity (5 = simplest) | **3** | 2 | 1 | 1 | 2 |
| GPU cost (5 = cheapest) | **4** | 3 | 2 | 3 | 3 |
| Integration risk (5 = lowest) | **4** (one pass, one variant) | 3 (per-pixel statistics, hash) | 1 | 2 | 2 |
| Future usefulness | 4 (all point/spot caustics; NRD-ready) | 5 (also volumetric/indirect caustics; NRD-ready) | 5 | 3 | 4 (general GI) |

**Leading candidate: A.** It needs the least new machinery, preserves the trapezoid's edges without a radius schedule, is unbiased, fixes the currently missing mirror caustic, and leaves every existing estimator untouched when disabled.

**Strongest alternative: B (SPPM).** It becomes preferable when:

- the main target is trace-every-frame NRD modes, where per-pixel splat noise is denoised anyway and fixed-radius bias is acceptable;
- many lights or receivers make camera splats too sparse per pixel;
- longer chains (L G G D E, for later depth > 2) matter, where gathering is more robust than camera connection.

## 8. Minimal prototype (design only)

**Intended paths:** exactly L x₁ x₀ E with a point or spot light, x₁ ∈ R (metallic 1 or conductor, roughness < τ_R = 0.35, delta included) and x₀ ∈ D (not R, roughness ≥ τ_D = 0.5). Depth-2 runs only.

**New GPU resources:**

1. A splat buffer: `RWByteAddressBuffer`, W×H×3×8 B, fixed-point uint64 (scale 2²⁰), cleared each frame by copying from a zero source (the R.9I.1D pattern).
2. A reflector list: a structured buffer of {world sphere centre, radius, instance ID}, built on the CPU from instances whose resolved material is in R.
3. Light-pass constants: light-path count N, seed, forward camera-relative view-projection, camera position, image size, τ_R, τ_D, reflector count, splat descriptor.

**Shaders:**

- `assets/Shaders/PathTraceCausticLightTrace.hlsl`: a new library containing one raygen (thread = light path) plus the existing closest-hit and miss shaders. It includes `PathTraceBSDF.hlsli`, `PathTraceLights.hlsli` and the shared surface builder, so the surface builder must be factored into an include **without changing the camera library's DXIL** (verify).
- A camera variant library `PathTraceReferenceCausticPartition.hlsl` that defines `PATH_TRACE_CAUSTIC_PARTITION` around:
  - (a) the vertex-1 point/spot NEE skip under the shared predicate (in `PathTraceDirectLightingLobes`, passing the x₀ class);
  - (b) the read of `splat(pixel)` added to the batch sum (`PathTraceRunRayGeneration`, before the mean at `:1475`).

**C++:**

- `PathTraceReferencePass`: `EnsureCausticPipelines`, `ExecuteCausticLightTrace` (dispatch N×1, same root signature as `Execute`, `:1185–1266`).
- `Renderer::RecordPathTraceReference`: when the toggle is on and the guards pass (no SHARC, denoiser Off, no DLSS, depth 2, a supported primary), record clear → light trace → UAV barrier → camera dispatch with the variant library. Otherwise record exactly as today.
- `DebugUI`: an experimental toggle, N (default 1 M), τ_R, τ_D, and reflector count and status.

**Correctness invariants:**

1. Toggle off → identical libraries, constants (except the appended fields) and images (DXIL and image hash check).
2. One shared predicate source file used by both estimators.
3. The emission mixture PDF is evaluated over **all** cones containing the direction.
4. Splat fixed-point overflow is detected and counted.
5. Light-pass RNG never touches the camera RNG streams.
6. No splat for points behind the camera or outside the image; a visibility ray for every splat.

**Intentionally unsupported:** SHARC on; NRD/DLSS; depth > 2 (L G D … chains); area/directional/sun/environment caustics; transmissive or refractive media; MIS; temporal reuse; normal-mapped receivers unless the shading-normal correction is applied.

**GPU cost measurement:** use a GPU timestamp region around the light pass (the `GpuTimestampProfiler` regions used at `Renderer.cpp:6900–6916`), on a warmed-up stationary replay (per the project's known readback/clock caveat). Report rays per frame, ms per frame, and relative MSE per ms against the baseline.

## 9. Validation plan

**Scene:** `Demo Scene.scene`; Point Light (5) only; Cube (19) TestMaterial at roughness 0.1 and then 0.2; stationary camera; depth 2; denoiser/DLSS/SHARC off. Use **Path Traced Reference** mode for exact-measurement validation (jittered box-filter primary), then Raster Primary and ReSTIR DI for the visual target.

1. **Energy (the primary test, avoiding millions-of-spp references):**
   - Define a fixed image-space region mask over the trapezoid (about 10⁴ pixels) and a control mask outside it.
   - Compare the region-mean radiance of (a) the baseline camera estimator at high spp (for example 4,096): its region-mean relative error is about 126/√(4,096 × 10⁴) ≈ 2 %; (b) the new estimator.
   - They must agree within combined error bars (target 2–3 %). Repeat at r = 0.2.
2. **Partition / no double counting:** a debug-only configuration with the light pass on and the camera skip **off** must give about 2× the caustic contribution in the mask. With the light pass off and the skip on, the caustic must be about 0 in the mask. Production must match (light only) + (camera with skip) = baseline.
3. **Independent analytic cross-check:** the scratch models predict a patch-centre radiance of about 0.10 × albedo × direct-scale at r = 0.1 (0.0758 at r = 0.2). Pixel values at known floor points should match within the models' stated error.
4. **Spatial fidelity:** compare edge profiles across the trapezoid boundary (row and column cuts) against the baseline region averages along strips. No widening is allowed beyond baseline error. Light tracing must not blur.
5. **Variance:** per-pixel sample variance across independent seeds at equal time. Report relative MSE × time; expect orders-of-magnitude improvement.
6. **Temporal stability:** stationary-camera frame-to-frame variance of the new splat estimate per pixel.
7. **No regressions:** control-mask means (direct, non-caustic indirect, glossy reflections on the metal) must be unchanged within error, and the toggle-off image must match the baseline bitwise.
8. **Response:** move Cube (19) and the light (as in the user's runtime test). The patch must follow, with the same region-mean agreement.
9. **Mirror caustic:** Cube (20) shows a new caustic only with the toggle on. Its energy is validated against an independent estimate (an analytic planar-mirror image of the light: E = F·I·cos/d² from the image point), because the baseline is zero by construction.

## 10. Open questions and limitations

- Light-path count, ray throughput and memory are estimates. No GPU measurement was made in this phase.
- The pixel-measurement mismatch (box versus pixel centre) in Raster Primary affects only edges and silhouettes, at about pixel scale. It is quantified by validation step 1 in Reference mode against Raster mode.
- The predicate thresholds τ_R and τ_D are heuristics. The partition is unbiased for any values, but the variance trade-off depends on them.
- The horizon-correction non-reciprocity (n·v < 0.02) makes light-traced and camera-traced BSDFs differ slightly at extreme grazing. Its magnitude is unmeasured.
- SHARC compatibility (excluding caustic classes from cache contributions) and NRD integration need separate design.
- The fixed-point splat scale must be chosen for the HDR range. Overflow is counted, never silent.
- Whether the user-visible NRD shadow-edge boiling shares this cause is a hypothesis, not established. The partition does not change direct shadows.
- Scene-data inferences (TestMaterial roughness at runtime, material index 3) carry over from R.9I.2.
- No tracked file was modified. Nothing was staged or committed.
