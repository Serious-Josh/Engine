# R.9I.1E — secondary specular BRDF peak verification

2026-10-27. Read-only investigation: no production code, shader, setting or diagnostic was changed. Scratch calculations are in the ignored folder `build/r9i1e/` (`brdf_peak.py`, `ms_term.py` and their `.txt` outputs).

## 1. Executive summary

**The secondary BRDF value of about 16,300 is consistent with Engine's implemented GGX single-scatter lobe at roughness 0.1. No defect was found.**

- **The peak is attainable.** At roughness 0.1, Engine uses α = r² = 0.01, so the peak of D is 1/(πα²) = 3,183. For a near-mirror configuration with n·l = n·v = 0.18 and an F≈1 surface, the full single-scatter value is f = D·G·F / (4 n·l n·v) ≈ **24,500**.
  - The observed 16,300 needs the half-vector only about 0.004 rad off the normal.
- **The secondary peak is single scattering only.** The secondary multiple-scattering lobe is at most **0.0019** in this configuration, measured with the bundled tables.
- **The other suspects are ruled out:**
  - shading-normal correction cannot produce the peak;
  - float32 evaluation is stable;
  - Smith G is the correct correlated G₂, not a visibility term divided twice;
  - point-light incidence is the attenuated value;
  - the diagnostic fields are the actual evaluated quantities.
- **The cause is a sharp glossy-to-glossy point-light connection.** The path is camera → glossy primary → glossy secondary → point light. The primary bounce is sampled from the primary BSDF and does not target the secondary's very narrow lobe. Occasionally a sample lands close to the secondary mirror configuration, and the delta point light then contributes a peak value that is not MIS-weighted.
- **This matches the observed roughness behaviour.** GGX peak height scales as 1/α² = 1/r⁴ and the lobe narrows with it. Roughness 0.1 gives a peak about 16× higher than roughness 0.2 (24,500 versus 1,500) in a lobe about 4× narrower. That fits roughness 0.1 being speckled and roughness 0.2 being smoother.

Classification: **expected variance** (high confidence) of an unbiased estimator. It is not an evaluation defect.

## 2. GPU evidence used

These are the R.9I.1D capture values supplied for this task:

- 2868×1065, Raster Primary ReSTIR DI, 1 spp, depth 2, denoiser/DLSS/SHARC off, one point light.
- All 32 retained candidates: specular secondary direct from point light 0, material 3, roughness 0.1, no SHARC.

Candidate #1:

| Field | Value |
|---|---|
| Throughput | (1.327, 0.0363, 0.0363) |
| f·cos·Li | 5,068.356 (all channels equal) |
| cos | 0.18 |
| Li | 1.728 |
| Light distance | 1.076 |
| Light PDF / weight | 0 / 1 (delta) |

Derived (not captured): f = 5,068.356 / (0.18 · 1.728) ≈ **16,295**.

The other candidates have f·cos·Li of about 3,700–5,400, cosines of 0.14–0.21 and Li of 1.1–2.3. That implies f of roughly 10,000–23,000, every one below the about 24,500 near-mirror peak at those cosines.

## 3. Source path, secondary vertex to recorded value

1. **Material resolution** — `PathTraceReference.hlsl:447–460`. Roughness is the material factor multiplied by the ARM or roughness texture red channel and saturated. Metallic is resolved the same way. The surface is then finalized.
2. **Finalization** — `PathTraceBSDF.hlsli:228–280`.
   - The shading normal is horizon-corrected against the view direction (`ShadingNormal.hlsli:6–58`; it only acts when n_s·v < 0.02).
   - Roughness and metallic are saturated, giving the exact `surface.material.roughness` used by every lobe.
   - Delta is chosen only when roughness ≤ 0.001.
3. **View direction:** the reversed incoming ray, pointing away from the surface. **Light direction:** normalized toward the point light (`SceneLights.hlsli:42–45`).
4. **Light incidence** — `SceneLights.hlsli:34–67`. It is `radiance · window² / max(d², min²)`, so the recorded Li is **after** attenuation. `PathTraceLights.hlsli:228–242` sets pdf = 0 (delta).
5. **Shading** — `PathTraceReference.hlsl:498–535`. The function first requires the light direction to be above both the shading and geometric hemispheres. Then:
   - `cosine = dot(surface.normal, l)`;
   - `value = EvaluateBSDFLobes(surface, l) · cosine · Li`;
   - the weight stays at 1 because pdf = 0.

   After the visibility test, the diagnostic records `value` and `cosine` (line 530).
6. **BSDF** — `PathTraceBSDF.hlsli:327–353`: single scatter + multiple scatter, plus energy-weighted EON diffuse for models 0 and 1. Model 2 (conductor) has no diffuse.
7. **Single scatter** — `PathTraceBSDF.hlsli:302–325`: f = D·G·F / max(4 n·l n·v, 1e-6). Both cosines use `surface.normal`, and h = normalize(l + v).
   - **D** — `:291–300`: α = max(r, 0.045)², the stable cross-product form α² / (π(cos²θh α² + sin²θh)²).
   - **G** — `BRDF.hlsli:117–145`: 2 n·l n·v / (n·v √(n·l²(1−α²)+α²) + n·l √(n·v²(1−α²)+α²)). This is Heitz's height-correlated **G₂** (≈1 near grazing at small α), not the visibility V = G₂ / (4 n·l n·v). There is no double division.
   - **F:** Schlick at v·h with F0 = lerp(0.04, baseColor, metallic), or the conductor Fresnel for model 2.
8. **Multiple scatter** — `:167–186`: Fms·M(l)·M(v) / (π Mavg), using tables read at u = √μ.

**Source-proven:** steps 1–8, and that the diagnostic records the full evaluated `f·cos·Li` (single scatter + multiple scatter + diffuse) together with the same cosine.

**Unknown at runtime:**
- material 3's model, base color, metallic and textures;
- the secondary n·v, n·h and v·h, which are not captured.

## 4. Mathematical analysis

The sharpest response occurs when h ≈ n, which needs n·v ≈ n·l. Then G ≈ 1 and F = F(v·h ≈ 0.18).

| r | α | D peak | f peak at n·l = n·v = 0.18, F = 1 |
|---|---|---|---|
| 0.1 | 0.01 | 3,183 | **24,524** |
| 0.2 | 0.04 | 198.9 | **1,500** |

**Off-peak falloff:** the half-vector is tilted by δ in the plane of incidence (`brdf_peak.txt`).

| δ (rad) | r = 0.1 f | r = 0.2 f |
|---|---|---|
| 0 | 24,524 | 1,500 |
| 0.002 | 22,190 | 1,461 |
| 0.005 | 14,885 | 1,380 |
| 0.010 | 5,530 | 1,201 |
| 0.020 | 806 | 792 |
| 0.050 | 23.6 | 150 |

- **Candidate #1** (f ≈ 16,300) sits at δ ≈ 0.004 rad, inside the roughness-0.1 peak. Values of 10,000–23,000 across the top 32 cover δ of about 0.001–0.007 rad, a very small solid angle. That is exactly why these events are rare (24,141 crossings out of about 2.5 billion samples) and very large.
- **Roughness 0.2:** the maximum f·cos·Li at the same cosine and Li would be about 1,500 · 0.18 · 1.73 ≈ **470**, against about 5,000 now. This matches the visibly smoother image.
- **Peak height versus energy:** both lobes reflect about the same total energy (directional albedo ≤ 1). Roughness 0.1 packs it into a lobe about 16× smaller in solid angle, so the variance of a point-light connection reached through a randomly sampled primary direction rises sharply.
- **Fresnel:** at v·h ≈ 0.18, the Schlick factor (1 − 0.18)⁵ = 0.37. Reaching f ≈ 16,300 at the δ ≈ 0.004 rad that the cosines allow needs F ≈ 1, which means a high-F0 (bright metal) secondary.
  - The equal RGB values show that the secondary's F0 is achromatic.
  - A 0.04-F0 dielectric gives F ≈ 0.40 and a peak of about 9,800, below several observed values. Combined with the zero diffuse part, the secondary is most likely a bright metallic surface: model 0 with metallic 1 and a gray or white base color, or a gray conductor.
- **Primary versus secondary:** candidate #1's primary path is red (throughput 1.33, 0.036, 0.036), so the primary and secondary surfaces differ in color. A material-3 surface tinted like the primary would give unequal RGB.

## 5. Numerical checks

- **float32 D** — `brdf_peak.txt`. At n·h = 1, 1 − 1e-7, 1 − 1e-5 and 1 − 1e-3, the float32 and float64 values agree to within 0.08%. The cross-product form avoids cancellation, so float32 does not cause instability.
- **Multiple scattering** — `ms_term.txt`, read from `assets/MaterialTables/GGXSingleScatterComplement.emt` (512², squared mapping), with the same texel-centre bilinear reads as the shader.

  | r | Mavg | M(0.18) | fms bound at n·l = n·v = 0.18 | fms bound at n·l = 0.18, n·v = 0.02 |
  |---|---|---|---|---|
  | 0.1 | 5.1e-4 | 1.75e-3 | **0.0019** | 0.094 |
  | 0.2 | 5.9e-3 | 0.030 | **0.048** | 0.13 |

  The bound uses Fms ≤ 1. Multiple scattering contributes under 1e-6 of the secondary value. At the **primary** vertex it can reach about 4.7 at n·l = n·v = 0.02, which matches R.9I.1C's grazing primary weights. That is a separate, moderate factor: candidate throughputs of 1.0–1.47 are not extreme.
- **Shading-normal correction:** it only engages when n_s·v < 0.02. It would then set n·v to 0.02 while n·l = 0.18. The half-vector is then about (88.9° − 79.6°)/2 ≈ 4.6° ≈ 0.08 rad off the normal, which drops D by more than 1,000×. The correction cannot produce the observed peak; it can only reduce the response. Separately, the above-hemisphere test also requires the geometric normal (`PathTraceBSDF.hlsli:282–286`).
- **Denominator floor:** max(4 n·l n·v, 1e-6) is inactive here (4 · 0.18 · 0.18 ≈ 0.13).

## 6. Diagnostic interpretation

| Question | Answer (source) |
|---|---|
| Does f·cos·Li include single scatter, multiple scatter and diffuse? | Yes. It is the complete `PathTraceEvaluateBSDFLobes` value × cos × Li, before the MIS weight (`PathTraceSampleDiagnostic.hlsli` `PathTraceDiagnosticLight`, `factors = float4(value, cosine)`). |
| Is the cosine the same as in the lighting? | Yes. The same variable, `dot(surface.normal, l)`, with the shading normal after correction. |
| Is Li before or after attenuation? | After: window² / d² (`SceneLights.hlsli:55–66`). Li 1.728 at d = 1.076 implies about 2.0 radiant intensity × window, if the window is about 1. |
| Is the roughness the secondary's? | Yes. It is `surface.material.roughness` of the vertex being shaded, after texture resolution and saturation: exactly the value passed to D and G (the 0.045 floor is inactive). |
| What are the throughput, RGB and luminance values? | Throughput is the camera-to-vertex-1 weight before shading the vertex. Values are scene-linear, before denoiser and exposure. Luminance is Rec.709. |
| Are the indices interpreted correctly? | Light index 0, type 0 (point), pdf 0 → weight 1: consistent. The material index is `payload.materialIndex` of the current hit (set at `PathTraceReference.hlsl:1056/1134` before the vertex's light loop). |
| Could bookkeeping explain the magnitude? | No. dominant = throughput × value × weight exactly, and each factor is read where the estimator computes it. The implied f is within the analytic GGX range. |

## 7. Findings

| # | Finding | Category | Confidence |
|---|---|---|---|
| 1 | The secondary f of about 16,000 is GGX single scattering near the mirror configuration at α = 0.01. The peak bound is about 24,500 at the observed cosines. | **Expected variance** | High |
| 2 | Multiple-scattering compensation drives the secondary peak (bounded at 0.0019) | **Not supported** | High |
| 3 | A shading-normal correction artefact | **Not supported**: the correction can only lower the response here | High |
| 4 | float32 instability in D, G or the denominator | **Not supported** | High |
| 5 | Diagnostic field mismatch, or misinterpreted attenuation or roughness | **Not supported** | High |
| 6 | Smith visibility divided twice by 4 n·l n·v | **Not supported**: `GeometrySmithGGX` returns G₂ | High |
| 7 | Material 3 parameters (high-F0 achromatic metal inferred) | **Unresolved**: inferred from the equal RGB, zero diffuse and the F≈1 requirement, not read | Medium |
| 8 | The exact secondary n·v, n·h and v·h for the candidates | **Unresolved**: not captured; the δ ≈ 0.004 rad value is inferred | Medium |
| 9 | The primary throughput above 1 in red (1.0–1.47) | **Expected variance** (R.9I.1C grazing primary weight). It is a minor factor here, not the driver. | Medium–high |

There is no confirmed or plausible defect, so no correction is proposed.

## 8. Recommended next experiment

**Rerun R.9I.1D with secondary-relevant roughness 0.2, changing nothing else.** Use the same scene, camera, light, depth 2, 1 spp, denoiser/DLSS/SHARC off and threshold. Restart accumulation, let the same number of frames run (about 820), then copy the report.

This needs no new code and directly tests the GGX scaling prediction:

- the top-candidate secondary f·cos·Li should drop from about 3,700–5,400 to **at most about 500** (f ≤ about 1,500 at cos ≈ 0.18, Li ≈ 1.7);
- threshold crossings above a fixed threshold should fall sharply;
- throughput should stay around 1.

If the top f·cos·Li values at roughness 0.2 clearly exceed about 1,500 · cos · Li, the GGX explanation fails. Only then would minimal telemetry be justified, for the dominant event: secondary n·v, n·h, v·h, D, G, F (RGB), and the single-scatter and multiple-scatter values separately.

### Future variance-reduction directions (not implemented)

These are listed because the response is mathematically consistent:

- Delta point lights cannot be MIS-combined with BSDF sampling. A small finite light radius (a sphere light) would let BSDF-sampled paths share the work.
- Path-space regularization or roughening of glossy vertices after a glossy bounce (biased, but controllable).
- Reservoir resampling of secondary paths (ReSTIR GI, path reuse) to resample rare glossy-to-glossy connections.
- A firefly clamp only as an explicit, biased last resort. It was not proposed in this phase.

## 9. Validation limitations

- No Engine launch, no GPU rerun and no scene inspection. Material 3's parameters and the secondary geometry angles are inferred, not observed.
- The scratch scripts reimplement the shader equations in Python (float64, with a float32 check of D) and read the bundled tables with the shader's bilinear rule. They are not the GPU code itself.
- A coarse quadrature albedo estimate in `brdf_peak.txt` is unreliable for the r = 0.1 lobe (E > 1 is a grid artefact) and was not used for any conclusion. The multiple-scattering bounds use the real tables instead.
- No tracked files were modified. Nothing was staged or committed.
