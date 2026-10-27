# R.9I.1D — automatic specular firefly capture

2026-10-27. Diagnostic only, off by default. Nothing in the estimator changed: GGX and its multiple-scattering compensation, BSDF sampling and PDFs, point-light attenuation, visibility, MIS, throughput, Russian roulette, RNG, SHARC, progressive accumulation, RTXDI, NRD and DLSS are all as they were. No clamp was added. The R.9I.1 selected-pixel diagnostic is unchanged and still works on its own.

## 1. Architecture audit (Phase A)

R.9I.1 already does passive per-sample bookkeeping (`g_sampleRecord`, 288-byte `PathTraceSampleRecord`). It is gated by one selected pixel and writes up to 64 records into a fixed buffer, with two fence-tracked readback slots and quarantine on abandoned recordings. Records are complete only when a sample finishes. Ray generation runs one lane per pixel and cannot synchronize with other lanes in the same dispatch.

Options considered:

| Option | Verdict |
|---|---|
| Append buffer of every threshold crossing | Rejected: unbounded, or silently truncating. |
| Full-resolution per-pixel record buffer + reduction pass | Rejected: about 600 MB at 1080p, and needs a new compute pass and resource growth that scales with the render extent. |
| Re-trace winners in a second dispatch (deterministic RNG) | Rejected: needs a new raygen and shader-table entry, and SHARC/accumulation state would have to match exactly. That is significant restructuring. |
| Tile/workgroup-local selection | Not available: ray generation has no group shared memory or group barriers. |
| **Fixed hashed buckets, atomic max key, single-attempt lock (selected)** | Bounded, needs no new pass, fits the existing slot/fence model, and avoids a global hot atomic. |

No renderer restructuring was needed, so the work went ahead.

## 2. Implementation

### GPU collection (per dispatch)

- **Buffer:** `PathTraceAutoBucketCount = 256` buckets × `PathTraceAutoBucketStride = 320` bytes, which is **81,920 bytes**. Each bucket has a 32-byte header followed by one unchanged `PathTraceSampleRecord`. The constants are shared through `assets/Shaders/PathTraceSampleRecord.h`.
- **Header words:** `[0]` key = `asuint(indirect luminance)` (0 means empty), `[1]` lock, `[2]` owner pixel `x | y<<16`, `[3]` threshold crossings, `[4]` record-lock failures, `[5]` mean-lock failures, `[6..7]` reserved.
- **Gate:** when automatic capture is on (`sampleAutoIndex != UINT_MAX`), `PathTraceRunRayGeneration` turns on the existing passive bookkeeping for every pixel. `g_sampleManual` keeps the selected-pixel store behind its own gate. Other entry points (split DI direct, sparse SHARC training) leave both gates false.
- **Per completed sample** (`PathTraceDiagnosticAutoStore`, `assets/Shaders/PathTraceSampleDiagnostic.hlsli`). This runs after the existing record finalization and before the selected-pixel threshold strips detail:
  1. It returns unless indirect luminance is `>= threshold` and `> 0`. NaN never qualifies.
  2. The pixel is hashed (integer mix) to one bucket. The hash spreads clustered fireflies over many buckets.
  3. `InterlockedAdd` on the crossings counter, then `InterlockedMax` on the key.
  4. If the previous key is `>=` this key, the lane stops. A tie keeps the earlier winner.
  5. Otherwise it makes **one** lock attempt (`InterlockedCompareExchange`, never a spin). On failure it counts a lock failure and stops.
  6. While holding the lock, it re-reads the key atomically and writes only if the key still equals its own. A stronger event that took the lock first is never overwritten by a weaker one.
  7. It writes the full record with the expanded-provenance flag set and `mean.w = -1` (unavailable), then writes the owner pixel atomically.
  8. Unlock is `DeviceMemoryBarrier()` followed by an atomic exchange, so one holder's stores are visible before the next holder writes. Records cannot interleave.
- **After the batch** (`PathTraceDiagnosticMean`): a lane that wrote a record makes one lock attempt. If the owner is still its pixel, it stores the actual post-batch progressive mean. If not, the mean stays marked unavailable and the failure is counted.
- **Atomic load:** atomics run only on threshold crossings and are spread across 256 buckets. No sample touches a global counter.

### Constants

The two former `sampleDiagnosticPadding` words became `sampleAutoIndex` (offset 312) and `sampleAutoThreshold` (offset 316). The buffer stays **320 bytes**. The C++ `static_assert` and the DXC reflection both confirm the offsets.

### CPU ownership and readback (`src/Renderer/PathTraceSampleDiagnostic.h`, `PathTraceAutoCapture`)

- Two GPU slots and two readback slots (81,920 bytes each), plus one shared all-zero upload buffer used as the clear source: **409,600 bytes** in total. They are allocated lazily on first enabled use. There are two UAV descriptors in total. Nothing scales with resolution, and nothing is reallocated on resize.
- **Per frame:** COMMON → COPY_DEST, then a copy from the zero buffer (clear), then → UAV for the dispatch. After the dispatch: UAV barrier → COPY_SOURCE → `CopyBufferRegion` to readback → COMMON. This is the same end state and decay rule as the R.9I.1 slots.
- **Fences:** the slot is bound to the successful submission fence through the existing `Commit`. `Configure` maps a slot only after its fence has completed, in fence order, with an empty write range. There is no wait anywhere. The device-removal sentinel suspends capture. An abandoned recording is quarantined until shutdown and never reused early. `Shutdown` runs after the renderer's existing queue drain.
- **Busy slot:** the frame renders normally but is **not observed**. It is counted as `framesSkipped`.
- **Generations:** changing enable, threshold, clear, render extent or run/reset identity advances the generation. Completed slots from an older generation are discarded unread. Selected-pixel changes do not touch automatic records, and the reverse is also true.
- **Integration:** `PathTraceSampleDiagnostic` owns a `PathTraceAutoCapture` and forwards Configure, Copy, Commit, Abandon, Invalidate and Shutdown to it. `Renderer.cpp` gained one call, `BeginAutomatic`, next to the existing `Begin`.

## 3. Ranking and retention semantics (Phase C)

- **On the GPU, per frame:** each bucket keeps its single strongest individual indirect sample by luminance. A tie keeps whichever reached `InterlockedMax` first.
- **On the CPU, across the capture:** each validated bucket winner is offered to a list of at most **32**, ordered strongest first (`upper_bound`, so an equal later candidate ranks after the earlier one). When the list is full, a stronger arrival evicts the weakest, and a weaker one is rejected. Both cases add to `notRetained`. Later, stronger events always replace earlier, weaker ones.
- **Validation before a candidate is retained:**
  - frame serial, old count and batch size match the slot;
  - the global sample index is inside the batch;
  - the pixel is inside the render target;
  - luminance is at or above the threshold.

  Any failure is counted as `invalid`, or as `maximaLost` if the bucket recorded a lock failure.
- **`bucketMaximum = false`** means the stored record is weaker than the bucket's key. A stronger same-frame event in that bucket lost the lock, and the count shows in `maximaLost`.

**Coverage, stated plainly: the list is not exhaustive.** A sample can be missed in four ways:

1. A stronger sample in the same hashed bucket in the same frame displaced it.
2. Its bucket's write lock was contended.
3. Its frame was unobserved because the readback slot was busy.
4. It was outranked by 32 stronger retained candidates.

Every one of these is counted and reported. None is silent.

## 4. Record fields (Phase B/D)

Each candidate holds pixel X/Y, run, and the R.9I.1 record unchanged:

- frame serial (low 32 bits), global sample index, old N, batch size, max depth, target, seed;
- flags: SHARC substitution, raw sample rejected, expanded provenance;
- raw sample RGB/Y, taken before rejection;
- indirect RGB/Y;
- post-batch progressive mean RGB/Y, or *unavailable*;
- primary, secondary direct, deeper direct, indirect miss, SHARC substitution (each RGB/Y);
- dominant secondary light: contribution and its diffuse/specular split, f\*cos\*Li RGB and cosine, Li RGB, light distance, incoming throughput RGB and incoming BSDF PDF, first secondary hit distance, dominant-vertex roughness, light PDF, MIS/pdf weight, vertex, light index, light type, material index.

When there is no visible secondary or deeper light sample, light index `UINT_MAX` is reported as *unavailable*.

### Telling the three cases apart

For a one-point-light, depth-2 event, the dominant vertex is 1. Its *incoming throughput* is the path weight after the primary specular bounce, which already includes the primary GGX multiple-scattering weight. *f\*cos\*Li* is the secondary response.

The report prints `throughput × f*cos*Li × weight`. This is a forward product; nothing is reconstructed by division. It is printed next to the recorded dominant contribution, plus the dominant share of indirect.

| Case | Signature |
|---|---|
| 1. Large arriving throughput | throughput Y well above 1, with moderate f\*cos\*Li Y |
| 2. Large secondary point-light response | throughput Y ≤ about 1, with large f\*cos\*Li Y; then check light distance, Li, roughness and cosine |
| 3. Both | both values are large |

None of these cases is labelled a bug. Raw BSDF f is not captured separately (as in R.9I.1), so f\*cos\*Li cannot be split into BRDF and cosine/Li without division. Cosine and Li are reported separately.

## 5. UI and report (Phase E)

Under **Rendering Settings → Indirect sample provenance (R.9I.1)**, below the selected-pixel controls, the separate **Automatic firefly capture (R.9I.1D)** section contains:

- Enable automatic capture (default off).
- Detection luminance threshold (default 10; scene-linear; filter only).
- Clear automatic records.
- Copy Diagnostic Report.
- State line.
- Run, frames observed / not observed, retained N of 32.
- Crossings, bucket winners, not retained, maxima lost, mean unavailable, invalid.
- An expandable **Strongest candidates** list. Each row shows indirect Y, throughput Y and f\*cos\*Li Y, and expands to the full block.

**Copy Diagnostic Report** uses ImGui's existing platform clipboard (`ImGui::SetClipboardText`); no dependency was added. The plain-text report contains:

- version line (`report v1`);
- a disclaimer that candidates are not confirmed visible white pixels;
- the scene-linear / pre-denoiser note;
- capture mode, bucket count, list size and threshold;
- state;
- render size, run/reset and current N;
- lighting mode, samples per frame, max depth, roulette depth, target, denoiser, render-scale index and DLSS, SHARC enabled;
- frames observed and unobserved;
- crossings, bucket winners, retained, not retained;
- maxima lost, lock failures, mean-lock failures, mean unavailable, invalid;
- the coverage statement;
- one block per candidate with every field in §4.

Scene light count and environment/sun state are explicitly listed as *unavailable*. A missing mean, hit distance, dominant light or material reads *unavailable*.

## 6. Validation (Phase F)

| Check | Result |
|---|---|
| Engine Debug and Release builds (`cmake --build build --config <cfg> --target Engine -- /m /v:minimal`) | Passed with no warnings or errors. Logs are in `build/r9i1d/`. |
| DXC, runtime-identical arguments (`-T lib_6_6/lib_6_9 -HV 2021 -I <dir>`, plus `-I <root>` for GI6/DI1) over the deployed tree | **72/72 libraries compile.** All PathTraceReference, SHARC, raw-distance, SER/SM69, GI6 and DI1 variants produce zero messages. The only warnings are existing `numthreads` notes in unrelated compute files the glob swept in. Validation accepts the device barrier and atomics in lib shaders. |
| Constant-buffer ABI | DXC reflection: `sampleAutoIndex` at 312, `sampleAutoThreshold` at 316, size 320. C++ `static_assert` agrees. Bucket stride = 32 + `sizeof(PathTraceSampleRecord)` is asserted. |
| CPU tests (`build/r9i1d/Checks.cpp`, ignored) | Passed: strongest-first order, tie order, capacity bound, eviction and rejection counts, later-stronger replacement. Generation isolation passed for threshold, clear, run, resize, invalidate and disable, and selected-pixel changes leave automatic records alone. Device-removal suspend passed. The merge rejects stale-frame, out-of-batch and out-of-range records and classifies lock loss and missing mean. |
| R.9I.1 regression tests (`build/r9i1/Checks.cpp`) | Still pass after the shared-header refactor. |
| Source noninterference | Compared with the R.9I.1 pre-diagnostic snapshot: RNG calls (19), `PathTraceCreateRandom`, BSDF sampling, throughput and roulette updates, ray direction, `TraceRay`/visibility calls, sample sum, rejection, mean arithmetic, accumulation and output stores, and SHARC injection are unchanged. The R.9I.1D diff of `PathTraceReference.hlsl` is 8 lines, all diagnostic identifiers. The threshold feeds only diagnostic branches. |
| Disabled behaviour | `sampleAutoIndex = UINT_MAX` keeps `g_sampleDiagnostic` false for every pixel except a selected one, which is identical to R.9I.1. No allocation, clear, copy or write. |
| Resource state, readback lifetime and shutdown | Reviewed: explicit transitions, Map only after fence completion, quarantine on abandon, release after the existing drain, no waits. |

**Not performed:**

- No Engine launch or GPU runtime execution of the capture, and no live readback or end-to-end clipboard test.
- No diagnostic-off versus diagnostic-on framebuffer comparison, and no performance measurement.

Source-level noninterference is established. GPU bitwise identity is **not** claimed. Turning the bookkeeping on for every pixel can change register allocation and scheduling, and the enabled-mode cost is unmeasured and may be significant.

## 7. Known limitations

- Candidates are large **raw indirect samples**, not confirmed display fireflies. The progressive mean and N show how much of each sample survives in the image.
- Indirect follows the R.9I.1 definition: vertex ≥1 direct light, misses after vertex ≥2, and SHARC substitution. Primary direct light is excluded. NaN indirect luminance is never detected. Rejected raw samples are captured when their indirect part qualifies, and they are flagged.
- Per frame, only the strongest sample in each of the 256 hashed buckets can be kept. Pixels that share a bucket compete with each other.
- Single-attempt locks can lose a bucket maximum or a mean under contention. Both are counted.
- Only frames whose readback slot is free are observed. Frames that are not observed are counted.
- A run or reset (camera move, setting change) clears the list. At the sample target with denoiser Off, no new batches run, so the list freezes and can still be copied.
- If a stronger candidate from a later frame is the same pixel, it appears as a separate row.
- Diagnostic settings are not saved.

## 8. Runtime reproduction procedure (no pixel selection)

1. Build and run Engine. Open the reproducing sealed-room scene: one point light, environment and sun off, SHARC off, PT denoiser **Off**, DLSS **Off** (native render scale), metallic 1.0, roughness **0.1**, max path depth **2**. Keep the camera still.
2. Open **Rendering Settings → Indirect sample provenance (R.9I.1)**. In **Automatic firefly capture (R.9I.1D)**, tick **Enable automatic capture** and leave the threshold at **10**.
   - If *Crossings* climbs into the millions per frame, raise the threshold. A higher threshold reduces contention and lost maxima.
   - If nothing is retained, lower it.
3. Restart accumulation once, for example by changing **PT sample target** and changing it back. Confirm that *frames observed* increases.
4. Let the image converge until fireflies appear. Expand **Strongest candidates** if you want to look at them in the UI.
5. Click **Copy Diagnostic Report** and paste the result.
6. Optional comparison: set roughness to 0.2, or move the light. Both changes restart the run and clear the list. Repeat steps 4–5 and compare throughput Y against f\*cos\*Li Y across the two reports.

Stop point: R.9I.1D only. No rendering fix and no luminance clamping.
