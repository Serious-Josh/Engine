# R.9I.3A — caustic estimator correctness review

2026-10-27. Design review against the current working-tree source, including its existing local changes. No production source, shaders, settings or scene data changed. Engine was not launched. No build, GPU execution, staging or commit was performed. The only deliverable added by this review is this document.

## 1. Decision

**Do not implement R.9I.3 verbatim.** A bounded reflector-targeted light tracer can replace a precisely defined subset of depth-2 point-light contributions in **Path Traced Reference**, but the proposed partition is not yet a proof of equal expectations.

Corrections required before enabling replacement:

1. Use the full receiver BSDF if excluding the full camera contribution. Roughness does not make Engine's receiver Lambertian or remove its specular lobe.
2. Account for geometric-versus-shading-normal measures. Reusing the forward `f*cos/pdf` weight is insufficient in general. View-dependent horizon correction also changes the BSDF itself on reversal; a generic reciprocal correction cannot repair that change.
3. Match the jittered pixel-area measurement and the near/far-clipped camera visibility interval. Raster Primary and RT Pixel Centre have a different measurement; a disjoint partition does not fix this mismatch.
4. Include the selected light and the actual emission-mixture density, with complete support over exactly the paths removed from the camera tracer. CPU material-factor selection alone cannot establish this.
5. Define splat normalization, batch weighting, quantization and overflow handling. Rounded fixed-point sums are not strictly unbiased.

Recommended first lighting prototype: one selected point light, one explicitly registered, flat-normal reflector, depth 2, Reference primary, all reconstruction/cache/upscaling disabled, a bounded positive light-path count, and a path predicate excluding horizon-corrected configurations. Other reflectors and all unsupported paths remain with the camera estimator. Start with glossy reflection; add the flat mirror as a separately validated extension of the renderer's supported transport.

## 2. Evidence and source contracts

Read the complete R.9I.2, R.9I.3 and R.9I.1E investigations. Their Python results support hypotheses and test magnitudes; they do not establish equality of GPU estimators. R.9I.3's runtime attribution of the patch is carried forward as reported evidence, not independently reproduced here.

Locations below refer to the inspected working tree, not an assumed clean revision:

| Source | Verified behavior and consequence |
|---|---|
| `assets/Shaders/PathTraceReference.hlsl:401–468` | `PathTraceBuildSurface` resolves material textures at LOD 0, ARM precedence, base color and model-0 metallic, flips both normals toward the incoming ray, then finalizes. Reversing traversal changes `view`. |
| `PathTraceReference.hlsl:1749–1854`; `RayTracingInstanceData.hlsli:21–38` | Closest hit resolves the triangle's material slot, transforms triangle and interpolated normals by inverse transpose, and applies tangent-space normal mapping. Instance default material alone is not the hit material. Degenerate-normal fallbacks can depend on ray direction. |
| `assets/Shaders/PathTraceBSDF.hlsli:228–280` | Finalization saturates resolved roughness/metallic, applies `CorrectShadingNormalViewHorizon`, and sets delta at **r <= 0.001**. |
| `PathTraceBSDF.hlsli:108–112, 291–353` | Non-delta alpha is `max(r,0.045)^2`. Total evaluation includes GGX single scatter, multiple scatter and energy-weighted EON diffuse; conductor model 2 has no diffuse. The delta threshold and GGX floor are distinct. |
| `PathTraceBSDF.hlsli:376–526` | Non-delta sampling uses the full VNDF/cosine mixture PDF and full BSDF. Invalid below-hemisphere outcomes are zero events, not resampled. Delta sample weight is Fresnel divided by delta-selection probability. |
| `assets/Shaders/ShadingNormal.hlsli:6–58` | Corrected normal depends on `view` below n_s dot view = 0.02, even for flat normals at sufficiently grazing incidence. Merely forbidding normal maps does not remove this issue. |
| `PathTraceReference.hlsl:498–592` | Table lights are enumerated, not randomly selected. Point-light NEE has PDF sentinel 0 and weight 1, then BSDF*cos*attenuated incidence*visibility. Sun/environment are separate contributions. |
| `assets/Shaders/SceneLights.hlsli:34–67`; `PathTraceLights.hlsli:228–242` | Point intensity is `radiance`; incidence uses range window squared and inverse squared distance clamped at 1 cm. Point and spot table lights are delta NEE; a light tracer still needs a nonzero emission-direction density. |
| `PathTraceReference.hlsl:990–1253` | `vertex` counts surfaces. At depth 2, vertices 0 and 1 receive NEE. A hit at vertex 2 is discarded, but a miss can contribute distant light. Roulette may act after vertex 0 and divides surviving throughput by survival probability. |
| `PathTraceReference.hlsl:594–610,1259–1290` | Reference draws uniform subpixel jitter from a 24-bit RNG; Pixel Centre uses (0.5,0.5). Camera rays start at unprojected near plane, stop at far plane, not at the pinhole origin. |
| `assets/Shaders/PathTracePrimarySurface.hlsli` | Raster primary uses pixel-centre G-buffer decoding, quantized materials/normals, interpolated geometric normal, parallax differences and a distance-dependent surface lift. Its discrepancy from RT Reference is more than antialiasing alone. |
| `PathTraceReference.hlsl:1475–1486`; `src/Renderer/Passes/PathTraceReferencePass.cpp:1114–1125` | Progressive mean uses accumulated camera sample count plus batch size. Accumulation is RGBA32F; presentation output is RGBA16F with an explicit half-maximum cap. Validate energy before presentation clipping. |
| `src/Renderer/ShaderData.h:133–204` | Current path constants are 320 bytes, with inverse world VP and inverse camera-relative VP, but no forward projection. Match CPU/HLSL layout with offset assertions. |
| `src/Renderer/Renderer.cpp:6594–6613,6902–6922` | Accumulation identity includes camera, settings, light table, TLAS recording, material revision and lifetime. Path dispatch has timestamp infrastructure and split/non-split branches. New estimator state must join the reset/epoch contract. |
| `assets/Shaders/RadianceCacheD6.hlsli:432–480` | Existing helper rounds to fixed point and adds low/high words with carry. This is useful arithmetic infrastructure, not a ready unbiased splat implementation or overflow policy. |

The analytical treatment below uses continuous random variables and exact intersection geometry. Like the existing renderer, a GPU implementation has finite RNG, floating-point and ray-offset errors. These must be bounded and measured separately from an estimator's mathematical expectation.

## 3. Exact path domain and camera term

Use light-first vertices `l -> r -> d -> c`: point light l, reflector r, camera-visible receiver d and pinhole c. In camera code, d is **vertex 0**, r is **vertex 1**. Prototype requires `maxPathDepth == 2`. It must not silently activate at greater depth or with cache substitution.

Define a deterministic path indicator chi(l,r,d,c), independent of sampled strategy, reflector-list selection outcome, random seed, visibility-test outcome or frame's surviving sample count. It contains:

- Feature successfully active for this frame and this camera measurement; N > 0 and resources/list epoch valid.
- l is a supported, selected **point** light in the published light table. Spot support is deferred; enabling point emission must not suppress spots.
- r belongs to the published supported instance/primitive domain, uses resolved model 2 or model 0 with resolved saturated metallic **equal to 1**, and resolved roughness `r_R < tau_R`.
- d is outside R, has resolved roughness `r_D >= tau_D`, and is an approved receiver. This defines a material class, not a diffuse scattering event. If a diffuse-only class is wanted, it needs a separate lobe-integrand definition below.
- Any additional prototype normal/direction restrictions, described in section 6, hold on the **same complete path** in both estimators.

Choose and document threshold boundaries exactly; e.g. tau_R = 0.35 and tau_D = 0.5 are tuning choices. Roughness 0.001 is delta; 0.001 < r < 0.045 is non-delta GGX using the 0.045 floor. Do not classify this whole interval as mirror. Equality at tau_R goes to the camera estimator. A receiver test of `not R` must include the same registration and material interpretation on both sides.

For a camera sample at d, let u point from d to r, v point from r to d, and a point from r to l. Let T_d be the actual vertex-1 throughput after d's full mixture sample and any roulette compensation. The exact excluded sample term, **per supported table-light index**, is

```
chi(l,r,d,c) * T_d * f_r^C(v,a) * max(n_sr^C(v) dot a,0)
                   * EvaluateSceneLightIncidence(l,r) * V_rl.
```

Here superscript C means the surface is built with the camera-order `view`: at r it is v, at d it is toward c. For point lights the existing NEE weight is 1, not a density division by the sentinel zero. Suppress only this light's addition in the vertex-1 table-light loop. Do not skip all of `PathTraceDirectLightingLobes`, `radiance += throughput*direct`, a BSDF continuation, or an entire path. Preserve vertex-0 direct lighting, sun/environment/directional light at vertex 1, all other table lights, terminal miss radiance and unrelated indirect paths. Retain RNG consumption order.

**Receiver lobe choice:** T_d contains total `f_d`, even when the cosine strategy generated u. Selecting the diffuse strategy is not selection of a diffuse physical path. If the light tracer uses total `PathTraceEvaluateBSDF` at d, replacing the term above is correct. If it uses only EON diffuse, exclude only the diffuse part: replace T_d in the excluded term by the propagated `f_diffuse/f_total` share (per channel, with the existing zero conventions). Leave the specular receiver remainder in the camera result. Prefer full-BSDF replacement for the first prototype. This may route some rough-receiver glossy energy through the new pass, which must be included in its validation.

Delta-only metal/conductor r has no non-delta NEE, and point lights are not intersectable emitters. Its baseline camera contribution is zero. New mirror caustic energy is therefore an **extension**, not proof of matching the previous image. Delta dielectrics can retain a continuous diffuse lobe; the conservative metal/conductor class avoids that mixed-measure complication.

## 4. Targeted emission with a known density

For supported light l choose it with probability p_L(l) > 0 (uniform or a published power distribution). The camera enumerates lights; the light tracer must divide by p_L once. Select a reflector proposal k with probability b_k(l), normalized across the proposals for that light. A proposal is a bound, not a guaranteed hit or a physical restriction on the subsequent ray.

For an enclosing world sphere centred at b with radius R, distance D = |b-l| > R, the visible cap axis is (b-l)/D and half-angle has cosine sqrt(1-R^2/D^2). Its solid angle is Omega_k = 2*pi*(1-cos(theta_k)). Sample cos(theta) uniformly on [cos(theta_k),1] and azimuth uniformly on [0,2*pi). Density is 1/Omega_k per steradian inside the cap. Use stable small-angle evaluation for 1-cos(theta). If D <= R, use the whole sphere, density 1/(4*pi). Degenerate bounds need an explicit fallback, not division by zero.

A robust distribution is

```
q_l(w) = epsilon/(4*pi)
       + (1-epsilon) * sum_k b_k(l) * 1_cap_k(w)/Omega_k,  0 < epsilon <= 1.
```

Generate the sphere component or a selected cone with these probabilities. Evaluate the **whole mixture**, including every overlapping cap and the sphere component, for every sample. Weight by 1/q_l(w), not 1/(b_selected*q_selected) when accepting the same physical path from multiple proposals. The selected k is only an auxiliary random variable, marginalized out. Trace to the actual nearest hit and classify that hit; hitting another registered eligible reflector is acceptable with this mixture weight. Never condition the PDF on a successful reflector hit unless the success normalization has been derived. Blocked, missed, ineligible and wrong-material first hits are zero outcomes and still count in N.

Without the sphere component, exact enclosing caps with positive b_k are sufficient only if every r in chi has a cap containing its direction. The sphere fallback makes geometric directional support robust, but does not fix an omitted light, a truncated propagation distance or a wrongly classified material. If a reflector-list cap is missing, either remove that instance from **both** sides' chi or retain full-sphere support and a proven acceptance domain. Capping proposal count must not silently cap only the light estimator's support.

Bounds must enclose actual transformed geometry: nonuniform scale, shear, rotation, animated/deformed mesh and scene/TLAS updates all matter. A local sphere scaled by a guessed average scale is unsafe. Rebuild or conservatively refit on geometry/transform changes; republish material-slot membership on material/texture changes even with unchanged TLAS. CPU eligibility must over-include possible textured hits, or use opt-in constant-material geometry initially. Resolve every hit's triangle slot and textures on GPU. Use a single immutable epoch of lights, geometry, materials, list and thresholds in both dispatches; reset accumulation on changes to the target.

For the one-reflector prototype, register exactly one stable instance/primitive set and point-light index. All unregistered geometry stays camera-owned. Use an exact enclosing bound plus sphere mixture, constant material, no skinning/deformation and verified flat normals. Ray misses and occlusion may dominate even then. R.9I.3's near-100% useful-hit claim is not established. GGX's scattered directions need their own full support; mirror changes only the second-edge sampling, not emission PDF.

## 5. Derivation for reciprocal, geometric-normal surfaces

First derive the simple case with shading normals equal to geometric normals and no horizon correction on either incident/outgoing direction. All surface directions point away from their vertex. Define

```
a = normalize(l-r);  v = normalize(d-r);  u = -v;
wc = normalize(c-d); rho = |l-r|; s = |r-d|; t = |c-d|;
c_ra = n_r dot a; c_rv = n_r dot v;
c_du = n_d dot u; c_dc = n_d dot wc;
H_l(r) = window(rho)^2 * rho^2 / max(rho^2,0.01^2),
```

with H = 0 outside range. For future spots multiply H by the exact cone factor squared from `SceneLights.hlsli`; do not reinterpret point intensity as total power. Isotropic total power would be 4*pi*I only in the unwindowed physical emitter. Engine's distance-dependent window is an edge attenuation, applied at r after tracing. `Li(r)=I_l H_l(r)/rho^2`.

Let the virtual film plane lie one unit ahead of the pinhole, coordinates h=(x/z,y/z), pixel footprint area A_j (A/P for a uniform P-pixel image), and z-angle cosine c_cam > 0. Projection gives

```
domega_c = c_cam^3 dh,
domega_c = c_dc/t^2 dA_d,
J_c(d) = dh/dA_d = c_dc/(t^2*c_cam^3).
```

Thus pixel measurement is the **average radiance over film area**, not irradiance on a sensor plane:

```
L_j = (1/A_j) integral_{h in pixel j} L_o(d(h),wc) dh.
```

The caustic portion for glossy r has area integral

```
L_j^chi = sum_l integral_d integral_r
  chi * 1_j(h(d))/A_j * J_c(d)
  * f_d(wc,u) * c_du
  * f_r(v,a) * [I_l H_l(r)/rho^2] * c_ra
  * c_rv/s^2 * V_lr * V_rd * V_dc
  dA_r dA_d.
```

All cosines in a nonzero integrand are positive. Visibility includes first-hit semantics and the clipped camera test in section 7. The Jacobians of emission and reflection sampling are

```
domega_l = c_ra/rho^2 dA_r;
domega_r = c_du/s^2 dA_d;
q_A(r|l) = q_l(w_lr)*c_ra/rho^2;
p_A(d|r,l) = p_r(v|a)*c_du/s^2.
```

Divide the integral by sampling density p_L*q_A*p_A. The first inverse-square falloff and the receiving cosine at r cancel; the second inverse-square falloff and receiving cosine at d cancel. One attempted light path deposits

```
B1 = I_l * H_l(r) / (p_L(l)*q_l(w_lr));
B2 = B1 * f_r(a,v)*c_rv/p_r(v|a);
Z_j = chi * 1_j(h(d)) * B2 * f_d(wc,u)
      * c_dc/(A_j*t^2*c_cam^3) * visibility;
C_j = (1/N) sum_{i=1..N} Z_ij.
```

There is **no extra receiver incoming cosine**, no extra inverse-square factor for l-r or r-d, no division by receiver sampling PDF after this expression, and no division by number of successful splats. The camera factor includes receiver-to-camera cosine once. Image-wide form is P/(A*c_cam^3). Moving the virtual plane to distance f changes dh/domega to f^2/c_cam^3 and its pixel area by f^2; they cancel. Using total film area without P underexposes by P; adding another cosine or camera falloff changes spatial shape.

Reflector density in Engine is `p_s*p_VNDF+(1-p_s)*cos/pi`, with p_VNDF = D*G1(a)/(4*n dot a). The half-normal reflection Jacobian is dOmega_v = 4*|a dot h| dOmega_h; this cancels the visible-normal |a dot h| factor. For a pure single-scatter VNDF proposal, f*cos/p reduces to F*G2/G1(a). **Engine's returned weight generally is not that simplified expression**: the full mixture, multiple scatter, denominator floors and total evaluation must be retained. Use `PathTraceSampleBSDF`'s weight in this restricted case; do not multiply it by f or cos/pdf again.

For a flat mirror the scattering measure is a discrete reflection, not a solid-angle density. Sample reflection deterministically (or divide Fresnel by its event probability), use B2=B1*F/P_delta, and keep the same camera factor and emission sampling. Do not divide by `sample.pdf == 0`. The integral is over emitted directions with deterministic reflected receiver; the area-density derivation above applies only to glossy reflection. No fictitious glossy delta PDF or finite cone replacement is needed. Planar image-light analysis is an independent mirror oracle; its source-image distance/falloff emerges from this pushforward and must not be multiplied into B2 again.

This derivation preserves the renderer's chosen BSDF energy, including its documented EON normalization approximation. It does not prove that Engine's material law is perfectly energy conserving in the physical sense.

## 6. General normals: corrected kernel and restrictions

Let g denote geometric cosines and s denote corrected shading cosines, with surfaces rebuilt in **camera order**:

- At r use view v toward d, then evaluate light direction a: f_r^C(v,a), s_ra^C.
- At d use view wc toward c, then evaluate u toward r: f_d^C(wc,u), s_du^C.
- g_ra, g_rv, g_du, g_dc are geometric cosines of these directions, with consistent two-sided orientation.

Where these geometric cosines are positive, a general glossy estimator for the existing camera integrand is

```
Z_j = chi * 1_j/A_j * I_l*H_l(r)/(p_L*q_l*p_r)
      * f_r^C(v,a) * (s_ra^C/g_ra) * g_rv
      * f_d^C(wc,u) * (s_du^C/g_du)
      * g_dc/(t^2*c_cam^3) * visibility.
```

This follows from the same **geometric** area Jacobians while retaining the camera code's shading cosines in the numerator. It is a direct change of variables, so it does not assume reciprocity of the view-conditioned law. Store raw hit information to rebuild r after d is known and d after camera projection; passing the light-arrival view to both builders and merely swapping BSDF arguments is wrong.

For a fixed reciprocal shading normal, comparing this to the naive forward weight produces the reflector correction `(s_ra/g_ra)*(g_rv/s_rv)` and receiver factor `s_du/g_du` when the camera factor uses g_dc. If using s_dc in that factor instead, the receiver correction is `(s_du/g_du)*(g_dc/s_dc)`. Directions and measures must be named; an unexplained generic “Veach factor” can invert this ratio. For reference, [PBRT's BDPT treatment](https://pbr-book.org/3ed-2018/Light_Transport_III_Bidirectional_Methods/Bidirectional_Path_Tracing) distinguishes light-subpath importance transport from camera radiance transport and applies shading-normal correction. The equations here were derived for Engine's actual cosine convention.

**Support remains essential.** A forward, horizon-corrected sampler can reject a direction with nonzero camera-order kernel. For general glossy geometry use a proposal with an explicit positive uniform-sphere component and evaluate its full mixture PDF against the camera-order numerator; rejected forward proposals remain zero samples, and the sphere component covers the additional directions. Do not use forward sampler validity to reject a camera-valid sphere proposal. Large cosine ratios can cause substantial variance near geometric tangency. Degenerate geometry must stay outside the supported replacement domain.

For delta reflection, changing the normal changes the support manifold, not just its weight. The formula above for a continuous glossy kernel cannot fix this with a finite multiplier. General horizon-corrected/normal-mapped mirrors require a separately derived adjoint delta mapping and Jacobian or a different sampler; this review does not approve them.

**Smallest approved restriction:** opt-in nondegenerate geometry with verified shading normal equal to face normal at every relevant hit, no normal maps or smoothing, and both reflector directions a,v having positive face cosine **>= 0.02**, plus receiver directions u,wc with face cosine >= 0.02. Include these complete-path restrictions in chi on both sides; below-threshold paths remain camera-owned for glossy replacement. Both directional builds then preserve the same normal. Do not use an approximate near-equality normal test as a proof of exact reciprocity. An initial deliberately flat test mesh/material contract is easier to establish than arbitrary scene eligibility. Material-only classification from R.9I.3 must therefore be strengthened for the simple estimator.

The mirror extension may use the same flat restrictions, clearly documenting any unsupported grazing mirror domain. It restores supported L S D E energy that the baseline lacks; it is not a complete solver for all mirror transport.

## 7. Camera projection, clipping, visibility and splatting

For a perspective pinhole, project d with the **forward transform inverse to the actual Reference ray construction**, using matching matrix multiplication conventions and camera-relative/world coordinates. Divide by clip w; map NDC to uv with x right and y down as `PathTraceCameraRay` does; pixel is floor(uv*resolution). Require finite coordinates, positive camera-axis depth and w, and uv in half-open [0,1). Enforce near/far clipping as the primary ray does. Do not project through a different jittered matrix or add a second subpixel jitter to the splat. Uniform jitter inside the camera pixel and continuous projected light paths estimate the same box integral.

For asymmetric/off-axis perspective use the actual virtual-plane pixel area and forward axis. A general Jacobian computed from unprojected pixel basis rays is safer than guessing FOV from a UI value. This review does not approve orthographic, lens, panoramic or nonlinear camera projection under the pinhole formula.

**Near-plane visibility correction:** Reference starts at the unprojected z=0 point and stops at unprojected z=1. An occluder between c and near plane is invisible to the primary camera ray. A shadow ray all the way from d to c would incorrectly hide d in that case. Connect over exactly the near-plane-to-d segment (or its reverse), not d-to-pinhole. Reject d beyond the far plane, but do not trace from d to the far plane. Keep the camera factor's distance t measured to the pinhole, not the near plane: clipping changes visibility, not the perspective Jacobian.

`PathTraceVisible` offsets the origin and shortens TMax by 1e-4 of distance for positional-light visibility. Reusing it unmodified for camera connection is only an approximation: it may skip an occluder near the near-plane endpoint, and endpoint offsets can alter grazing visibility. Specify a camera-specific segment contract and verify it against forward primary first-hit identity, including near-plane occluders, self-intersection and silhouettes. Finite-precision ray offsets already approximate ideal transport; do not claim bitwise equivalence or an exact geometric proof from this helper alone. Existing light/camera reverse visibility can also differ at offset-scale boundaries; measure and bound that discrepancy.

Each continuous path splats to exactly one box pixel. No bilinear spatial spreading, depth tolerance gather or G-buffer visibility shortcut is part of the unbiased box estimator. These change the filter or visibility. At silhouettes, the projected hit must be the first camera-visible surface on the clipped segment; rejection needs no “coverage correction” or renormalization. Paths outside the image, blocked paths and boundary misses count in N. Assign exact edges with a fixed half-open convention; boundary roundoff is a numerical test case.

**Reference mode:** the corrected estimator is unbiased for the ideal continuous box-filter measurement under the stated restrictions and exact arithmetic/visibility. Actual 24-bit jitter and intersection numerics require GPU validation. **RT Pixel Centre and Raster Primary:** box splats do not estimate their point measurement. Their pixel-centre target is a Dirac film measure; accepting a finite bin changes that target, even with a disjoint material partition. Raster additionally changes hit data and position. Therefore keep this feature guarded off in those modes initially. A valid alternative for their existing measurement is receiver-conditioned sampling at each fixed primary d: sample r with a known area density q_A and evaluate

```
f_d^C(wc,u)*s_du^C * f_r^C(v,a)*s_ra^C*Li_l(r)
* g_rv/s^2 * V_dr*V_rl / (p_L*q_A(r|d,l)).
```

This is a receiver-specific NEE estimator for **glossy** r, with complete-area support; it avoids splatting and can replace exactly its matching camera term. A planar-mirror analytic connection or manifold solver with the correct solution Jacobian is needed for delta r. A fixed-radius photon gather is another option only if explicitly accepted as biased. Partitioning cannot by itself turn an area measurement into a centre measurement.

## 8. Combination and normalization proof

Let F_j be the baseline glossy pixel path integrand, including its BSDF law and measurement. Decompose it as `(1-chi)F_j + chi F_j`. Camera sampling with the local per-light subtraction in section 3 has expectation integral `(1-chi)F_j`. The light estimator in sections 5/6 has expectation integral `chi F_j` because its path density cancels the geometric change of variables. Their sum has expectation integral F_j. No independence assumption is needed for this expectation proof; separate RNG streams are still desirable for variance and reproducibility.

This proof requires: identical chi on the same oriented complete path, identical BSDF numerator, identical visibility and pixel measurement, and positive light-tracing density wherever chi F_j is nonzero. A shared material predicate alone proves none of the other premises. Paths sampled by two different reflector proposals are not two integrand contributions; the marginalized mixture handles them. The mirror extension adds a separate delta path measure whose camera estimator is zero; baseline equality tests exclude that extension.

For frame f, camera batch size S_f and attempted light budget N_f, define C_fj = sum Z/N_f. Add **S_f*C_fj** to the camera `sampleSum` once before the existing mean update:

```
A_new,j = [K*A_old,j + sum_{s=1..S_f} X_fsj + S_f*C_fj] / (K+S_f).
```

Do not divide splats by camera spp, successful hits, image pixel count or N a second time. Dividing by total emitted light paths across frames is a different weighting scheme; do not mix it with the camera sample-count mean. Variable deterministic budgets still give unbiased frame estimates; variance changes. Include zero events in N and completed camera attempts in S. Avoid data-dependent stopping or accepting only favorable frames.

At N=0, empty supported domain, failed resource preparation or unavailable pipeline: choose the unpartitioned camera variant **before dispatch**, or mark the whole frame invalid and do not advance accumulation. Never run camera skip without its replacement. Changing thresholds, registration, light identities, normal restrictions or measurement resets accumulation. N changes alone need not reset the expectation, but may reset for reproducible comparisons. On/off resets are mandatory. Shader variant isolation should preserve the existing disabled path's resources, RNG and DXIL; verify hashes later. The runtime source has not been changed to provide these guarantees yet.

## 9. Numerical and GPU risks

- Splat buffer is per-frame, cleared before writing, then UAV barrier and resolve before camera reads; no concurrent reads of two-word partial sums. Each in-flight resource needs correct lifetime/fence handling. Descriptor validity, byte offsets and W*H*24 allocation arithmetic require checked wide CPU arithmetic.
- RGB uint64 fixed point costs **24*W*H bytes** plus counters/resolve resources: 73,306,080 bytes (69.9 MiB) at 2868x1065. A whole zero-copy clear buffer doubles that portion of residency. Use a fixed maximum allocation and resolution guard.
- D6's carry add gives the final modular 64-bit sum after all writers complete. It does not provide an atomic 64-bit snapshot or saturating overflow detection. Concurrent high-word overflow detection needs its own proven counter/protocol. Saturating, wrapping or dropping large contributions biases the estimate. Counting an overflow does not repair its energy.
- Nearest rounding each splat to scale Q=2^20 introduces up to 1/(2Q) error per channel per splat; it is deterministic quantization bias. If storing raw Z and dividing by N, the worst per-frame pixel error is hits/(2QN) <= 1/(2Q); if storing Z/N first the bound is hits/(2Q), much worse. Conversion through float adds error beyond that rounding bound. D6's documented input magnitude bound is not established for caustic splats.
- An expectation-preserving integer conversion can stochastically round raw Q*Z to floor or ceil with the fractional probability, then divide resolved sums by Q*N. It needs a separate random draw and correct high/low conversion. This removes ideal quantization bias, not float error or overflow. A finite accumulator cannot promise overflow safety for arbitrary unbounded HDR inputs. Establish a conservative scene/input/contribution bound or a recoverable wider/multiword representation. On numerical failure, invalidate and retry the affected measurement with a correct fallback; selectively retaining “good” samples/frames is biased. Until this contract exists, describe fixed point as a bounded-error approximation, not strictly unbiased GPU arithmetic.
- Narrow GGX, geometric grazing ratios, small camera distances and small q can create large Z even for finite light intensity. Clamp-based firefly removal, per-path caps and silently dropping nonfinite samples are not valid fixes. Track finite failures, maxima and representability before enabling replacement.
- R32F accumulation eventually loses small increments at large counts; uint sample-count addition can overflow, and float count weights lose integer exactness above 2^24. Half output caps hide energy. Establish long-run limits and inspect raw HDR accumulation, not displayed brightness.
- Hot caustic pixels can serialize RGB atomics; mirrors and focusing geometry may be especially contentious. Wave aggregation is optional only if it preserves the same destination and sum. No contention or time claim is established without measurement.

## 10. Bounded performance controls

Use an explicit N in [1,N_max] for active replacement and no work proportional to “N per object.” Cap supported lights/proposals and preprocessing memory. Per light path: one first-hit ray, at most one reflected first-hit ray, at most one camera visibility ray; early exits reduce work. No rejection-until-success loop. Proposal evaluation is O(K_max) for the simple overlap sum; bound that too. If roulette is added to light tracing it needs survival compensation; unnecessary for this two-surface prototype.

Material-aware conservative registration reduces wasted work. Keep selection probabilities, bounds and snapshot versions visible in diagnostics. If no supported reflectors or lights remain, dispatch zero light rays and restore the unpartitioned camera path. Do not report “no caustics” merely because a random frame produced no hits.

Measure separate timestamps for clear, light trace, resolve/fold and total incremental frame cost using the existing profiler infrastructure. Count attempts, rays by edge, eligible first hits, receiver hits, visibility accepts, splats, PDF zeros, finite failures, overflow and atomic hot spots. Compare independent-seed region means, RGB ratios, edge/falloff profiles and MSE at equal GPU time. Prior 1 M-path and millisecond/variance estimates are hypotheses, not measured budgets. Region error bars must use independent runs or measured covariance; assuming 10,000 pixels are independent can understate uncertainty when splats correlate them.

## 11. Incremental implementation sequence — future work only

Each stage must preserve the unpartitioned default. Do not progress on failed correctness checks. “Build” below means the project's configured C++/shader builds and appropriate existing validation targets; no builds were run for this document-only review.

| Stage | Deliverable and build gate | Correctness gate | Stop condition |
|---|---|---|---|
| 1. Shared classification contract and tests | Define complete-path eligibility, instance/light identity, resolved-material boundaries and flat-normal restrictions; CPU tests plus an unused HLSL classifier harness compile. No estimator activation. | Fixtures at delta 0.001, GGX floor 0.045, tau_R/tau_D boundaries, mixed slots, texture changes, normal grazing boundaries, depth/primary/feature guards. Camera/light vertex ordering maps to the same answer. | Any ambiguous full-BSDF versus diffuse-only policy, stale identity or disagreement. |
| 2. Data/resource and numeric contracts | Layout assertions, bounded N/list/buffer size, snapshot/reset and overflow/quantization design; builds pass. | Empty-list/N=0/failure selects baseline; p_L and q normalize; memory arithmetic and representability bounds established. | Unsupported HDR range or incomplete proposal/identity support. |
| 3. Noncontributing dispatch | Isolated shader/pipeline, hit reconstruction, counters and timestamps; camera variant remains unpartitioned. | Dispatch bounds, ray budget <=3N, first-hit slot/material matches camera source; no output changes and disabled DXIL unchanged. | Resource hazards, counter anomalies or image differences. |
| 4. Projection and numeric splat validation | Synthetic known-density surfaces, raw splat/resolve output only; builds pass. | Integrate constants over known film bins at several FOV/aspects/resolutions; area Jacobian and resolution scaling; clipped visibility, silhouettes, subpixel/half-open edges; atomic carry/quantization stress. | Any systematic FOV/range/pixel-area bias, overflow or camera first-hit mismatch beyond defined numerical bounds. |
| 5. One-light/one-glossy-reflector estimator | Standalone caustic output, unpartitioned camera retained; builds pass. | Flat analytic scene: total receiver BSDF, distance window/clamp, roughness sweep including just above delta, color/energy/shape versus independent integration and high-spp camera component. Independent seeds with measured confidence intervals. | Agreement fails or support requires an unproved normal correction. |
| 6. Exact camera subtraction | Camera variant suppresses only the section-3 per-light term; builds pass. | Output excluded camera component separately. Its expected value equals standalone light component in glossy domain. Direct/control paths untouched. | Any unrelated term suppressed or different chi on same paths. |
| 7. Combined validation and mirror extension | Fold S*C with correct progressive count; optional mirror variant separately; builds pass. | At S=1 and S>1, varying N and accumulation length: combined glossy mean equals baseline; raw RGB, falloff and edges agree within independent-run uncertainty. “Double” debug mode doubles only the isolated glossy component. Flat mirror agrees with image-light oracle, not zero baseline. Off mode and guards restore baseline. | Energy mismatch, mean dependence on N/S, filtered edge mismatch, invalid-frame bias or mirror-domain uncertainty. |
| 8. GPU performance | Instrument final clear/trace/resolve pipeline and bounded settings; builds pass. | Warm stationary workloads, measured rays, memory, contention and equal-time MSE; no assumed millisecond target. | Budget cannot be bounded or variance/time fails to improve. |
| 9. Expansion | Multiple light/proposal overlap tests first; general glossy normals only with section-6 kernel/support. Hybrid/NRD/SHARC and other camera measurements require separate reviewed contracts. | Revalidate support, target BSDF, measurement and signal split at each expansion; moving geometry/material epochs and disabled-mode regression checks. | Any domain expansion relies only on “same predicate” or smoothing as evidence. |

**Explicit first implementation step:** stage 1 only, under a separate authorization: specify and test the complete-path classifier for Reference/depth-2/one-point-light/registered-flat-reflector/full-receiver-BSDF replacement. Do not add a camera skip or lighting dispatch as part of that first step. Include the direction-dependent normal restrictions and publication identities in its inputs; material class alone is inadequate.

## 12. Unresolved blockers and acceptance status

The mathematical simple estimator is approved only for the restricted reciprocal, common-measure domain; the general glossy camera-order kernel above supplies a valid design direction. The original unrestricted design is **not approved**. Outstanding implementation blockers are the exact clipped visibility/numerical contract, demonstrably identical publication/classification on both traversals, and expectation-preserving or explicitly bounded-error HDR splat storage without silent overflow. General delta normals/horizon correction and Raster Primary point-measurement coexistence remain unsupported, rather than accepted small biases. No GPU equivalence or performance claim has been validated.

Preserving reflected-light energy requires all of these contracts, not just a smoother patch. None of the implementation stages was started in this review.

