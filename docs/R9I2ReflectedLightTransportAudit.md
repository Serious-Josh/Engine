# R.9I.2 — reflected-light transport and variance-reduction audit

2026-10-27. Read-only: no rendering code, shader, material, scene, diagnostic or setting was modified, and Engine was not launched. The saved scene was read as data only.

Scratch work is in the ignored folder `build/r9i2/`:
- `scene_dump.py`/`.txt` — scene summary;
- `floor_caustic.py`/`.txt` — a Monte Carlo model of Engine's estimator for the candidate path.

## 1. Executive summary

- **The trapezoidal red floor patch is most likely a glossy reflection of the point light:** light → the gray metal face of **Cube (19)** → the red diffuse floor → camera. In Heckbert notation (light first) that is **L G D E**, where G is the glossy metal and D is the diffuse floor.
- **It is real, valuable transport.** In the model, its mean radiance is **0.65–0.73× the floor's direct lighting** at roughness 0.1. It has a geometric trapezoidal footprint and takes its red color from the floor albedo.
- **The R.9I.1D/1E fireflies are very probably the extreme tail of this same estimator, not a separate artefact.** Every captured number fits a camera → floor → Cube (19) west face → light path (§3.3).
- **The graininess is expected Monte Carlo variance of an unbiased but badly matched estimator.**
  - In camera order, the floor's cosine-weighted BSDF sample must happen to land on the small patch of the cube face where the metal's glossy lobe lines up with the light.
  - At roughness 0.1, only about 1.6–5.7% of floor samples hit the face lobe at all, and about 0.1% carry more than 10× the mean.
  - Per-sample relative standard deviation is **about 126–163**. That is about **6–10 million spp for 5% relative error** (0.6–1.2 million at roughness 0.2). At the captured 821 spp the patch's per-pixel relative error is still about **440%**.
- **No estimator or accumulation defect was found.** Denoiser, DLSS and SHARC were off and play no part.
- **Recommended direction:** a light-tracing ("light subpath to camera") technique for caustic-like L G+ D E paths, combined by MIS with the existing path tracer. It is unbiased and preserves energy, shape and edges. A photon-based (SPPM) alternative also covers the delta-mirror caustics Engine currently cannot render. The cheapest experimental option, path regularization, is biased and blurs the patch.
- **Next experiment:** move Cube (19) +0.5 m in x, with no code changes. The model predicts exactly how the trapezoid should change (§7).

## 2. Scene facts (from `Projects/DefaultProject/Scenes/Demo Scene.scene`, read only)

| Item | Saved data |
|---|---|
| Active light | **Point Light (5)**: white, intensity 2, range 10, at (−4.544, 2.25, 2.166). Every other point light and the directional light are disabled. Environment `builtin:none`. |
| Floor | Cube (5), top at y = 0.125. **`Rat.material`: dielectric, base color (1.0, 0.0147, 0.0147), roughness 1.0, IOR 1.536.** A red, rough, mostly diffuse floor. |
| Cube (19) | A 1 m cube at (−3.850, 0.875, 2.098), so its west face is at x = −4.350. **`TestMaterial`: metallicRoughness, metallic 1, base color 0.495 (achromatic), saved roughness 0.2.** |
| Cube (6) | Wall at x = −7.5, also TestMaterial (3 × 6.9 m metal wall). |
| Cube (20) | At (−5.75, 0.75, 2.25), rotated about 45°. `clean_steel`: metallic, roughness factor **0.0** × texture, so roughness 0, a **delta mirror** (`PathTraceBSDF.hlsli:251`, threshold 0.001). |
| Other walls, ceiling, Cube (13) | `builtin:default`. Cube (4) and others use `Rat`/`Rat2` (dielectrics). |

**Assumptions (not established from the file):**
- the reproduction used this scene;
- material roughness was set to 0.1 at runtime (the saved TestMaterial roughness is 0.2);
- R.9I.1D's material index 3 is TestMaterial. This is supported by the achromatic F, the 0.1 roughness and the zero diffuse part, but the material-table ordering was not checked.

## 3. Transport analysis

### 3.1 Depth semantics at maxPathDepth = 2 (`PathTraceReference.hlsl:1001–1240`)

`vertex` counts surface interactions; vertex 0 is the camera-visible (G-buffer) surface.

- **Vertices 0 and 1** get next-event estimation (`:1158–1176`) and BSDF sampling (`:1192`).
  - In the ReSTIR DI split mode, vertex 0's direct light is the stored RTXDI estimate (`:1167`).
  - Vertex 1 uses the conventional all-lights loop (`PathTraceDirectLightingLobes`).
- **The ray leaving vertex 1** is traced, and only its miss (environment/sun) is added (`:1096`/`:1144`). With environment and sun off, it adds nothing.
- **The point light is delta** (pdf 0, MIS weight 1; `PathTraceLights.hlsli:228–242`). It is never hit by a BSDF ray, so it is reached only by NEE.

Supported point-light paths at depth 2 (light first):

| Path | Camera order | Meaning |
|---|---|---|
| **L X₀ E** | NEE at x₀ | Direct lighting of any visible surface (RTXDI in ReSTIR DI mode) |
| **L X₁ X₀ E** | BSDF sample at x₀, NEE at x₁ | Exactly one bounce |

Everything with two or more intermediate bounces is absent at depth 2 (for example L D D D E, or L G D G E).

### 3.2 Classification of the one-bounce paths for the patch

| Variant | Present at depth 2? | Relevance to the floor patch |
|---|---|---|
| L D E (direct light on the floor) | Yes | Smooth and well sampled (RTXDI/NEE). Not grainy. Not red-*boosted*. |
| L D D E (floor lit by diffuse walls) | Yes | Low frequency. Defaults/dielectrics are spread out and dim. |
| **L G D E (light reflected by glossy metal onto the floor)** | **Yes** | **Main candidate.** Caustic-like; trapezoidal footprint (§3.3). |
| L S D E (point light via a delta mirror, Cube (20)) | **No: zero contribution** | At a delta x₁, `hasNonDeltaLobe` is false (specular probability 1 for a metal with no diffuse), so NEE is skipped (`:1158`). The delta continuation cannot hit a point light. **Engine cannot render the steel cube's caustic at any sample count**, which is standard for unidirectional path tracing. |
| L D G E / L G G E (camera sees a glossy reflection of a lit surface) | Yes | Seen *on the metal objects*, not on the floor. These produce the speckling on the reflective objects themselves. |
| Paths with two bounces (L G D D E, …) | No | Excluded by depth 2. |

### 3.3 Evidence that the patch is L → Cube (19) west face → floor

- **Geometric footprint.** The point light mirrored in the face plane x = −4.350, projected through the face corners onto the floor, gives a trapezoid with corners (x, z) of about (−4.376, 1.52), (−4.376, 2.66), (−4.627, 0.79), (−4.627, 3.22). It is narrow near the cube and widens away from it, and it lies **between Cube (19) and the steel Cube (20)**. Roughness widens and softens it.
- **Fit with the GPU candidate.** R.9I.1D candidate #1 values compared with this path:

  | Captured | Model |
  |---|---|
  | Light distance 1.076 | Distance from the light to the west face at y ≈ 1.2 (Δx = 0.19 from the face plane) |
  | Secondary cosine 0.18 | The face normal (−x) is at 0.19/1.07 ≈ **0.18** to the light |
  | Li = 1.728 | Intensity 2 / 1.076² = **1.728**: the light is exactly Point Light (5) with the range window ≈ 1 |
  | Primary throughput (1.33, 0.036, 0.036) | Red floor albedo × energy-compensated diffuse weight / diffuse selection probability. The x₀ surface is the red floor, not a red metal. |
  | First secondary distance 1.18 | Floor to face at y ≈ 1.2 |
  | Indirect red 6,726 | The model's per-sample maximum is about 4,600 × 1.33 ≈ 6,100, the same order |

- **The wall Cube (6) cannot produce the captured events:** it is at least 2.9 m from the light. The patch might also receive wall reflections, but those are wider and dimmer.

**Not established without runtime evidence:** that the patch the user sees is exactly this footprint, and that candidate #1's pixel lies on it.

## 4. Sampling strategy for L G D E

| Question | Answer |
|---|---|
| How is the path generated? | Camera → floor x₀ (G-buffer). At x₀ a mixture strategy picks cosine (diffuse) or GGX VNDF. For roughness 1.0 with F0 ≈ 0.045, selection clamps the specular share near its 0.1 minimum. The ray hits the cube face x₁. NEE at x₁ connects to the point light and evaluates the metal GGX f·cos·Li. |
| What is importance sampled? | The floor BSDF at x₀ (cosine-like) and the light choice at x₁ (deterministic, all lights). **Nothing samples the important quantity:** the floor directions whose x₁ has its glossy lobe aimed at the light. |
| Does the PDF match the integrand? | No. The integrand over the floor hemisphere is concentrated in a small solid angle (the light's image in the face). The cosine PDF is broad. |
| Is MIS available? | Not for this path. The point light is delta, so x₁ has only one strategy. At x₀ there is only one way to choose x₁ (BSDF sampling); no light-aware strategy exists. Making the point light a small sphere would allow MIS **at x₁**, but the bottleneck is at x₀. |
| Is it hard to discover? | Yes. Only about 1.6–5.7% of floor samples hit the useful part of the face at r = 0.1, and a few hundredths of a percent hit the lobe peak. |
| How often are contributions large? | About 0.08–0.11% of samples exceed 10× the mean (r = 0.1); the maximum is about 4,600–5,200 for unit albedo. |
| Does it converge without bias? | Yes. Every expression is unbiased (R.9I.1C/1E; this audit), with no clamp. Error falls as σ/√N, but σ/μ ≈ 126–163 per sample at r = 0.1. |
| Does any existing technique help? | No. ReSTIR DI acts only at x₀'s *direct* light. SHARC (off) caches incident radiance at path vertices and is not designed for sharp view-dependent glossy relay. NRD and Engine reconstruction (off) would filter, not resample. |

### The three sampling problems are not interchangeable

1. **Sampling a narrow glossy BRDF at an existing x₁:** irrelevant here. At x₁ the light direction is fixed by NEE; there is nothing to sample.
2. **Sampling an x₀ direction that must find an x₁ with favourable light alignment:** **this is the actual bottleneck.** The probability is set by the glossy lobe's footprint as seen from x₀.
3. **Sampling from the light toward the reflector and onto the floor:** follows the glossy lobe naturally. Light → face (cosine of emission) → GGX VNDF reflection → floor gives a PDF that closely matches the integrand. This is why light tracing, BDPT or photon methods suit L G D E.

## 5. Why a coherent patch looks granular

- **Accumulation** is a plain progressive mean (`:1475–1486`): mean' = mean·n/(n+s) + sum/(n+s). There is no spatial or temporal reconstruction with the denoiser off. Each pixel converges independently.
- **The estimator is heavy-tailed.** Most samples contribute 0 or a little, and rare samples contribute thousands of times the mean.
  - At N = 821 spp, a pixel sees about 47 face-lobe hits (5.7%), but only **about 0.9 samples above 10× the mean**.
  - Neighbouring pixels therefore differ mainly in whether they got 0, 1 or 2 rare events. Relative error per pixel is about σ/μ/√N ≈ 126/√821 ≈ **4.4 (440%)** at r = 0.1, and ≈ 39/√821 ≈ **1.4** at r = 0.2.
- **The patch is coherent in expectation and grainy in realisation.** The region with non-zero expectation has the trapezoid's shape, but each pixel's realisation is dominated by a handful of rare events. The bright speckles are the pixels that got lucky; the dark gaps underestimate until they do.
- **Roughness 0.2 is better but still slow.** The lobe is wider and the peak about 16× lower (R.9I.1E), giving roughly 10× fewer samples for the same error. Even so, about 0.6–1.2 million spp is needed for 5% error.
- **Source proves:** unbiased expressions, plain accumulation, and no filtering in this configuration.
- **Needs GPU evidence:** the actual per-pixel distribution in the patch; that the patch is this footprint; temporal stability (with a stationary camera and no jitter, nothing temporal beyond accumulation should apply, but jitter settings were not verified).

## 6. Variance-reduction options

| Approach | Paths improved | Energy / shape | Bias | Cost and memory | DXR complexity | Compatibility | Toggleable | Validation |
|---|---|---|---|---|---|---|---|---|
| **A. Light tracing (light subpath → camera splat), MIS with path tracing** | L G⁺ D E, and L D E variants | Preserved; sharp edges preserved | Unbiased (with MIS, or with a disjoint path partition) | One light-path raygen dispatch (≈ render resolution or fewer paths/frame) plus atomic splats to an R32 ×3 buffer | Moderate: emission sampling, VNDF at the reflector, camera projection, connection visibility, MIS PDFs of both strategies | Progressive Off mode: direct. NRD needs splat signals fed in. ReSTIR DI unaffected (vertex-0 direct). Off when disabled. | Yes | High-spp path-tracing reference comparison (mean, per-region error), the same render with the toggle off, an energy test |
| **B. Photon mapping / SPPM caustic photons** | L (G\|S)⁺ D E, **including the currently missing delta-mirror caustic (Cube (20))** | Consistent; kernel blur at edges shrinks over iterations | Biased, consistent | Photon buffer plus spatial hash (the SHARC hash-grid pattern is reusable), gather at x₀ | Moderate–high | Off mode direct. With NRD as a separate diffuse signal. | Yes | Convergence of the bias with radius, reference comparison, edge sharpness |
| **C. Full BDPT** | All | Preserved | Unbiased | High (subpath storage, many connections) | High | Hard with the split RTXDI and NRD pipeline | Yes | As A |
| **D. Manifold / specular NEE (planar or general)** | L S D E (delta) and narrow L G D E via MNEE-style glossy extension | Preserved | Unbiased (MNEE) or near-unbiased | Per-sample Newton solve against geometry | High; robust only for simple geometry | Fine | Yes | As A |
| **E. Path regularization / mollification** (roughen x₁ after a diffuse x₀) | L G D E | Energy roughly preserved if the lobe stays normalised; **blurs edges and shape** | Biased; consistent only with a shrinking schedule | Negligible | Low (one roughness override) | Full | Yes | The bias must be measured against a reference. Risks failing acceptance criterion 3. |
| **F. Reservoir reuse (ReSTIR GI / ReSTIR PT)** | Diffuse x₀ to an x₁ found by neighbours | Preserved if shifts are exact | Unbiased only with correct Jacobians **and re-evaluating x₁'s glossy BRDF toward the new x₀**. Engine's diffuse-focused ReSTIR GI direction, which reuses stored x₁ radiance, would be **biased** for glossy x₁. | Reservoir buffers at screen resolution | High | Planned GI direction; needs a glossy-aware reconnection (ReSTIR PT-style hybrid shift) | Yes | Reference comparison, especially near the patch edges |
| **G. Denoising (NRD RELAX diffuse, Engine)** | Any, by reconstruction | Can preserve the mean if the input is not firefly-clamped; anti-firefly paths darken; can blur edges | Biased (filtering) | Exists | — | Exists | Exists | Complementary only. Fails criterion 4/8 on its own with this input. |
| H. Luminance clamp | — | **Darkens the patch** | Biased | — | — | — | — | Rejected by criterion 2. |
| I. Sphere-light conversion / MIS at x₁ | L→x₁ only | Changes the light model | — | Low | Low | — | — | Does not address the x₀ bottleneck. |

## 7. Ranked recommendations

1. **(A) Light tracing for caustic-like paths with MIS.** This is the best match: it samples the important distribution directly, is unbiased, preserves brightness, color and edges, and stays bounded in memory. It fits a separate raygen and splat buffer, behind a toggle, in the progressive (denoiser Off) mode first.
2. **(B) Progressive photon mapping for L (G|S)⁺ D E** if delta-mirror caustics (Cube (20)) must also appear. They are currently impossible. It is consistent rather than unbiased and needs a radius schedule.
3. **(G) Reconstruction as a complement** after (A) or (B) have removed the heavy tail. The NRD diffuse signal suits a diffuse receiver.
4. **(F) Glossy-aware path reuse** as a longer-term direction. The diffuse-focused ReSTIR GI plan alone is not sufficient for this transport.
5. **(E) Regularization** only as a clearly labelled, biased preview option, measured against a reference.

## 8. One recommended next experiment (no code changes)

**Move Cube (19) by +0.5 m in x in the editor, temporarily.**

- **Change:** only Cube (19)'s position, from x −3.850 to −3.350, so its west face moves from x −4.350 to −3.850.
- **Keep fixed:** everything else — camera, Point Light (5), all materials (TestMaterial roughness as used in the reproduction), ReSTIR DI mode, 1 spp, depth 2, roulette depth 3, denoiser/DLSS/SHARC off. Restart accumulation and compare at the same sample count (about 800 spp).
- **Prediction if the patch is light → Cube (19) west face → floor:** the trapezoid's near edge stays at the cube's west foot and the patch **deepens from about 0.25 m to about 0.9 m** (x ≈ −3.94 to −4.84, z span unchanged at about 0.79–3.22). It becomes dimmer per unit area (same face, larger footprint) and its graininess persists. Optionally, R.9I.1D's top candidates should show a changed light distance and cosine, consistent with the new face position.
- **Against it:** the patch stays put, or tracks a different object (the wall Cube (6) or the steel Cube (20)).
- **Effect on the reflection:** it moves the desired reflection; it does not remove it.
- **Return to baseline:** undo the move (Ctrl+Z, or set x back to −3.850) and do not save the scene. Alternatively, reload the scene without saving.

## 9. Acceptance criteria for a future solution

1. The trapezoidal patch remains, at the same location and footprint for unchanged geometry.
2. Mean radiance within the patch matches a high-spp reference within a stated tolerance (for example 2% over a patch region mask), with no global darkening. Red color transfer preserved (RGB ratio within tolerance).
3. Edge position and falloff match the reference (for example an edge profile or gradient comparison), with no extra blur beyond what the reference shows.
4. Per-pixel variance and firefly frequency in the patch drop substantially at equal time (report the relative MSE per unit time against the path tracer).
5. Practical convergence: lower patch error at 256/1024 spp than the current estimator, measured.
6. No regressions: direct lighting, non-caustic indirect light and glossy reflections on the metal objects are unchanged against the reference.
7. With the feature off, the output is identical to the current estimator (source- and image-level check).
8. Every judgement is made against a converged reference, never by visual smoothness alone.

## 10. Uncertainties and limitations

- No Engine launch, no GPU rerun, no screenshot analysis. The patch-to-footprint attribution is supported by geometry and by the exact match of the captured light distance, cosine, Li, throughput and peak magnitude. It is **not** confirmed visually or at a specific pixel.
- The roughness at runtime (0.1) differs from the saved TestMaterial (0.2). Material index 3 = TestMaterial is inferred.
- The model ignores visibility between floor, face and light, the floor's small specular lobe and multiple scattering (tiny at the secondary, R.9I.1E), and texture effects. Statistics come from 4 million samples per point at three floor positions. They show order of magnitude and are not exact.
- Wall Cube (6) reflections could add further, broader, dimmer patches. They were not modelled.
- The cost and complexity figures in §6 are architectural estimates, not measurements.
