# R.9I.1C — secondary specular transport audit

2026-10-27. Read-only investigation. No source, diagnostic, settings, scene, build configuration or estimator changes.

## 1. Executive summary

**No concrete GGX sampling/PDF, point-light MIS or double-weighting defect was demonstrated. A targeted runtime factor check is the next step; a correction or clamp is not justified yet.** The inspected implementation uses GGX visible-normal sampling with the appropriate reflection-direction PDF, then the full mixture BSDF/PDF weight. Secondary point lights are evaluated once each, with inverse-square incidence and delta-light weight one.

Two distinct, mathematically valid high-variance mechanisms deserve priority:

1. A sampled secondary surface can have a very sharp BRDF response toward the point light, amplified by short light distance. This direct-light evaluation is not divided by a secondary BSDF sampling PDF; it is the correct point-light connection, and its peak need not cancel.
2. The **multiple-scattering compensation component** of the existing specular BSDF can produce large primary throughput at grazing incidence, even though the **single-scattering GGX component** has a well-behaved matching VNDF PDF. Isolated calculations with the actual bundled tables found an approximately 66.66 weight at roughness 0.1, NoV=0.02, versus a 3.89 maximum in the corresponding roughness-0.2 sample set. These are CPU examples, not observed GPU samples or bounds on the renderer.

The second finding materially narrows earlier audits: “GGX peak” and “large specular throughput” must not be conflated. The current BSDF contains single scattering **and** compensated multiple scattering. R.9I.1's specular remainder does not distinguish those components.

Roughness 0 uses a separate delta-mirror branch, removes multiple scattering, and changes stochastic path behavior. It is not a continuous low-roughness control for the 0.1/0.2 cases. The reproduction excludes SHARC, environment/sun, NRD and DLSS as necessary sources. Progressive accumulation remains the verified mechanism retaining an outlier, not evidence that its generating weight was wrong.

## 2. Reproduction evidence and limits

Accepted as user observations: sealed room, one enabled point, environment/sun/SHARC/denoisers/DLSS off; metallic=1; pronounced colored noise and bright pixels at roughness 0.1; much smoother at 0.2; colored floor-like noise but no obvious white pixels at 0; point movement redistributes artifacts and proximity to a box edge concentrates them; depth 1 removes them, depth 2 introduces them, and greater depth increases opportunities.

These strongly localize the issue to reflective surface-to-surface transport. They do not identify which surface has the relevant roughness, whether every secondary material is metallic, its model/texture-resolved parameters, primary grazing angles, point-to-hit distances, actual roulette setting, or the linear HDR values of a bright event. Metalness=1 only suppresses diffuse for material **model 0**; models 1 and 2 have different semantics. No numeric R.9I.1 record from the reproduction was supplied. No scene was launched or GPU sample independently reproduced for this audit.

Reviewed `docs/R9HIndirectBrightPixelAudit.md`, `docs/R9JRestirGIArchitectureAudit.md` and `docs/R9I1IndirectSampleDiagnostic.md`; current source is authoritative. Earlier reports' line numbers predate instrumentation, so references below use current locations. Shader paths below are relative to `assets/Shaders/` unless stated otherwise.

## 3. Complete secondary specular path walkthrough

1. **Primary material and frame:** `PathTraceReference.hlsl:399–467`, `PathTraceBuildSurface`, resolves camera-traced hits; raster-primary decoding is `PathTracePrimarySurface.hlsli:120–166,174–184`. Both call `PathTraceFinalizeSurface` (`PathTraceBSDF.hlsli:228–279`). Geometric/shading normals are faced toward the incoming view; finalization validates hemispheres and corrects the shading normal near its view horizon.
2. **Metallic reflection sample:** `PathTraceSampleBSDFLobes` (`PathTraceBSDF.hlsli:459–516`) chooses VNDF/delta versus cosine proposal, produces direction, evaluates full BSDF and mixture PDF, and returns f*cos/pdf. A cosine-selected event can still be **entirely specular** on a metal; proposal name is not a physical diffuse classification.
3. **Throughput:** `PathTraceTracePath` (`PathTraceReference.hlsl:988`), `:1190–1249`, multiplies throughput by that weight once; possible roulette divides by survival once; the continuation uses geometric-normal origin offset and the sampled direction. Primary view is the direction toward the camera, secondary view is minus the arriving ray direction (`:402`).
4. **Secondary hit:** a hit at x1 is built/shaded only when max depth>=2 (`:1094–1104,1142–1152`). Depth 1 may trace the continuation but stops before illuminating its surface. Thus no camera -> x0 -> x1 -> point term exists at depth 1.
5. **Secondary point connection:** `:1155–1187` calls `PathTraceDirectLightingLobes` except for split RTXDI x0. The conventional function (`:540–584`) evaluates every table light. `PathTraceSampleTableLight` (`PathTraceLights.hlsli:228–243`) calls incidence and marks a delta sample. `PathTraceShadeLightSampleLobes` (`PathTraceReference.hlsl:496–531`) evaluates f1*cos1*Li, checks visibility and returns it with weight one for the point.
6. **Camera contribution and accumulation:** `radiance += throughput*direct` (`:1177`); complete sample at `:1426–1443`, rejection at `:1435–1440`, actual progressive mean at `:1471–1482`. It is the original total radiance that is accumulated. Diffuse/specular extraction (`:1181–1211,1497–1498`) does not feed back into total path radiance.

RTXDI direct reservoir weighting is confined to split primary x0 (`:1162–1174`); x1 uses conventional direct evaluation. The one-point depth dependence does not implicate the primary reservoir. SHARC replacement hook still exists (`PathTraceRadianceCacheQuery.hlsli:390–507`), but the established reproduction explicitly disables it.

## 4. GGX/BSDF/PDF mathematical audit

Let n be the finalized shading normal, v point toward the previous vertex, l toward the next, h=normalize(v+l), mu_v=n·v>0 and mu_l=n·l>0. All densities below are with respect to outgoing solid angle.

### Roughness, distribution and proposal

`PathTraceGGXAlpha` (`PathTraceBSDF.hlsli:108–112`) computes alpha=max(r,0.045)^2. Stable D at `:291–299` uses alpha^2/[pi((n·h)^2*alpha^2+|n×h|^2)^2]. The geometry helper independently uses the same roughness mapping (`BRDF.hlsli:117–145`). Therefore r=0.1 -> alpha=0.01 and r=0.2 -> alpha=0.04; treating r itself as alpha would be a mistake, but the inspected sampling and evaluation both square it consistently.

`PathTraceSampleVisibleNormal` (`PathTraceBSDF.hlsli:414–442`) stretches v by alpha, constructs the projected-disk basis, warps its disk sample for visible normals, then unstretches/normalizes. This is **VNDF**, not NDF sampling. Its structure matches the method described by [Heitz, Sampling the GGX Distribution of Visible Normals](https://jcgt.org/published/0007/04/01/).

For a visible microfacet normal, the expected density is:

`p_h(h|v) = D(h)*G1(v)*max(v·h,0)/mu_v`

Reflection has Jacobian `|dh/dl|=1/(4*|v·h|)`, giving:

`p_VNDF(l|v) = D(h)*G1(v)/(4*mu_v)`.

`PathTraceVisibleNormalPdf` (`:378–398`) implements exactly this canceled form, with half-vector length/support checks. It is **not missing a factor v·h**: that factor cancels the reflection Jacobian. `PathTraceSmithG1` (`:371–375`) is the GGX G1 for the same alpha. With normalized valid input there is no demonstrated sampling/PDF mismatch.

### Geometry and Fresnel

Single scattering is `f_ss=D*G2*F/(4*mu_l*mu_v)` with denominator floor 1e-6 (`PathTraceBSDF.hlsli:302–321`). `GeometrySmithGGX` (`BRDF.hlsli:117–145`) returns the height-correlated Smith G2:

`G2 = 2*mu_l*mu_v / (mu_v*sqrt(alpha²+(1-alpha²)*mu_l²) + mu_l*sqrt(alpha²+(1-alpha²)*mu_v²))`.

The denominator floor is a finite numerical regularization. Using G2 in evaluation and G1 in the VNDF PDF is **correct**, not a masking mismatch: the resulting importance weight retains G2/G1. They serve different roles.

Model 0 uses Schlick F0=lerp(0.04,baseColor,metallic) (`PathTraceBSDF.hlsli:114–124`); at metallic=1, F0=baseColor. Its sampled/view Fresnel and BRDF Fresnel use the same model, evaluated at their appropriate view or half angle (`:143–158,314–320`; `BRDF.hlsli:147–154`). Model 2 instead uses complex-IOR conductor Fresnel (`BRDF.hlsli:156–214`); a metallic slider does not convert model 1 into model 0. No contradictory Fresnel branch was found. Physical bounds assume sensible finite baseColor/IOR/conductor data; secondary baseColor is not universally upper-clamped.

### Lobe probabilities and metalness

Model-0 diffuse albedo is baseColor*(1-metallic) (`PathTraceBSDF.hlsli:127–140`), so exactly resolved metallic=1 gives zero EON diffuse. However the specular BSDF still includes compensated multiple scattering (`:160–185,327–352`). The cosine proposal therefore often remains enabled on metals.

`PathTraceFinalizeSurface:253–279` estimates specular selection using view Fresnel and cosine selection using diffuse+multiple-scatter albedo; if both are positive, p lies in [0.1,0.9]. The reflection mixture PDF is `q=p*p_VNDF+(1-p)*mu_l/pi` (`:401–411`). Non-delta sample weight uses the **full** BSDF and that **full** q (`:504–515`). There is no additional division by selected-strategy probability. Probabilities need not equal exact lobe energy for correctness as long as the actual proposal PDF is used.

At delta roughness<=0.001, the mirror branch returns Fresnel/p, checks the reflected geometric hemisphere, and records a delta event with PDF=0 (`:473–485`). For model-0 metallic=1, no diffuse and no multiple scattering remain in this branch, so ordinarily p=1. The continuous cosine/delta mixture handling is distinct from the non-delta mixture.

### Hemispheres and numerical limits

`PathTraceDirectionAboveSurface:282–285` requires positive shading and geometric normal dots. Invalid sampled directions are zero events without retry/renormalization (`:498–501`), which preserves the unconditional proposal's zero-event accounting. Surface/normal construction checks finiteness and normalizes/falls back (`PathTraceReference.hlsl:1763–1800,1831–1840`; `PathTracePrimarySurface.hlsli:125–144`). `ShadingNormal.hlsli:6–63` applies view-dependent horizon correction near NoV=0.02. This is an acknowledged shading-normal/reciprocity approximation, not a roughness-0.1 switch.

Non-delta q must be positive/finite; sample weight finite (`PathTraceBSDF.hlsli:504–515`). No lower bound is imposed on valid positive q. Small q alone does not imply a bad weight because numerator and density may shrink together. Degenerate half-vector guard exists in PDF; BRDF normalizes v+l directly, but valid positive NoV/NoL excludes exact antiparallel unit directions. Invalid finite geometry/material values and float32 extreme cases remain runtime concerns, not proven causes.

## 5. Secondary point-light and MIS audit

`src/Renderer/FrameLighting.cpp:22–47` packs radiance=color*intensity once. `SceneLights.hlsli:34–66` computes direction to point, distance d and:

`Li = authoredRadiance * saturate(1-(d/range)^4)^2 / max(d²,0.01²)`.

There is no sampled emitter position or finite area-light PDF. `PathTraceLights.hlsli:228–243` sets point PDF=0 and validates finite positive incidence/nonzero distance. Every active table light is evaluated (`PathTraceReference.hlsl:561–569`); with one point the inclusion probability is one. There is neither a missing 1/lightCount factor nor a second selection-probability division.

`PathTraceShadeLightSampleLobes:499–531` multiplies by the secondary shading cosine once, rejects directions below either hemisphere, tests visibility once and applies MIS/PDF only when light PDF>0. For the delta point, weight=1. BSDF rays cannot hit the analytic zero-area point source as emissive geometry in this implementation, so no competing secondary BSDF-hit strategy needs a point-light MIS weight. Dividing the point contribution by the secondary BSDF PDF would be **incorrect**; its absence is correct. Sun/environment MIS is separate and inactive in this reproduction.

Visibility uses `PathTraceOffsetRayOrigin`/`PathTraceVisible` (`:329–371`): geometric-normal offset, TMin=0, shortened positional TMax and binary occlusion via DXR/RayQuery. Moving the point changes visibility, distance and half-vector alignment simultaneously. A box-edge concentration can therefore result from legitimate reflection/occlusion changes, near-point energy, or numerical visibility errors. It does not establish self-intersection. Offset origin versus direction/distance computed from the original point is a numerical approximation worth checking only if telemetry implicates boundary visibility; no bad ray was observed.

For d much smaller than range, attenuation is approximately 1 at 1 m, 100 at 0.1 m, 2500 at 0.02 m and 10,000 at/below 0.01 m. The floor makes it finite, not small. At exactly zero distance the table sample is invalid. No extra inverse-square factor or light-intensity multiplication was found in throughput or the secondary direct sum.

## 6. Throughput and energy accounting

For the specified two-surface point-light path, before roulette:

`T1 = [f0_ss(v0,l0)+f0_ms(v0,l0)] * mu0 / q0(l0)`

`C_indirect = T1 * f1(v1,lightDirection) * mu1 * Li1 * V1`.

Here q0 is the actual primary mixture density and includes lobe-selection probabilities; the point's inclusion probability and MIS multiplier are both one. If roulette is active before x1, surviving T1 additionally divides by survival probability s once. Default roulette starts at 3 (`src/Renderer/RendererLightingMode.h:48`), so default depth 2 does not introduce that arriving multiplier. User override is unknown. Sampling x1's continuation happens **after** its direct term and cannot retroactively multiply x1 direct by the next BSDF weight (`PathTraceReference.hlsl:1177,1190–1215`).

The matching primary **single-scatter** term simplifies, where numerical guards are inactive:

`f_ss*mu_l/p_VNDF = F*G2/G1(v)`.

For physical 0<=F<=1, G2/G1<=1. Since q_mix>=p*p_VNDF, the single-scatter part of full weight is bounded by 1/p. Thus a narrow primary D peak does not by itself imply enormous arriving throughput. This bound does **not** apply to the full compensated BSDF or early roulette.

The multiple-scatter component actually implemented is:

`f_ms = Fms * M(mu_l)*M(mu_v)/(pi*Mavg)`

`Fms = Favg²*(1-Mavg)/(1-Favg*Mavg)`.

See `PathTraceBSDF.hlsli:160–185`. At small roughness Mavg can be tiny while grazing directional missing energy M is much larger. This reciprocal compensation can therefore have a large pointwise value despite bounded integrated energy. Its cosine proposal is broad; the VNDF proposal is concentrated around single-scatter reflection. The mixture is valid but not guaranteed low variance for this component. A sample weight above one is not a violation of energy conservation; expected reflected energy, not every Monte Carlo sample, is the relevant constraint.

Likewise the secondary single-scatter D is **not** divided by an x1 BSDF sampling PDF in the direct point-light term. For v1=l1=n1, unit incidence and F0=0.8, single-scatter f*cos is approximately 636.62 at r=0.1 and 39.79 at r=0.2: a factor **16**, because peak D scales as r^-4. These are analytic aligned-direction examples, not scene measurements. Narrower peak support balances its larger magnitude in the angular integral, but a path that finds that alignment can be very bright.

The final total sample is dropped only if nonfinite/negative (`PathTraceReference.hlsl:1435–1440`). Large finite positive C passes and contributes to the running mean (`:1471–1482`). An event E has an E/N share until reset; target stopping holds the resulting image (`src/Renderer/Renderer.cpp:6653–6663`). No evidence shows an incorrect division in the mean. Reflective RGB weights and colored secondary surfaces can produce colored speckling; large channels can appear white after exposure/tone mapping. Not all colored speckles must be high-luminance outliers.

Diffuse/specular classification does not cause extra energy in total radiance: at model-0 metallic=1 the primary diffuse share is zero (`PathTraceBSDF.hlsli:515`; `PathTraceReference.hlsl:1203`), and extracted specular is total minus diffuse (`:1497–1498`). Choosing the cosine proposal on a metal does not make that sample physically diffuse.

## 7. Roughness-dependent behavior and isolated validation

There is no transport switch at r=0.1 or 0.2 in the inspected BSDF. Relevant switches are delta at 0.001, minimum non-delta roughness 0.045, and continuous lobe-probability clamping. LUT interpolation is bilinear with texel-center clamping (`PathTraceBSDF.hlsli:75–105`). Changing roughness can substantially reshape peaks/tails without a branch discontinuity.

Textures multiply roughness and metalness (`PathTraceReference.hlsl:374–396,434–459`; raster material writing in `Triangle.hlsl:578–593,629–646`). Raster primary roughness is G-buffer alpha, **8-bit linear UNORM**, despite the RGB sRGB format (`src/Renderer/GBuffer.h:11–15`; `PathTracePrimarySurface.hlsli:146–159`). Camera/secondary hits resolve material values directly. Thus slider values do not prove identical effective roughness or metallic on every surface. Quantization and texture/LOD differences can move a narrow highlight but do not establish incorrect alpha/PDF weighting.

**Roughness zero:** delta selection removes both finite-width single-scatter evaluation and multiple-scatter compensation, and changes the continuation to ideal reflection. It need not display the same white outlier mechanism. Floor-like colored noise can still come from reflected secondary rough/diffuse materials, textures/coverage or deeper stochastic paths. Conversely, if every encountered surface were truly perfect delta metal and all environment/emission were absent, the analytic point cannot be reached via geometric BSDF hits and delta surfaces skip conventional NEE: nonzero floor-colored point illumination would require some additional non-delta material/source/state. The observation does not establish that all those stronger conditions hold.

### Calculations performed

Disposable scripts/results live under ignored `build/r9i1c/`; reproduce with `python build/r9i1c/numerical_audit.py`. These translate the inspected equations in NumPy float64, use the actual 512x512 bundled LUT and do not execute HLSL or launch Engine. Its SHA-256 matches the pinned `a974ed16…df90e` (`src/Renderer/MaterialTables/MaterialTableFile.h:68–73`); no table-corruption evidence was found.

- Six cases, 250,000 mixed samples each, r=0.1/0.2 and NoV=1/0.2/0.02, model-0 metal with scalar F0=0.8, aligned geometric/shading normals, no roulette. Both proposals used the actual mixture density and LUT compensation.
- Independent VNDF support checks: normal-incidence sampled slope CDF agreed with its analytic GGX CDF (KS<0.005); oblique half-normal moments agreed approximately with independently NDF-sampled, VNDF-density-reweighted estimates. Estimated normalization ranged 0.99974–1.00461; this finite-sample check is not a proof of the whole distribution or GPU implementation.
- Explicit microfacet-PDF/Jacobian formula and implemented canceled PDF agreed to maximum relative difference 4.53e-16 in these float64 valid samples. This checks the algebra, not a float32 DXR precision bound.

| r | NoV | Mean full weight | 99th percentile | Maximum observed weight |
|---:|---:|---:|---:|---:|
| 0.1 | 1 | 0.79993 | 0.88885 | 0.88885 |
| 0.2 | 1 | 0.79927 | 0.88825 | 0.88826 |
| 0.1 | 0.2 | 0.86576 | 0.97727 | 1.85370 |
| 0.2 | 0.2 | 0.85647 | 0.95777 | 2.39746 |
| 0.1 | 0.02 | 0.94187 | 1.06596 | **66.66137** |
| 0.2 | 0.02 | 0.90219 | 1.44627 | **3.89012** |

All six cases selected p=0.9 from the actual selection rule. The largest r=0.1/NoV=0.02 example decomposed into single-scatter weight **0.32548** and multiple-scatter weight **66.33589**, with outgoing NoL=0.01618 and q=0.00085336. Mview=0.08645 and Mavg=0.0005111. Single-scatter weight stayed <=1/p across the tested valid events. No nonfinite full weights occurred. These observed maxima are seed/sample-count dependent, not universal maxima, and the table does not claim roughness 0.2 always has lower variance at every angle.

This establishes **a concrete admitted high-weight mechanism**, not an estimator mismatch or the user's cause. It also explains why testing only single-scatter GGX would miss a relevant tail. No float32 shader execution, geometry/point-connection distribution, actual material RGB or numerical energy-integral convergence proof was performed. CPU sample means are not definitive energy certificates for all materials or the LUT approximation.

## 8. R.9I.1 diagnostic coverage and gaps

Current bookkeeping is `PathTraceSampleDiagnostic.hlsli:18–96,119–125`, record layout `PathTraceSampleRecord.h:14–37`, and light hook `PathTraceReference.hlsl:528`.

For this one-point/depth-2/SHARC-off/environment-off case, **indirect ≈ secondary direct ≈ dominant visible light** when the first continuation hits a valid non-delta surface illuminated by the point. A nonzero primary direct term can coexist; it is separate. Compare within floating-point regrouping tolerance. Existing telemetry can already check:

- Raw sample and indirect RGB/Y versus the actual accumulated mean; rejection bit distinguishes an estimate production did not keep.
- Dominant light must be the point at vertex 1; light PDF=0 and MIS/PDF multiplier=1. Light index is table index, not necessarily an authored persistent ID.
- `dominantRGB ≈ incomingThroughputRGB * factorsRGB` for this point; `factors` truthfully contains secondary **f*cos*Li**, not raw f. This checks the multiplication chain directly.
- High incident RGB/short light distance implicates proximity; moderate incoming throughput with large secondary f*cos*Li implicates the secondary response; large arriving throughput implicates earlier BSDF/roulette amplification. The secondary diffuse/specular decomposition tells whether that vertex's response is diffuse or specular.
- SHARC flag must remain false in the established setup; current peak stores actual substitution participation, not training status. Mean changes across increasing N separate source events from retention/display appearance.

Limits: incoming throughput is the product of prior weights and roulette, not an independent recomputation. Incoming BSDF PDF is recorded but primary f, cosine, selected strategy/p, primary normals/view/material roughness and single/multiple-scatter split are absent. Captured roughness/material belong to the **dominant light's vertex**, not the primary. `specular` includes both GGX single and multiple scattering. A small incoming PDF or large incoming throughput cannot by itself prove incorrect f/q. Raw secondary f is not captured separately. Rejected/occluded samples have no dominant visible-light record; the peak can be below detail threshold. Shader-on/off GPU equality was not established by R.9I.1 and is not asserted here.

If existing records show large incoming T1 and cannot resolve its origin, the smallest targeted later extension would capture **x0 NoV/NoL, effective roughness/model, p/selected strategy, q_VNDF/q_cos/q_mix, f_ss*cos/q and f_ms*cos/q, plus applied survival probability**, for the selected peak only. Retain raw sampled direction/normal if needed for independent PDF recomputation. This is a proposed follow-up, not implemented and not a request for a broad path trace or automatic detector. First use the existing telemetry.

## 9. Ranked findings and confidence

| Finding/classification | Actual versus expected behavior; fit and evidence | Focused validation; confidence |
|---|---|---|
| **1. Expected variance: secondary BRDF peak and point proximity** | Point term is T1*f1*cos*Li with no fictitious BSDF-PDF division (`PathTraceReference.hlsl:496–531,1177`; `SceneLights.hlsli:34–66`). Narrow secondary glossy alignment and inverse-square energy can be very large yet correct. Fits roughness/light/depth dependence, but actual secondary roughness/distance remain unknown. | Existing peak: compare incident, distance, secondary specular remainder and incoming T1. **High** for available mechanism; **moderate** for scene attribution. |
| **2. Expected variance: compensated specular throughput tail** | Full BSDF includes f_ms and samples a VNDF/cosine mixture (`PathTraceBSDF.hlsli:160–185,253–279,401–411,504–515`). Tested grazing r=0.1 admits large finite f_ms/q without a PDF mismatch. Fits reflective/grazing edges and smoother r=0.2; actual x0 angles are missing. | Existing arriving throughput first; only then the narrow x0 factor extension above. **High** for analytical/CPU mechanism; **unresolved** as GPU root cause. |
| **3. Expected variance/verified retention: progressive mean** | Finite outlier retained as E/N; tracing stops at target with Off/DLSS Off (`PathTraceReference.hlsl:1435–1482`; `Renderer.cpp:6653–6663`). Explains persistence/reset, not generation or roughness dependence alone. | Peak/current mean with exact N across a fresh run. **High** source confidence. |
| **4. Not supported: VNDF reflection Jacobian missing, NDF/VNDF mismatch, repeated primary lobe division** | Sample/PDF share alpha; G1*D/(4*NoV) includes canceled Jacobian; non-delta uses full mixture once (`PathTraceBSDF.hlsli:371–442,459–516`). Correlated G2 versus VNDF G1 is expected. No demonstrated inconsistency. | Independent x0 factor/PDF comparison if records implicate T1. **High** source/algebra confidence, not a blanket GPU precision proof. |
| **5. Not supported: point selection/MIS/attenuation duplicated** | Every table light included once, point PDF=0, light multiplier=1, intensity packed once (`FrameLighting.cpp:22–47`; `PathTraceLights.hlsli:228–243`). Adding 1/q_BSDF here would introduce error. | Existing point factor identity and light distance. **High**. |
| **6. Plausible defect: finite material/normal/visibility pathology** | Resolved secondary color not universally upper-bounded; model/texture state may differ from sliders; normal correction and offsets approximate geometry (`PathTraceReference.hlsl:399–467,329–371`; `ShadingNormal.hlsli:6–63`). These could produce excessive response/leaks, but none is demonstrated. No roughness-0.1-specific broken branch found. | Identify actual material/angles and a suspect visibility event before additional ray telemetry. **Low** root-cause confidence. |
| **7. Unresolved: float32/LUT extreme behavior** | Stable D, length/PDF/finite guards exist; LUT matches pinned bytes. Compensation is an approximate BRDF and guards regularize extremes; CPU float64 tests do not rule out GPU-only precision errors. | Targeted CPU/GPU factor comparison for a captured problematic event, not global clamp. **Unresolved**. |
| **8. Not supported as necessary origins: SHARC, environment, sun, NRD, DLSS, or primary DI reservoirs** | First five explicitly off; x1 does conventional illumination and requires depth 2. These cannot be assigned the controlled reproduction's source merely because they exist elsewhere in Engine. | Confirm effective diagnostic flags/state if contradictory terms appear. **High** conditional on the established configuration. |

**Confirmed defect category:** none established in the audited estimator for this reproduction. Known approximations/finite guards and substantial variance are documented separately from defects. Neither colored noise nor white display pixels alone determine which mechanism dominates.

## 10. Single recommended next experiment and conclusion

Perform **one selected-pixel factor-consistency capture** in the already reproducing configuration: metallic material at roughness 0.1, depth 2, one point, environment/sun/SHARC/denoiser/DLSS off, fixed camera/light. No new diagnostic is required.

Enable existing R.9I.1 at a previously affected internal render coordinate, set detail threshold 0, and restart accumulation by changing the PT sample target. Capture the peak indirect record during the fresh run and retain its raw numerical values before changing selection. Record current roulette-start setting without changing it. Inspect the same peak's indirect/secondary/dominant RGB, incoming throughput/PDF, secondary roughness/material, Li/distance, f*cos*Li, diffuse/specular remainder, light PDF/weight, flags and subsequent mean/N.

For a normal unrejected point-lit x1 event, check indirect≈secondary≈dominant and dominant≈T1*(f1*cos*Li), point weight=1/PDF=0 and SHARC=false. A large T1 directs the investigation toward primary compensation/weights or early roulette; moderate T1 plus high Li directs it toward proximity; moderate T1/Li plus large secondary specular response directs it toward secondary BRDF alignment. Later decreasing mean with no recurrent bright event establishes retention/dilution rather than continually identical raw samples. This experiment can distinguish those locations of amplification without pretending the existing fields independently validate every BRDF factor.

If no bright sample is captured at that coordinate, the result is inconclusive, not proof of correctness. If the multiplication chain is inconsistent beyond rounding, first verify record/run identity and diagnostic correctness before asserting an estimator fault. If it is consistent but T1 is inexplicably large, propose the narrow x0 factor extension; do not implement it automatically.

**Conclusion:** evidence presently supports a **targeted numerical/runtime diagnostic using the existing capture**, with valid specular variance as a strong source-grounded explanation. It does not support a specific source-level correction, R.9I.1B automatic detection, or R.9I.2 clamping. Variance reduction can be evaluated after the event's factors are known; no renderer fix was created.

Validation summary: six isolated CPU sampling cases, analytic PDF/Jacobian comparison, actual pinned-table reads, point attenuation/aligned-BRDF calculations and current-source review. No build, shader modification, renderer launch, GPU capture or runtime-setting change. The report is the only new nonignored artifact; existing working-tree files were preserved. Nothing staged or committed. Stop here.
