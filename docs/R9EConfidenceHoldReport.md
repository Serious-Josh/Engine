# R.9E experimental confidence hold — coordinate audit blocker

Date: 2026-10-27.

**Status: stopped after the required read-only temporal-coordinate audit, before implementation.** No hold selector, GPU resources, dispatches, shaders, or renderer changes were added. This report is the only new file.

## 1. Initial audit findings

Reviewed `docs/R9CImplementationReport.md`, `docs/R9DDiagnosticAudit.md`, the current R.9B/R.9C implementation, retained G-buffer resources, raster motion generation, renderer submission ordering, and the previously retrieved exact NRD 4.17.3 temporal shader.

The working tree contains extensive unrelated modifications and the existing R.9A/B/C work. A search of source, shaders and reports found no partial `ENGINE_NRD_RELAX_CONFIDENCE_HOLD` implementation. Existing files were left intact.

The existing generated-confidence integration is not the blocker: it correctly supplies this update's confidence in the immediately previous frame's space. The missing information concerns transport **between successive confidence outputs**, which are themselves one frame behind the current G-buffer.

## 2. Temporal coordinate-space reasoning

Let t denote consecutive compatible, successfully submitted rendering updates, rather than an arbitrary CPU attempt.

| Quantity available while recording frame t | Surface/image space |
|---|---|
| Current G-buffer and its motion M[t] | Frame t; motion maps t to t−1 |
| Newly generated raw/filtered confidence C[t] | Frame t−1; old visibility experiments retraced through TLAS[t] |
| A proposed retained output H[t−1] from the preceding update | Frame t−2 |
| NRD's confidence sampling position for frame t | Previous UV in frame t−1 |

The existing paired evaluation reads the preceding probe record and writes its signal and geometry at that old probe index (`assets/Shaders/NrdHistoryConfidence.hlsli:306–375`). It then constructs the next record from a pixel in frame t (`:378–454`). Thus C[t] and C[t−1] do not, in general, describe the same surfaces at equal texture indices.

To combine C[t] with a preceding held value, a surface represented in frame t−1 must be located in frame t−2. The required motion is **M[t−1]**, evaluated for the represented frame-t−1 raster sample, with appropriate historical geometry/validity checks. Reading the current M[t] at the same numeric texel instead uses the wrong frame's surface and the wrong time interval.

The sign convention itself is established: `assets/Shaders/Triangle.hlsl:666–670` writes currentUV−previousUV and previous-view-depth minus current-view-depth. For the correct historical motion, the nominal unjittered mapping would therefore be UV[t−2]=UV[t−1]−M[t−1].xy. A complete implementation must also account for raster jitter and the sample actually represented by each historical probe, and reject invalid/disoccluded history. This equation alone does not establish those conditions.

After any valid transport, the final held confidence must still be in frame t−1 space. NRD must continue to sample it once at prevUVSMB. The exact v4.17.3 `RELAX_TemporalAccumulation.cs.hlsl:595–605` samples diffuse confidence at prevUVSMB using linear clamp; no extra final-binding reprojection belongs in R.9C. Historical transport and NRD's final sampling are different operations over different frame intervals.

### What the repository actually retains

1. **Previous G-buffer history omits motion.** `src/Renderer/PreviousGBufferHistory.h:10–13` explicitly states this; its `Slot` at `:30–36` contains only depth, surface, geometricNormal and material. `PreviousGBufferHistory.cpp::RecordCopy` copies those four resources. `Renderer.cpp:5686–5700` exposes the same four SRVs and a camera snapshot; no preceding motion SRV is available.
2. **The missing motion includes object motion, not just camera motion.** Raster motion uses `previousModel` and previous camera transforms (`assets/Shaders/Triangle.hlsl:455–470`). Reprojecting an old world position through camera matrices cannot reproduce the previous material-point position of a moving/deforming receiver.
3. **Probe records cannot reconstruct that transport.** The 32-byte record stores world position, packed geometric normal, light/visibility flags, sample UV and contribution (`NrdHistoryConfidence.hlsli:453–454`). It has no object/primitive identity, barycentrics, previous transform, or recorded motion. The light-sample UV is not a surface coordinate. The last word is written as zero; it is not latent motion metadata.
4. **Probe representatives change even when the camera stays still.** The selected pixel cycles through the four offsets of a 2×2 block (`NrdHistoryConfidence.hlsli:378`). Equal probe indices across outputs can refer to different receiver positions, or to different surfaces at a boundary. “Same half-resolution texel” is therefore not an established material-point correspondence even in a stationary-camera experiment.
5. **The existing geometry output is overwritten each generation.** `NrdHistoryConfidencePass.cpp:205–240` writes the old-record geometry into one texture, and `:243–285` produces this update's confidence. The pass has no separate retained geometry/validity history tied to a held result (`NrdHistoryConfidencePass.h:161–166`). New diagnostic history would need its own ownership and submission validity.
6. **The two G-buffer slots are not a supplied confidence-history contract.** They provide useful historical surface information, but neither contains motion. Their public renderer view exposes the committed read slot, not a validity-qualified pair associated with a new hold history. Directly assuming the other slot is always the required frame t−2 would also need explicit handling of gaps, resets, abandoned recordings and resource recreation. Merely reading that slot would not fix the absent object transport.
7. **Recording order does not supply the missing frame.** `Renderer.cpp:10128` records the reference path and its confidence generation before `RecordPreviousGBufferCopy` at `:10140`. That copy records frame t's four G-buffer resources; it does not recover M[t−1]. The confidence-specific committed camera is only a position used for filter plane scaling (`Renderer.cpp:7703,10969`), not a historical object-motion field.

### Correctness-critical blocker

The existing retained inputs do not establish the required t−1-to-t−2 surface correspondence for a general moving-camera/moving-surface hold. Implementing equal-index reuse, applying current motion, or assuming old world positions are static would introduce the potentially incorrect history path the request explicitly prohibits.

This finding does **not** mean a dedicated diagnostic hold is impossible, that an object-ID/V-buffer redesign is mandatory, or that R.9C is misbound. Additional diagnostic-only historical motion and validity metadata may support a narrow implementation. However, the necessary sampling, geometry association and invalidation contract is not established by the existing resources. No unverified reprojection implementation was substituted for that missing contract in this task.

Depth/normal agreement alone is not proof that a stored probe and a newly represented surface are the same material point. In particular, camera-only world reprojection loses receiver motion, and coarse mixed-surface blocks can produce plausible geometric matches on unrelated surfaces. Such tests could be part of a deliberately specified conservative reprojection scheme, but their thresholds and limitations cannot be silently assumed here.

### Why simpler restrictions were not implemented

- Resetting on camera changes alone does not address moving receivers or changing probe representatives.
- Resetting on every TLAS transform change would also discard the hold during the moving occluder updates that this experiment is intended to measure. R.9B deliberately permits those updates (`Renderer.h:1847–1850`).
- A stationary-camera/static-receiver-only mode would be a restricted experiment requiring explicit applicability and rejection rules. This audit did not silently narrow the requested behavior to that mode.
- Applying current motion or reprojecting the final NRD binding would use the wrong interval or change the established confidence coordinate contract.

The specification says: “If a safe temporal correspondence cannot be established within this narrow scope, stop and report the blocker instead of implementing an approximate or potentially incorrect history path.” This is the stop condition applied here.

## 3. Exact implementation and files changed

Only `docs/R9EConfidenceHoldReport.md` was added. No existing implementation or configuration file was edited. Existing R.9A/B/C allocation, dispatch, bindings, selector precedence, confidence floor, probe generation, mapping and Views 4/5/6 are preserved by making no implementation changes. Hybrid and non-RELAX paths are unchanged.

## 4. Hold expiration semantics

Not implemented or executable. The requested semantics remain the intended contract: a fresh raw C[t] event may establish a bounded hold, a stronger new reduction may replace it with its own expiration, and a retained low value must not refresh its own timer. Raw confidence must stay separate from held output. Correct expiration cannot compensate for associating a timer with the wrong surface, so no timer-only implementation was added.

## 5. Resource lifecycle and resets

No new resources, allocations, barriers, dispatches or GPU-wide waits were added. Existing first/reset behavior remains neutral confidence or the R.9C constant-one fallback, as documented in R.9C.

A future implementation needs to establish, before coding transport, which diagnostic-owned metadata represents each confidence output's surfaces; how the correct historical motion is retained and sampled; how jitter, probe offset phase, depth/normal/material compatibility and disocclusions are handled; and how hold outputs and metadata commit together only on valid submitted frames. Scene/light/environment/extent/resource changes, missing generation, skipped/incompatible frames and abandoned submissions must break that chain. These are requirements for resolving the blocker, not implemented lifecycle guarantees.

## 6. Selector compatibility

`ENGINE_NRD_RELAX_CONFIDENCE_HOLD` was **not added**. Consequently it has no parsing, warning or effective-duration behavior in this build. Do not interpret setting it as enabling a hold. Existing generated-over-constant precedence is unchanged. Implementing a selector that announces an unavailable experiment would be misleading, so no partial startup switch was introduced.

## 7. Debug and Release builds

Not run for R.9E: the required stop occurred before code changes. Existing R.9C build results are historical and are not presented as R.9E implementation validation. Building unchanged code would not validate the missing coordinate contract.

## 8. Shader compilation

Not run for R.9E. No shaders were added or modified. Prior R.9C shader compilation does not demonstrate temporal-hold correctness.

## 9. Runtime validation performed

No Engine launch, selector experiment, GPU transition test, resize/mode-switch stress run, or visual comparison was performed. Repository inspection and source verification only. No GPU correctness or visual improvement is claimed.

## 10. Remaining interactive tests

The requested A/B/C/D comparison cannot yet be performed: there is no R.9E hold implementation. The existing A baseline remains R.9C generated confidence. Hold 4/8/16 must not be labeled as distinct implemented variants or judged visually until the coordinate blocker is resolved and a hold path has been built and validated.

## 11. Known limitations and stopping point

This report identifies missing historical correspondence inputs and the unsafe shortcuts; it does not prove that every possible narrowly scoped implementation must fail. A follow-on can first establish a diagnostic-only lagged-motion/geometry transport contract, or explicitly authorize a conservatively restricted experiment with clear applicability checks. Neither a general temporal denoiser nor a V-buffer is proposed as necessary.

All 760 existing inventoried repository files retained their SHA-256 contents during this task; only this report was added. Nothing was staged or committed. Work stops at the requested correctness gate; no confidence tuning or R.9E implementation changes were made.
