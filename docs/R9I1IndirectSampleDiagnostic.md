# R.9I.1 — indirect sample provenance diagnostics

2026-10-27. Instrumentation only. Default disabled; no clamp, estimator correction, ReSTIR GI, scene/lighting change or NRD/SHARC tuning.

## 1. Source audit and instrumented locations

Reviewed R.9H/R.9J and the current transport, reconstruction, UI and submission code before implementation. Existing full/diffuse extraction does not isolate indirect lighting: diffuse contains primary direct and later lighting carried through the primary diffuse BSDF. Therefore the diagnostic adds passive sums at existing contribution sites, without changing the production accumulator or extracting indirect by subtracting two large radiance values.

| Location | Instrumentation |
|---|---|
| `assets/Shaders/PathTraceReference.hlsl:1001–1178` | Observe incoming throughput/PDF, secondary hit/material, primary/secondary/deeper direct terms and existing miss terms. Two miss expressions now name their existing single evaluation before adding it, allowing observation without a second evaluation. |
| Same file, `PathTraceShadeLightSampleLobes:528`, `PathTraceDirectLightingLobes:555–575` | Observe existing visible light contribution, diffuse component, incidence, cosine, light PDF/weight, light index/type and dominant contributing vertex. No extra BSDF/light/visibility evaluation. |
| Same file, `PathTraceRunRayGeneration:1405–1434` | Gate by selected render pixel; reset passive state for each individual sample; store completed raw estimate before existing nonfinite/negative rejection. |
| Same file, `:1471–1482` | Capture the actual computed progressive mean after the entire batch; original accumulation and presentation stores remain unchanged. |
| `assets/Shaders/PathTraceRadianceCacheQuery.hlsli:504` | Observe the exact throughput × incident term added by a successful SHARC substitution. No changes to cache eligibility, queries, training or termination. |
| `assets/Shaders/PathTraceSampleDiagnostic.hlsli` | Diagnostic-only bookkeeping, dominant-light selection, bounded raw-buffer stores and batch-mean write. |
| `src/Renderer/Renderer.cpp:6647–6651,6831–6839,6915,9706,11040,11285` | Configure/poll, bind optional buffer, copy before reconstruction, abandon unsubmitted recording, associate successful submission fence and release after shutdown drain. Broad scene reset invalidates diagnostic history at `:4841–4842`. |
| `src/Debug/DebugUI.cpp:1325,1474` | Controls/readout inside path-tracing settings. |

Frame diffuse/specular exports remain at `PathTraceReference.hlsl:1497` onward. Engine/NRD reconstruction later replaces presentation independently; tone mapping remains in `assets/Shaders/ToneMap.hlsl`, downstream of the renderer's normal resolve/upscaling path. This diagnostic does not write those signals or display resources.

## 2. UI controls

In Rendering Settings, expand **Indirect sample provenance (R.9I.1)**:

- **Capture selected PT pixel** enables capture; default false.
- **Render pixel X/Y** are explicit zero-based coordinates in the internal path-tracing render target. The actual target dimensions and selected coordinates are displayed. Invalid coordinates suspend capture rather than silently picking another pixel.
- **Detail luminance threshold (capture only)** defaults to 100 scene-linear luminance units; use **0** initially to retain expanded provenance for every nonnegative indirect sample. This threshold never changes radiance, PDFs, transport, accumulation or sampling.
- **Clear sample records** clears diagnostic history/peak and invalidates pending old-generation records. It does **not** reset progressive accumulation.

The UI shows capture state, run/reset serial, current CPU accumulated count at recording time, latest completed record, peak indirect record, RGB/luminance, provenance, SHARC/rejection/detail flags and a compact recent-sample history. Readback is asynchronous, so latest completed data can lag the current count. The expandable peak retains its original frame/sample metadata even after its ordinary history row is overwritten.

Changing enablement, pixel, threshold or clear serial starts a new diagnostic capture generation without changing the PT accumulation run. Camera/scene/PT-setting resets also discard old-generation diagnostic results. No mouse picking or large visualization system was added.

## 3. GPU layout and bounded capture semantics

The shared C++/HLSL ABI is `assets/Shaders/PathTraceSampleRecord.h`: **288 bytes**, eighteen 16-byte rows. No packed float3 assumptions. C++ size/offset assertions guard layout; the path constants additionally assert descriptor offset 288, threshold 300 and frame serial 304. DXC reflection agrees; total path constant buffer is now 320 bytes. Non-raw-distance shaders reserve the existing raw-distance row so the diagnostic extension has identical offsets in every library variant.

| Rows | Meaning |
|---|---|
| `meta`, `config` | Submitted-frame sequence low 32 bits, global sample index, accumulated count **before** batch, batch size, max depth, target, run seed; flags for SHARC substitution, rejected raw estimate and expanded provenance. |
| `sample`, `indirect`, `mean` | Individual raw sample RGB/Y before rejection; passively summed individual indirect RGB/Y; actual progressive RGB/Y after the batch. |
| `primary`, `secondary`, `deeper`, `miss`, `cache` | Camera-weighted source categories described below; each has RGB and scene-linear Y. |
| `dominant`, `diffuse`, `specular` | Largest visible individual secondary/deeper light term and its diffuse/specular decomposition at that vertex, camera-weighted. |
| `factors`, `incidence`, `throughput`, `hit`, `info` | Existing pre-MIS f*cos*Li RGB and cosine; Li/light distance; incoming throughput/incoming BSDF PDF; first secondary distance, dominant roughness/light PDF/MIS-PDF multiplier; dominant vertex/light index/type/material index. |

Each selected pixel writes at most **64 records per traced frame**, matching the validated production maximum samples-per-frame. Sample offset selects a unique slot; every write and mean-update loop is bounded to 64. There are no atomics, cross-pixel races or dynamically growing GPU queues. Unselected pixels write nothing. Other raygen entry points, notably the split DI pass and sparse SHARC training, leave the capture gate false.

Every sample remains an **individual** record even when a batch has multiple samples. All records from one batch share the actual final batch mean. The old N is `meta[2]`; new count is N+batch. It is incorrect to treat the captured mean as the mean immediately after that particular sample, or the raw sample as a batch average. Rejected raw samples are flagged: production replaces them by zero and still counts them in the denominator.

Two fixed GPU buffers and two fixed readback buffers each hold 64×288=18,432 bytes: 73,728 payload bytes total, excluding D3D allocation granularity. They allocate lazily only when capture is enabled and tracing. CPU keeps 128 recent records plus one peak. History overwrite count is separate from dropped capture records. Busy/quarantined slots, allocation/Map failures, identity mismatches and any over-capacity batch increment dropped count. At target convergence no new batch runs, so no fabricated/repeated record is emitted. Disabling capture performs no new GPU diagnostic allocation, write or copy; previously allocated bounded resources remain owned until shutdown.

The compact fields are always retained. Expanded provenance is zeroed/marked unavailable below threshold. Thus a peak below threshold can lack detailed light information; rerun with threshold 0. Threshold comparisons do not suppress the compact sample/mean record.

## 4. Indirect definition and provenance

**Indirect** is the passive sum of direct illumination at vertices >=1, misses following at least one secondary surface (vertex >=2), and SHARC replacement terms. Primary direct includes x0 illumination, its immediate BSDF-sampled sun/environment miss, and camera background where applicable. Therefore depth 1 has no surface-bounce indirect term. The split RTXDI path's omitted primary non-delta miss remains omitted; the diagnostic does not reconstruct a term the estimator did not add.

Secondary direct is vertex 1 only. Deeper direct is vertices >=2. Miss is indirect miss lighting only. SHARC's category is the **complete actual replacement incident term**, including its optional immediate miss contribution; it is not cached RGB alone. These categories are disjoint in diagnostic indirect bookkeeping. Passive sums can differ from regrouped production totals by normal floating-point rounding; they are not used to render or to assert a bit-exact partition.

Dominant light is the maximum **camera-weighted luminance of each individual visible conventional light sample**, across every secondary/deeper vertex visited. It is not the last visited light/vertex. For that event, capture its existing full and diffuse contribution and their remainder. Primary RTXDI direct is captured in aggregate; the diagnostic does not add reservoir introspection or reevaluate its light.

Compare secondary/deeper/miss/SHARC aggregate magnitudes to classify which mechanism dominates overall. The maximum-light record does not claim to be the dominant path event when the cache or a miss is larger; the separate categories make that distinction visible. SHARC participation means actual substitution, not merely an enabled/allocated/training cache. Light types are 0 point, 1 spot, 2 directional, 3 sun, 4 environment. Sun/environment indices are UINT_MAX-1/-2; UINT_MAX denotes unavailable/no dominant visible light. Material index and roughness belong to the dominant light's vertex; the captured hit distance belongs to the first secondary ray, not necessarily that dominant vertex.

## 5. Readback and lifetime safety

`src/Renderer/PathTraceSampleDiagnostic.h` owns both slots. `Configure` consumes only records whose successful submission fence has completed. A device-removal completion sentinel suspends capture rather than treating every fence as complete. CPU frame/sample/batch identity is checked against each record before publication, preventing unwritten/stale bytes from appearing as fresh data. Completion is consumed in fence order.

Per slot: COMMON -> UAV for the path dispatch; explicit UAV barrier -> COPY_SOURCE -> readback CopyBufferRegion -> COMMON. Ending in COMMON also matches D3D12 buffer decay and the Buffer CPU state tracker. Readback resources remain COPY_DEST; Map occurs only after completed-fence proof, with a read range and empty CPU-write range. No diagnostic GPU-wide wait, per-frame synchronous wait or new queue is introduced.

Successful Renderer submission commits the slot fence. A recording lacking confirmation is quarantined, not recycled or freed; capture may continue on the other slot. Quarantined memory remains bounded and is released only after the existing shutdown GPU drain. A renderer restart recovers such slots; the diagnostic does not add a recovery synchronization redesign. Scene/coordinate/extent/run changes advance capture generation and discard pending results from older generations without prematurely freeing in-flight resources. Buffers are independent of render extent and are not reallocated on resize. Broad scene-history resets immediately clear readouts. Allocation failures leave transport untouched and show capture failure/drop state.

## 6. Validation performed

| Check | Result |
|---|---|
| Debug and Release Engine | Passed with existing `cmake --build build --config <configuration> --target Engine -- /m /v:minimal`; logs under ignored `build/r9i1/`. EngineRuntime dependency also built. No production C++ warnings/errors in successful logs. |
| DXC shader checks | Passed HLSL 2021 lib_6_6 and lib_6_9 checks covering reference, raw-distance, SHARC render/raw render/query/deferred/training, split RTXDI/extraction, SER and GI6/DI1 wrappers. No warnings/errors in successful shader checks. |
| Shared record/constants ABI | C++ assertions passed; DXC disassembly/reflection confirms the appended constant offsets and 320-byte buffer. Fixed record row offsets also checked in the isolated C++ executable. |
| History isolation/defaults | Isolated executable using the production diagnostic header passed: disabled default, same-run count advancement preserves records; run/pixel/clear/resize/invalidation/disable changes clear old records; device-removal sentinel suspends capture. These are CPU state tests, not a GPU readback execution test. |
| Source noninterference | Compared against pre-edit snapshots. Existing RNG calls/sample indexing, BSDF sampling, throughput/roulette expressions, ray directions, visibility calls, SHARC radiance injection, progressive arithmetic/store and frame extraction arithmetic were retained. New operations feed only diagnostic locals/buffer; threshold never feeds estimator decisions. |
| State/fence/retirement review | Audited explicit UAV/copy/common transitions, completion-before-Map, success-bound submission, quarantine and shutdown ordering. No new waits or early descriptor releases. |
| Working-tree preservation | SHA-256 inventory and comparison found only the seven intended pre-existing integration files changed; all other existing files were preserved. New files are the diagnostic owner, shared record/helper shaders and this report. No staging or commit. |

The sandboxed MSBuild attempt failed before compilation because its file tracker lacked access; successful builds used the required execution access. Scratch compiler discovery/include-path issues were resolved within `build/r9i1/`, not by changing production build files.

No Engine launch, original-scene visual reproduction, GPU readback execution, performance measurement or deterministic diagnostic-off/on framebuffer comparison was performed. The original live reproducing scene/configuration was not identified sufficiently for an isolated rendering test. Source-level noninterference is established; **GPU bitwise identity and negligible disabled-mode timing cost are not measured**. Added shader uses can affect compiler register allocation/fusion even when source transport expressions are unchanged. No visual improvement or root-cause diagnosis is claimed from compilation.

## 7. Known limits

- Not a complete bounce/light trace. It captures the largest visible secondary-light event, aggregate other categories, first secondary distance and incoming throughput/PDF, not every BSDF weight/roulette survival along the path.
- Raw BSDF f is not retained separately by the existing light-shading expression. The record truthfully shows **f*cos*Li** plus cosine, incident Li, light weight and incoming throughput. No division to reconstruct f or extra BSDF evaluation was introduced. This can distinguish high incidence versus large weighted response, but cannot prove which internal BRDF factor is faulty.
- SHARC replaced incident and participation are recorded; cache key, moments, publication metadata and separate cached/miss split are not. Training alone does not set participation.
- Recorded mean is progressive pre-denoiser/pre-exposure RGB. No separate post-RELAX, post-DLSS or post-tone-map pixel is read back. With Off/DLSS Off, a declining captured mean with visually constant brightness supports a presentation/perceptual explanation; it does not identify the exact display transformation. With Engine/NRD enabled, the displayed value can come from another history.
- Frame serial is low 32 bits; run identity and bounded pending slot lifetime also guard record ownership. No disk export or unbounded logging; use peak/history readout to record values. A 128-record history can overwrite early data rapidly at high samples-per-frame.
- Instrumentation overhead when enabled and compiler-level noninterference remain runtime validation items. Diagnostic settings are not serialized scene/renderer settings.

## 8. Exact user experiment

1. Build/run the updated Engine and open the **same reproducing scene**. Use its existing lights/materials, **PT denoiser Off, DLSS Off, depth 2**, stationary camera. Keep confidence strength 2 and SHARC settings unchanged.
2. Expand **Indirect sample provenance (R.9I.1)**. Enable capture, enter the internal render-target coordinate of a previously bright pixel, and set detail threshold **0** for the first investigation. Use displayed dimensions; window/display coordinates can differ at reduced render scale.
3. Change **PT sample target** once to restart accumulation. **Clear sample records** clears only the diagnostic and is not a substitute. Watch the reset/run serial and sample count.
4. Let fresh samples arrive. Open **Peak indirect sample** and note its frame/sample/seed, indirect/sample RGB/Y, old N/batch and mean; compare primary, secondary direct, deeper, miss and substitution terms. For a dominant light, note type/index/material, incidence/light distance, roughness, f*cos*Li and incoming throughput/PDF.
5. Compare later **Recent individual samples** and latest progressive mean as N grows. An isolated energy E has an analytic E/N share while samples accumulate; the actual mean also includes every other sample. Check for recurrent bright events instead of inferring them from a held display. At target the image and records stop advancing in this configuration.
6. Repeat with another target reset. Capture a nearby unaffected coordinate in another run; coordinate changes discard diagnostic history, so note the first run's peak before switching. Optionally repeat at greater depth, keeping other settings fixed.
7. If drops appear, inspect busy/allocation/quarantine state and reduce samples-per-frame only in a separately noted test. If expanded provenance is absent, verify threshold 0 and reset again. Do not infer a scene diagnosis from the visual spot alone.

## 9. Is this sufficient for R.9I.2 clamp design?

It supplies the first needed evidence: whether a fresh indirect sample is large, whether the progressive mean retains/dilutes it, and whether secondary direct lighting, deeper/miss transport or SHARC replacement is the dominant category. **It does not yet justify a luminance clamp or select its domain/threshold.** First run the experiment, confirm readback/noninterference on the GPU, and classify the outlier. Incorrect PDFs/material/ray behavior require a source correction; valid high variance requires an estimator/quality assessment. Some cases will need more narrowly targeted factor/cache telemetry before any clamp design. R.9I.2 was not started.
