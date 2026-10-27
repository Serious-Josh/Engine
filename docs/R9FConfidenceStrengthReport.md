# R.9F — generated RELAX confidence strength diagnostic

Implemented 2026-10-27. Experimental startup diagnostic only. No temporal hold or additional tuning.

## 1. Audit and implementation summary

Reviewed the R.9C implementation report, R.9D audit, R.9E blocker report, current R.9B/R.9C shaders and pass ownership, renderer sequencing and working-tree state before editing. Existing R.9C already enabled the shared generator, selected its completed current output for diffuse confidence, supplied constant-one specular/fallback confidence, and disabled conflicting R.9A modes. These mechanisms were retained. Extensive unrelated working-tree edits were preserved.

R.9F adds only a strict startup selector, one scalar parameter through existing constant-buffer padding, and an optional exponent at the existing final confidence write. No new GPU resource, descriptor, pass, dispatch, shadow ray, readback, temporal history, or GPU-wide wait was introduced.

## 2. Exact files changed

| File | R.9F change |
|---|---|
| `src/Renderer/Renderer.h` | Default-one diagnostic strength member. |
| `src/Renderer/Renderer.cpp` | Strict bounded environment parsing, dependency warning, effective-mode logging, and parameter passed to the existing generator call. |
| `src/Renderer/Passes/NrdHistoryConfidencePass.h` | Filter constant strength replaces one padding word; Execute accepts the scalar; View 5 contract comment updated. |
| `src/Renderer/Passes/NrdHistoryConfidencePass.cpp` | Copies the scalar into the existing filter upload. |
| `assets/Shaders/NrdHistoryConfidence.hlsli` | Matching constant layout and explicit optional exponent branch after original mapping. |
| `assets/Shaders/NrdPathTracePrepare.hlsl` | Comment clarifies View 5's strengthened meaning and nearest-sample/display limitations; no executable changes. |
| `docs/R9FConfidenceStrengthReport.md` | This report. |

Checks, generated test source/binaries, logs and shader disassemblies are under ignored `build/r9f/`. No build files outside that scratch directory were changed. Nothing was staged or committed.

## 3. Selector behavior and precedence

`ENGINE_NRD_RELAX_CONFIDENCE_STRENGTH` is read once at startup. A two-byte buffer and returned length enforce an exact one-character whitelist.

| Value/configuration | Effective result |
|---|---|
| Unset or `1` | Strength 1: original R.9C arithmetic. |
| `2`, `4`, `6`, with generated confidence enabled | Corresponding exponent applied to final generated confidence. |
| `2`, `4`, `6`, without `ENGINE_NRD_RELAX_GENERATED_CONFIDENCE=1` | Clear dependency warning; override ignored; effective strength 1. |
| Unsupported or malformed nonempty value | Clear unsupported-value warning; override ignored; effective strength 1. No clamping or prefix parsing. |
| Generated mode plus R.9A constant mode | Existing generated-over-constant warning and precedence remain unchanged. |
| Strength selector alone | Does not enable generated confidence, probe allocation, confidence bindings or generation. |

Examples rejected: `0`, `8`, `16`, `-1`, `1.0`, `2.0`, `+2`, `02`, leading/trailing spaces and overlong values. The effective strength is always logged. Modes above 1 explicitly log that View 5 displays strengthened confidence and NRD samples it bilinearly. No command-line option was added. As with the existing selectors, an absent/zero-length Win32 environment result uses the default.

## 4. Transformation location and coordinate contract

`NrdHistoryConfidencePass::Execute` still records one probe pass and the same three filter passes. Their sampling, geometry rejection, contribution weighting, strides and spatial weights are unchanged. `NrdHistoryConfidenceFilterConstants` remains 64 bytes; strength occupies byte offset 56, previously padding, matching HLSL register c3.z.

In `CSFilter`, the last pass first executes the original calculation:

`c_generated = max(minConfidence, (1-saturate(sensitivity*r))^2)`

with existing minConfidence=0.1 and sensitivity=2. Only then, under an explicit `[branch] if(confidenceStrength > 1)`, it computes:

`c_test = max(minConfidence, pow(saturate(c_generated), float(confidenceStrength)))`.

The existing saturated final write follows. Neutral/no-measurement confidence starts at 1 and remains 1. The exponent is applied to the full-precision result of the original mapping before the existing R16_FLOAT storage conversion, as permitted by the requested final-write insertion. It does not exponentiate categorical state or raw change evidence.

The same half-resolution resource remains in previous-frame probe space. Its UAV-to-SRV transition, ownership, current-frame completion gate, NRD registration, previous-UV bilinear sampling, specular-one binding and missing-generation constant-one fallback are unchanged. No additional reprojection or correspondence history exists. Regular RELAX remains the only generated-confidence consumer; Hybrid and other rendering paths were not modified.

## 5. Baseline preservation evidence

- The original mapping block remains verbatim, including its arithmetic and final saturated write. A scratch source check compared it with the pre-edit shader snapshot and passed.
- The complete probe portion of the shader remains verbatim. No probe/visibility/light-sampling changes.
- Strength 1 skips the new exponent branch. DXC disassembly (`build/r9f/CSFilter.txt`) shows an unsigned strength>1 conditional branch around the Log/Exp instructions that implement pow. The baseline path reaches the existing texture store without those instructions.
- Original no-measurement initialization and mapping floor remain unchanged. CPU checks verify identity at strength 1 and exact neutral 1 for all supported exponents; GPU execution was not measured.
- Existing resource creation/release, Execute dispatch sites, generator enablement, NRD frame flag and confidence binding code were not changed. Filter upload size is unchanged. There is a new scalar load/branch when generation runs, but no new resources or dispatches; ordinary rendering without enabled confidence diagnostics does not run it.

This establishes source arithmetic preservation and compiled branch structure, not a rendered bit-for-bit GPU comparison across hardware/compiler drivers.

## 6. Numerical examples

Ideal mathematical values before half-float storage/interpolation:

| Original confidence | Strength 1 | Strength 2 | Strength 4 | Strength 6 |
|---:|---:|---:|---:|---:|
| 1.00 | 1.00 | 1.00 | 1.00 | 1.00 |
| 0.95 | 0.95 | 0.902500 | 0.814506 | 0.735092 |
| 0.90 | 0.90 | 0.810000 | 0.656100 | 0.531441 |
| 0.80 | 0.80 | 0.640000 | 0.409600 | 0.262144 |
| 0.60 | 0.60 | 0.360000 | 0.129600 | 0.100000 |
| 0.40 | 0.40 | 0.160000 | 0.100000 | 0.100000 |
| 0.20 | 0.20 | 0.100000 | 0.100000 | 0.100000 |
| 0.10 | 0.10 | 0.100000 | 0.100000 | 0.100000 |

The mapping is bounded by [0.1,1] before storage and never raises confidence for a supported exponent. Existing R16_FLOAT quantization remains: decimal 0.1 stores as approximately 0.0999756, as it already did in R.9A/B/C. No format/floor change was made. NRD subsequently saturates its sampled confidence; half-resolution interpolation still affects consumer values.

## 7. Build and shader compilation results

| Validation | Result / evidence |
|---|---|
| Debug Engine | PASS — `build/r9f/debug-build.log`. |
| Release Engine | PASS — `build/r9f/release-build.log`. |
| CSProbe | PASS — DXC cs_6_6, HLSL 2021. |
| CSFilter | PASS — same profile; disassembly confirms guarded exponent. |
| CSResolveRelaxSignalView | PASS — same profile; display shader executable behavior unchanged. |
| C++/HLSL constant-buffer layout | Existing C++ 64-byte static assertion passes in both builds; shader reflection/disassembly shows strength at offset 56. |
| Whitespace check | PASS on edited tracked sources. |

Engine commands: `cmake --build build --config Debug --target Engine -- /m /v:minimal` and the corresponding Release command. MSBuild ran with the required access for its file tracker. Both also built the EngineRuntime dependency. No warnings or errors were reported in the successful build logs.

Direct shader compilation used `build/_deps/dxc-src/bin/x64/dxc.exe`, source under `assets/Shaders`, include roots `assets/Shaders` and `build/Debug/assets/Shaders` for deployed RTXDI headers. Outputs/disassemblies are in `build/r9f/`.

## 8. Runtime checks performed

An isolated Windows C++ executable ran the **exact extracted production generated/strength-selector block**, with log capture, rather than a separately reimplemented parser. It exercised 510 combinations of generated selector, strength value, existing constant mode and independent probe selector. All passed: exact parsing, malformed warnings, generated dependency, effective mode, unchanged probe activation, generated-over-constant precedence and strengthened-View-5 logging.

The same executable passed 36,004 CPU numerical checks over [0.1,1] and exponents 1/2/4/6 for range, non-increasing confidence, strength-1 identity and neutral identity. Preparation also verified that the old mapping and complete probe body were preserved verbatim.

Reproduction: `python build/r9f/prepare_checks.py`, configure/build its ignored CMake scratch project, then run `build/r9f/check-build/Debug/R9FChecks.exe`. Results: `build/r9f/check-run.log`. Tests run in their own process; they do not change the user's shell or Engine process environment.

No full Engine launch was performed: an isolated rendering launch with guaranteed scratch-only settings/output had not been established. No D3D12 runtime transition, visual quality, resize/mode-switch or full-frame baseline-equivalence claim is made from compilation or the selector executable.

## 9. Remaining interactive tests

Use the original complex scene, one active point, HDRI None, no directionals, regular RELAX, identical camera framing/occluder motion and other settings. Restart between startup modes:

1. Generated enabled; strength unset or `1` as baseline.
2. Generated enabled; strength `2`.
3. Generated enabled; strength `4`.
4. Generated enabled; strength `6` if needed.

Compare moving-shadow responsiveness, residual lag after stopping, stationary noise and boundary flicker. Check Views 4/6 retain raw evidence/state; View 5 intentionally changes with strength. Inspect the effective startup mode and verify missing-probe fallback remains neutral. No visual improvement is claimed by this implementation report.

## 10. Known limitations

View 5 now displays the strengthened final texture in modes 2/4/6. No separate unstrengthened display texture was allocated. Its nearest-texel load and subsequent presentation/tone mapping still do not equal NRD's bilinearly sampled numerical confidence. The original unstrengthened view is obtained by restarting at strength 1.

The experiment changes strength only, not the duration of evidence, probe spatial coverage, indirect-light representativeness or RELAX history semantics. It cannot resolve R.9E's historical correspondence blocker and does not attempt to. Pow and R16_FLOAT introduce normal floating-point rounding in experimental modes. High strengths may increase noise; that tradeoff remains an interactive observation, not a production recommendation.

Only the six listed existing source/shader files changed; all other inventoried existing files were preserved. No staging or commit was performed. R.9F stops here.
