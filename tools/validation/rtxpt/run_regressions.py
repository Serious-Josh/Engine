"""RTXPT regressions: the LIGHT.3A final set, unchanged, with private output under build/<milestone>/regressions.

Usage: run_regressions.py <milestone> <camera-games> [case ...]
  milestone     output directory under build/, e.g. rtxpt1 or rtxpt1a2
  camera-games  directory holding CameraGame, CameraFreeGame and DisabledMainGame (the CameraDataTests output)
  case          optional subset; results from earlier runs are kept in results.json
"""
from pathlib import Path
import json, shutil, subprocess, sys, tempfile
from rtxpt_common import BUILD, DEBUG_RUNTIME, EDITOR, GPU_PROJECT, TEST_SOURCES, fail, require, require_diagnostics_editor

if len(sys.argv) < 3:
    fail(__doc__)

out = BUILD / sys.argv[1] / 'regressions'
games = Path(sys.argv[2]).resolve()
only = set(sys.argv[3:])
membership = BUILD / 'e85-membership/Project'
h9 = BUILD / 'h9/HierarchyPhysicsGame'
light_games = BUILD / 'light1/light-data'
historical = BUILD / 'light1/historical/CameraGameV10'
require(TEST_SOURCES + [DEBUG_RUNTIME, GPU_PROJECT, membership] +
    [archive / name for archive in (h9, light_games / 'LightGame', historical) for name in ('Game.toc', 'Game.eap')] +
    [games / title / name for title in ('CameraGame', 'CameraFreeGame', 'DisabledMainGame')
        for name in ('Game.toc', 'Game.eap')], 'regression inputs')
require_diagnostics_editor()

fixtures = out / 'fixtures'
editor = EDITOR
runtime = DEBUG_RUNTIME
camera_scene = '12345678-1234-4234-8234-123456789abc'
cases = [(name, editor, ['--verify-' + name], 0) for name in [
    'scene-physics-data', 'scene-physics-persistence', 'scene-hierarchy', 'scene-lifecycle', 'primitive-creation',
    'physics-simulation', 'packaged-physics', 'project-independent-startup', 'project-browser',
    'asset-browser-scenes']]
cases += [
    ('startup-scene', editor, ['--verify-startup-scene', str(out / 'startup-games')], 0),
    ('project-system-workflow-first', editor, ['--verify-project-system-workflow', 'first', str(out / 'workflow')], 0),
    ('project-system-workflow-reopen', editor, ['--verify-project-system-workflow', 'reopen', str(out / 'workflow')], 0),
    ('editor-grid', editor, ['--verify-editor-grid'], 0),
    ('viewport-marquee', editor, ['--verify-viewport-marquee'], 0),
    ('viewport-pan', editor, ['--verify-viewport-pan'], 0),
    ('viewport-grid-snap', editor, ['--verify-viewport-grid-snap'], 0),
    ('scene-membership', editor, ['--verify-scene-membership', str(fixtures / 'Membership/Scenes/Membership.scene')], 0),
    ('scene-hierarchy-gpu', editor, ['--verify-scene-hierarchy-gpu', str(fixtures / 'gpu-project/Scenes/Hierarchy.scene')], 0),
    ('packaged-bootstrap-h9', editor, ['--verify-packaged-bootstrap', str(h9 / 'Game.toc'), str(h9 / 'Game.eap'),
        '147a2dcf-55b7-5926-aad8-3912482ae702'], 0),
    ('runtime-h9-v9-archive', runtime, ['--verify-bootstrap', '147a2dcf-55b7-5926-aad8-3912482ae702',
        str(h9 / 'Game.toc'), str(h9 / 'Game.eap')], 0)]
# Current-format archives: Main renders; camera-free and disabled-Main are refused at startup.
cases += [('runtime-' + title, runtime, ['--verify-bootstrap', camera_scene, str(games / title / 'Game.toc'),
    str(games / title / 'Game.eap')], code) for title, code in
    (('CameraGame', 0), ('CameraFreeGame', 1), ('DisabledMainGame', 1))]
# LIGHT.1: current v11 Light archive renders; a real CAM.2-era v10 archive still boots unchanged.
light_scene = '22345678-1234-4234-8234-123456789abc'
cases += [
    ('runtime-LightGame-v11', runtime, ['--verify-bootstrap', light_scene, str(light_games / 'LightGame/Game.toc'),
        str(light_games / 'LightGame/Game.eap')], 0),
    ('runtime-CameraGame-v10-historical', runtime, ['--verify-bootstrap', camera_scene, str(historical / 'Game.toc'),
        str(historical / 'Game.eap')], 0),
    ('packaged-bootstrap-v10-historical', editor, ['--verify-packaged-bootstrap', str(historical / 'Game.toc'),
        str(historical / 'Game.eap'), camera_scene], 0)]
# The scene-hierarchy-gpu case builds these two archives into the fixtures directory.
for title in ('LocalLightGIGame', 'LocalLightGISpotGame'):
    local_game = fixtures / title
    cases.append(('runtime-' + title, runtime, ['--verify-bootstrap',
        '79a73add-8065-570e-b92f-a2c88c6238aa', str(local_game / 'Game.toc'), str(local_game / 'Game.eap')], 0))
if only:
    unknown = only - {case[0] for case in cases}
    if unknown:
        fail('unknown case(s): ' + ', '.join(sorted(unknown)))
    cases = [case for case in cases if case[0] in only]

# Inputs and case names are validated; only now is the output tree reset.
if out.exists() and not only:
    shutil.rmtree(out)
(out / 'build').mkdir(parents=True, exist_ok=True)
if not (fixtures / 'Membership').exists():
    shutil.copytree(membership, fixtures / 'Membership')
if (fixtures / 'gpu-project').exists():
    shutil.rmtree(fixtures / 'gpu-project')
shutil.copytree(GPU_PROJECT, fixtures / 'gpu-project')
results_path = out / 'results.json'
results = json.loads(results_path.read_text()) if only and results_path.exists() else {}

for name, exe, args, expected in cases:
    with (out / (name + '.out')).open('w') as log:
        try:
            process = subprocess.Popen([str(exe), *args], cwd=out, stdout=log, stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW)
            result = process.wait(timeout=900)
            runtime_log = Path(tempfile.gettempdir()) / ('EngineRuntime-' + str(process.pid) + '.log')
            if exe.name == 'EngineRuntime.exe' and runtime_log.exists():
                shutil.copy2(runtime_log, out / (name + '-runtime.log'))
        except subprocess.TimeoutExpired:
            result = 'timeout'
    results[name] = {'exit': result, 'expected': expected, 'ok': result == expected}
    results_path.write_text(json.dumps(results, indent=2))
    print(name, result, 'ok' if result == expected else 'UNEXPECTED', flush=True)

raise SystemExit(any(not result['ok'] for result in results.values()))
