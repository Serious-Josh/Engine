# R.9C implementation and validation

Completed 2026-10-27. Diagnostic integration only; no R.9D work or denoiser tuning.

## Audit and changes

The working tree already contained substantial unrelated work and partial R.9C changes in
`src/Renderer/Renderer.cpp` and `src/Renderer/Renderer.h`. The partial implementation already parsed
`ENGINE_NRD_RELAX_GENERATED_CONFIDENCE`, enabled the existing R.9B generator, disabled conflicting
R.9A constant modes with a warning, enabled R.9A's NRD confidence flag, and bound the current generated
texture with constant-one specular and missing-generator fallback. These mechanisms were retained.

Changes made during this completion:

- `src/Renderer/Renderer.cpp`: avoid allocating/clearing the unused 0.1 confidence constant in
  generated mode; preserve allocation and values for existing R.9A modes. Report the effective
  startup confidence source after precedence resolution, and avoid falsely attributing automatic
  probe enablement to `ENGINE_NRD_RELAX_CONFIDENCE_PROBES=1`. Clarify the verified sampling contract.
- `src/Renderer/Passes/NrdHistoryConfidencePass.h`: update the obsolete diagnostic-only-consumption comment.
- `assets/Shaders/NrdHistoryConfidence.hlsli`: update the consumption comment; no executable shader changes.
- This report.

No new ownership, resource type, generator dispatch, synchronization wait, reprojection, parameter,
sampling, filter or mapping change was introduced. Existing unrelated modifications were preserved.
Nothing was staged or committed.

## Coordinate and resolution contract

The vendored `external/NRD/Include/NRD.h` identifies version 4.17.3.
`NRDDescs.h` specifies confidence computed for the previous frame in the current frame, R8 or better,
and explicitly allows lower-resolution textures with linear upscaling.

The vendored SDK does not contain RELAX's internal shader source. The exact upstream
[v4.17.3 temporal shader](https://github.com/NVIDIA-RTX/NRD/blob/v4.17.3/Shaders/RELAX_TemporalAccumulation.cs.hlsl)
was therefore downloaded to `build/r9c/RELAX_TemporalAccumulation.cs.hlsl` for inspection.
Lines 595-599 sample diffuse confidence using `SampleLevel(gLinearClamp, prevUVSMB, 0)`;
lines 638-642 do the same for specular. The previous UV includes the motion/reprojection calculated
earlier in that shader. No additional motion transformation belongs in the confidence binding.

R.9B evaluates each previous record at its original probe index against the current TLAS, writes
the change and geometry at that index, then records the current experiment for the next frame.
Filtering retains that grid. This already produces the previous-frame-space confidence consumed
by NRD this frame. An extra reprojection would apply motion twice.

The output is R16_FLOAT at ceil(width/2) by ceil(height/2). R.9A registration uses R16_FLOAT.
`NRDIntegration.hpp::DenoiseD3D12` wraps the supplied native resource and format without substituting
the full render extent; `_Dispatch` creates views from the wrapped texture description. The native
half-resolution texture is compatible with the documented contract. Actual GPU execution remains
an interactive validation item below.

## Selector and fallback checks

Static review confirmed:

| Configuration | Behavior |
|---|---|
| All diagnostic selectors unset | No confidence inputs registered, no constant confidence allocation, no generator allocation/dispatch |
| Generated unset, R.9A mode 1 or 2 | Original diffuse 1 or 0.1 and specular 1 bindings |
| Probe selector only | Original generator and signal views; confidence consumption remains disabled |
| Generated exactly `1` | Enables existing generator; generated diffuse and constant-one specular |
| Generated plus R.9A mode | Startup warning; R.9A mode disabled; generated takes precedence |
| Generated with no completed generator this frame | Constant-one diffuse and specular; warning once |
| Generated plus views 4/5/6 | Same single generation call; views retain signal/confidence/state resources |
| Hybrid or non-RELAX selected | No added generation; their NRD settings/bindings and rendering functions unchanged |

Selectors are read once at startup and require the exact one-character values already recognized
by their parsers. Views do not implicitly enable probes; that existing selector behavior is preserved.

## Ordering, history and lifetime

Static review of the renderer, command context, generator, shaders and NRD integration confirmed:

- Graphics and ray-tracing interfaces refer to the same command-list object (`QueryInterface`).
  The existing generator call records before regular RELAX consumption, once per frame, independently
  of signal-view selection. No extra list submission was added.
- Only a successful complete `NrdHistoryConfidencePass::Execute` sets the current-frame dispatch flag.
  The renderer clears that flag before frame recording; the binding cannot select an earlier frame's
  confidence when the generator fails or is skipped.
- Each probe thread reads its own old record before overwriting it. There are no cross-probe record
  reads. Existing UAV barriers order record access across queue submissions.
- Signal/geometry/filter transitions order all three filter passes. The final confidence transitions
  from UAV to shader-readable before NRD. NRD's snapshot restores its declared input states.
- Generator resources remain owned by the existing pass through NRD consumption; they are neither
  copied nor released by the new binding. Existing resize/drained release and fenced retention paths
  govern destruction. Texture and RenderTarget shutdown free their descriptors.
- Probe-history identity covers scene resource lifetime, lighting/path key, environment and extent;
  a missing immediately preceding submitted probe frame also invalidates history. Identities advance
  only after successful frame submission. Recreated resources invalidate previous records.
- First/reset frames pass `previousValid=0`: every probe writes zero change and no geometry, and the
  final filter writes confidence 1. Missing/failed generation instead selects the constant-one texture.

These are source-level checks, not GPU debug-layer or descriptor-stress test results. No new GPU-wide
wait or resource lifetime policy was added.

## Builds and focused validation

| Check | Result |
|---|---|
| Debug Engine build | PASS, `build/r9c/debug-build.log` |
| Release Engine build | PASS, `build/r9c/release-build.log` |
| CSProbe and CSFilter | PASS, DXC cs_6_6, HLSL 2021 |
| CSPrepareRelax, CSResolveRelax, CSResolveDenoisedRelax | PASS, same compiler/profile |
| CSResolveRelaxSignalView (including views 4/5/6) | PASS, same compiler/profile |
| Existing NRD CPU SDK query, Debug and Release | PASS_CPU_ONLY; `build/r9c/sdk-debug.csv`, `sdk-release.csv` |
| Focused selector, reset, ownership, ordering and isolation review | PASS at source level, as detailed above |
| Renderer diff whitespace check | PASS |

Build command: `cmake --build build --config <Debug|Release> --target Engine -- /m /v:minimal`.
The Engine target also built its EngineRuntime dependency. Initial restricted Debug execution hit
MSBuild file-tracker access failure; rerunning with the required access passed. Final build logs
contain no warnings or errors.

Shaders were compiled directly from `assets/Shaders` with include paths `assets/Shaders` and
`build/Debug/assets/Shaders` (the deployed RTXDI headers), using `build/_deps/dxc-src/bin/x64/dxc.exe`.
Outputs are `build/r9c/CS*.dxil`.

The existing `tests/HybridOrigin` CPU query executable was run against Debug and Release SDK DLLs.
Their SHA-256 hashes match the respective vendored DLLs. It exercises combined RELAX and other SDK
denoisers, not the renderer: confidence-disabled frames report zero confidence bindings; enabling
confidence adds the expected bindings without changing steady-state dispatch counts. For its B case,
steady-state dispatch counts remain 19 and confidence binding occurrences change from 0 to 14.
This is an SDK regression check, not a full-frame R.9C test or a measurement of GPU allocations.

## Remaining interactive tests and limitations

No full-frame GPU run or visual comparison was performed. Shader compilation and source review do
not establish absence of debug-layer errors or descriptor leaks under repeated runtime transitions.
No improvement in responsiveness, stationary noise or lingering shadows is claimed.

Use `build/lighting-l2-tests/ControlledProject/ControlledLighting.engineproj`, regular RELAX with
Raster ReSTIR DI, and otherwise identical settings. Restart between startup-selector cases:

1. Baseline: generated and constant-confidence selectors unset.
2. Generated: `ENGINE_NRD_RELAX_GENERATED_CONFIDENCE=1`.
3. Constant low: generated unset; `ENGINE_NRD_RELAX_HISTORY_CONFIDENCE=2`.

Compare stationary noise, shadow motion and lingering shadows after stopping. Inspect views 4/5/6
under generated mode, first/reset frames, resize, light/environment changes, mode switches and
generator fallback. Check D3D12 debug-layer messages and descriptor/resource stability during repeated
transitions. Hybrid and non-RELAX full-frame comparisons remain unverified at runtime.
