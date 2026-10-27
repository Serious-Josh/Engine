# Scene lighting infrastructure report

L.1 and L.2 are implemented. The approved ownership changes resolve the original migration audit findings. The targeted regression suite passes 821 checks; Debug and Release Engine builds pass. A controlled point-light-only project is generated for interactive rendering checks, which remain outstanding.

## L.1 files changed

- `CMakeLists.txt`: diagnostics build target for the environment regression executable; previous local edits preserved.
- `src/Scene/SceneDocument.h`, `src/Scene/SceneDocument.cpp`: explicit `builtin:none` selection and schema 12 validation.
- `src/Scene/SceneSerializer.cpp`: admit schema 12; retain historical references and neutral semantics.
- `src/Engine/SceneEnvironment.cpp`: route None through the editor's existing assignment/activation workflow.
- `src/Engine/BuildGame.cpp`: built-in None needs no imported/cooked HDR asset.
- `src/Renderer/BuiltinEnvironment.h`: built-in RGBA32F-compatible black equirectangular image, 16 by 8, alpha one.
- `src/Renderer/EnvironmentMap.cpp`: accept zero-energy images, keep valid CDF buffers without dividing by zero; validate dimensions/storage.
- `src/Renderer/Renderer.cpp`: discover None, upload through normal resource management, display packaged None, and keep the identity cache correct when switching back to loose HDRIs. Previous local renderer edits preserved.
- `src/assets/CookedScene.h`, `src/assets/CookedScene.cpp`: version 12 with a mutually exclusive None flag, no external environment binding; versions 1 through 11 remain readable.
- `src/assets/CookedEnvironment.cpp`: reconstruct packaged None from the built-in source.
- `tools/validation/SceneLightingEnvironmentCheck.cpp`: persistent regression check source.
- `docs/SceneLightingInfrastructureReport.md`: this report.

## L.1 behavior and renderer audit

The empty reference continues to mean the historical gray neutral environment; assigned canonical asset references retain their meaning. None is explicitly `builtin:none` and survives loose save/reload and cooked archive loading. L.1 introduced version 12; L.2 now writes version 13 while retaining the same None semantics. Loading does not rewrite source files.

The existing HDRI combo consumes renderer-discovered entries, so appending None exposes it without changing the diagnostic UI or the original default HDRI ordering. File-backed assignments persist the reference through normal scene save. Packaged environments remain immutable as before.

There was no reusable black environment meeting all required bindings. The implementation uploads a built-in black image through EnvironmentMap, preserving its source texture, mip chain, cube, diffuse cube/array, GGX multiscatter cube and CDF descriptor contracts. The normal preprocessing passes operate on the black source. Their RGB integrations are linear in source radiance and yield zero for black. No new shading model or shader changes were introduced.

Raster deferred ambient, background, reflections and GI retain their existing bindings. The path tracer and GI sampling already guard against nonpositive importance weight. RTXDI's production BuildRtxdiLightSet also requires positive environment weight, so valid descriptors with totalWeight zero do not create an environment record. Environment switching drains the GPU and uses the existing preprocessing, lighting invalidation and history-reset paths. The loose-HDRI setter now updates the same identity cache used by None, preventing a None/HDRI/None switch from incorrectly skipping the final replacement.

## L.1 validation

The Debug regression executable passed 579 checks on the local D3D12 device with the debug layer enabled when available. It checked:

- Loose None save/reload and legacy schema-11 neutral/assigned-reference compatibility.
- Cooked None roundtrip, archive open/reference validation and built-in packaged black pixels.
- Version-11 cooked compatibility and rejection of conflicting environment flags.
- Assigned HDRI cooking and preservation of decoded source pixels.
- Actual GPU upload of black, neutral and assigned HDRI resources over three cycles.
- Valid source, derived-resource and CDF descriptors; zero black importance weight and positive neutral/HDRI weight.
- Descriptor counts returning to their initial value after every release/switch.
- The production scene snapshot, frame lighting and RTXDI light-set builders producing exactly the intended point-light record when legacy sun intensity is explicitly zero and the environment is black.
- Environment record restoration for a positive-energy HDRI, and removal of its previous/current RTXDI history mapping when switching to None.
- No D3D12 error/corruption messages in the exercised resource operations.

The test does not execute full renderer frames or environment convolution passes. It does not establish pixelwise visual equivalence, full-frame synchronization correctness, interactive combo operation, or rendered local-light behavior in all renderer modes. Those remain interactive/runtime verification items.

Build and execution logs:

- `build/lighting-l1-build.log`: final Debug Engine and EngineRuntime build passed.
- `build/lighting-l1-release-build.log`: Release Engine and EngineRuntime build passed.
- `build/lighting-l1-tests-build.log`: Debug diagnostics regression build.
- `build/lighting-l1-tests-run.log`: 579 successful regression checks.

Run the check with `SceneLightingEnvironmentTests.exe <scratch-directory>` from the diagnostics Debug binary directory. The executable writes only its scratch fixtures.

## L.2 Gate 1: ambient semantics and feasibility

Every production `ambientStrength` use was traced before editing. It was a persisted legacy sun field, copied by scene preparation/publication/capture, edited in the legacy inspector, and uploaded into `DirectionalLightConstants`. DeferredLighting.hlsl, RayTracedGI.hlsl and RayTracedReflection.hlsl each declare that member but never read it. The path-tracing and RTXDI lighting estimators do not use it. Keeping the existing constant-buffer layout and forwarding a scene-owned value therefore preserves its current meaning: authored, saved data with no current shading effect. No shader behavior was added.

| Situation | Preserved ambient behavior |
| --- | --- |
| Legacy sun intensity is zero | Ambient strength migrates independently; zero intensity does not discard it. |
| Directional objects already exist | Their existing precedence is preserved; legacy ambient becomes a scene setting, and the previously inherited radius is copied into directional components. |
| Final directional is removed | Scene ambient remains saved and editable; no sun storage remains in the committed file scene. |
| HDRI None | Ambient strength cannot synthesize environment radiance; the established L.1 black resource behavior remains unchanged. |
| Legacy raster, reflections and RTGI | The same scalar reaches the same unused constant-buffer slot. |
| Reference, raster-primary and RTXDI path tracing | Ambient remains unused, as before. |

Historical sun validation allowed any finite signed RGB and any finite nonnegative intensity, exceeding component RGB [0,1] and intensity [0,1000000]. Directional component validation now admits those legacy values exactly, without normalization, intensity rescaling or clamping. Point/Spot limits remain unchanged. A type change to Point/Spot can be rejected when its retained values exceed those types' limits; it never silently clamps them. Existing float multiplication behavior at extreme radiometry is unchanged.

## L.2 ownership and compatibility implementation

`SceneDocument` and `PreparedScene` now contain independent `SceneLightingSettings`, not a legacy directional-light record. Current loose schema and cooked payload version are 13. Current loose output writes `lightingSettings.ambientStrength` and ordinary Light components, each including `angularRadius`. Current cooked output writes the scene ambient scalar and 40-byte Light bodies. Historical 11/12 components retain their 36-byte reader.

Both loose and cooked codecs consume legacy sun data only at the historical-input boundary. They immediately migrate it in memory and retain no second authoritative sun in the document. Source files are not rewritten by loading; only explicit save writes the new representation. Cooked metadata retains its source payload version for archive validation, while its in-memory authored document uses the current schema.

Migration rules:

- Historical root sun with no Directional components: append one root, meshless Directional Light object at position (0,0,0), with unit scale and Euler orientation whose world +Z reproduces the normalized legacy travel direction. Copy color, intensity and angular radius exactly. Ambient becomes scene-owned. Zero-intensity suns retain their zero intensity as an editable object.
- Historical scenes with any Directional component, including disabled components: preserve their IDs, order, parent relationships, poses, enabled states, colors and intensities. Those objects already suppressed the legacy root; do not append a second sun. Copy the previously inherited legacy radius into every Directional component, so present and subsequent shadow owners preserve that value. The inactive root's direction/color/intensity are intentionally superseded by the existing object representation.
- No legacy root and no Directional objects: create no default sun.
- Current `lightingSettings` plus objects take precedence over a stale legacy root when both are present. A current scene does not migrate that inactive root again.
- Migrated root IDs are deterministic and collision-checked. Current save/load cycles retain those IDs and never rerun migration.

The runtime Scene discards its optional procedural-demo sun storage when file-scene settings are committed. File lighting capture is authoritative even when empty. Renderer error and missing-snapshot paths also stay dark for these scenes. Deleting/disabling the final light cannot resurrect legacy energy. Procedural demos and their explicit diagnostic snapshots retain their pre-existing compatibility API; this is not a persisted or hidden fallback for migrated scenes.

The existing 64-record GPU light table and separate sun slot are unchanged. Authoring/validation now allow 65 components when one can occupy that directional slot, otherwise 64. This accommodates a formerly full 64-light table plus its migrated legacy sun without a renderer-capacity redesign. The default cooked object admission bound increases from 8192 to 8193 to accommodate that one appended migration object in a formerly maximum-size scene.

Undefined historical inputs are diagnosed rather than assigned a different lit direction: an emitting legacy sun whose travel vector cannot be normalized is rejected with an explicit migration error. Such a vector has no representable orientation; zero-energy variants remain dark and editable. No substitute emitting direction or silent radiometric clamp is applied.

## L.2 renderer and editor integration

The first enabled Directional component in document order remains the owner of the existing sun/shadow/secondary-lighting path. Other directionals retain their existing table-light behavior; their angular radii are persisted but affect the dedicated sun path only if they become owner. This preserves the established multiple-light treatment and inspector explanation rather than redesigning multiple-sun shadowing.

The resolved owner now supplies its angular radius to FrameLighting. Existing shadow constants, path-traced sun sampling and MakeRtxdiSunSource consume that derived frame value. The path-trace accumulation/lighting keys already include radius, and RTXDI sun identity includes the resulting cone and object ID. Removing the owner or changing its cone invalidates the corresponding previous/current light mapping. The shadow history comparison now includes angular radius as well as direction, preventing reuse of the preceding cone's shadow estimate. No NRD/confidence code was changed.

Directional objects continue through the existing hierarchy, transform, light-component creation, duplication/deletion and value-history workflows. The inspector exposes angular radius in degrees and numeric linear RGB for directional colors, allowing signed/HDR legacy colors to remain editable. Component equality/history includes angular radius. The file-scene panel is now Scene Lighting, with independent ambient settings and guidance to edit directional objects through the hierarchy. Ambient retains the previous scene-setting editing behavior; no unrelated history feature was introduced.

## L.2 files changed

- `src/Scene/SceneLightingSettings.h` (new): independent ambient settings.
- `src/Scene/LegacyLightingMigration.h` (new): shared codec-boundary legacy migration.
- `src/Scene/LightComponent.h`, `SceneDocument.h`, `SceneDocument.cpp`, `SceneSerializer.cpp`: ownership, validation, schema and authored fields.
- `src/assets/CookedScene.h`, `CookedScene.cpp`: current/legacy cooked layouts and migration with aligned mesh-binding slots.
- `src/Scene/Scene.h`, `Scene.cpp`, `PreparedScene.h`, `PreparedScene.cpp`, `SceneCommit.cpp`, `SceneMetadataPreparation.cpp`, `SceneMembershipPublication.cpp`, `SceneDocumentAdapter.cpp`, `FileSceneRuntime.cpp`, `SceneLightCapture.cpp`: settings publication, capture, dirty/equality checks and runtime ownership.
- `src/Scene/SceneLighting.h`, `SceneLighting.cpp`, `src/Renderer/FrameLighting.h`, `FrameLighting.cpp`: authoritative file lighting snapshots and owner radius.
- `src/Renderer/Renderer.h`, `Renderer.cpp`, `src/Engine/Engine.cpp`: dark file-scene fallback, radius-aware shadow history and capture-failure settings.
- `src/Editor/Editor.h`, `Editor.cpp`, `LightControls.cpp`, `ScenePrimitiveCreation.cpp`, `src/DemoContent/FileBackedDemoScene.cpp`: inspector, creation limits and compatibility hook.
- `tools/validation/SceneLightingEnvironmentCheck.cpp`: expanded existing L.1 regression executable and controlled project generator.
- Local optional verification fixtures were adapted to the new authored API: `src/Engine/tests/PresentationTransformVerification.cpp`, `ScenePhysicsPersistenceVerification.cpp`, `LightDataTests.cpp`, `LocalLightGIVerification.inl`, `EnvironmentVisibilityVerification.inl`. No NRD or confidence verification implementation was edited. These local fixture files follow the repository's existing ignored-test convention.
- `docs/SceneLightingInfrastructureReport.md`: this update.

Existing unrelated edits in shared files were preserved. L.1 environment resource/selection behavior was not reimplemented or redesigned.

## L.2 validation and builds

Final targeted run: **821 checks passed** using `SceneLightingEnvironmentTests.exe build/lighting-l2-tests` (absolute scratch path supplied in the actual invocation). The mesh importer's directory pinning required running this fixture generator outside the sandbox. The executable creates only scratch fixtures and normal temporary primitive staging.

Coverage includes:

- Loose and cooked versions 1 through 12, plus legacy archive reference validation.
- Historical 36-byte Directional component records in cooked versions 11/12.
- Preserved direction, signed/out-of-range color, intensity, radius and ambient; +Z cardinal/pole orientation checks; deterministic ID collision handling; source files unchanged during load.
- Existing directional IDs and parent relationships, disabled-object precedence, no-root input, and deterministic current-over-legacy precedence.
- Three current save/load cycles and exact current cooked roundtrips with no duplicate lights or legacy root output.
- Actual committed FileSceneRuntime capture, component radius/intensity edits, before/after undo/redo value transactions, rotation, translation without direction changes, disable/re-enable, and component removal.
- Actual runtime removal with HDRI None and one remaining point light: production BuildRtxdiLightSet contains exactly the intended point-light identity and no Sun or Environment. Its removed-sun history mapping is -1 while the point-light mapping survives.
- Radius changes invalidate the sun's RTXDI history mapping.
- Ordinary hierarchy light creation, duplication, owner promotion after deletion and final-object deletion with no fallback.
- Capacity preservation for 64 table lights plus the migrated sun.
- The existing L.1 GPU upload/descriptors, HDRI/None resource switching, environment exclusion and descriptor-cleanup checks.

Build results:

| Build/check | Result | Log |
| --- | --- | --- |
| Debug Engine and EngineRuntime | Passed | `build/lighting-l2-debug-build.log` |
| Release Engine and EngineRuntime | Passed | `build/lighting-l2-release-build.log` |
| Debug targeted regression build/run | Passed, 821 checks | `build/lighting-l2-tests-build.log`, `build/lighting-l2-tests-run.log` |
| Optional Debug EngineVerification library compile | Passed | `build/lighting-l2-verification-build.log` |

The broader optional verification executables were not run. The verification-library compile reports the existing wchar-to-char conversion warning in its scene-hierarchy unit; no build errors remained.

## Controlled integration project and remaining visual checks

Generated project: `build/lighting-l2-tests/ControlledProject/ControlledLighting.engineproj`.

Startup scene: `Scenes/ControlledPointOnly.scene`, containing a roughness-0.8, metallic-0 floor, a freely movable occluder, one enabled point light, a Main Camera, no Directional components and HDRI None. Its cube source uses the established primitive mesh import service, and its material and assets use normal project registry entries. The regression suite recreates this project in its scratch directory and checks its actual production RTXDI light-set construction, not an editor light count.

Outstanding interactive checks: open this project; render Legacy raster, RTGI/reflections, reference, raster-primary and RTXDI modes; compare migrated scenes visually against prior output; exercise inspector gestures, hierarchy selection and keyboard undo/redo; inspect shadow softness after radius edits and after owner deletion. No full renderer frames, convolution execution, packaged gameplay frames or visual image comparisons were run in this continuation. Resource-upload/debug-layer checks and runtime/frame-input tests do not substitute for those visual and full-frame synchronization checks.

## Scope and repository state

No NRD, confidence generator, confidence diagnostic view or confidence-probe implementation was edited by this task. The workspace already contained unrelated changes in those areas; they were preserved. No procedural sky, renderer redesign, speculative optimization or new lighting model was added. No files were staged or committed.
