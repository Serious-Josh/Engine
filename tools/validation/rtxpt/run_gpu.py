"""RTXPT GPU suites: a fresh copy of the H.7 fixture, the scene-hierarchy GPU group plus the selected suite.

Usage: run_gpu.py <suite> <name> [NAME=VALUE ...]
  rtxpt1                 RTXPT.1 reference path tracer suite, including the RTXPT.1A.2 table checks
  rtxpt1a                RTXPT.1A material model investigation
  rtxpt1a1 <table.emt>   RTXPT.1A.1 GPU validation of a table file (pass the table as the third argument)
  rtxpt1a2-investigation RTXPT.1A investigation against the renderer's published tables
Output: build/<suite directory>/run/<name>.out, with an exit= trailer; matching lines are echoed.

The suite is enabled by an environment variable that an Engine built without its sources ignores. A run
counts as passed only when the exit code is 0 and the suite's completion markers are present.
"""
from pathlib import Path
import os, re, shutil, subprocess, sys
from rtxpt_common import BUILD, EDITOR, GPU_PROJECT, TEST_SOURCES, fail, require, require_diagnostics_editor

SUITES = {
    'rtxpt1': ('rtxpt1', {'ENGINE_RTXPT1': '1'}, 'RTXPT|FAIL|exit=',
        ['PASS: RTXPT.1A.2 renderer publishes', 'RTXPT.1 D3D12 debug-layer errors before/after']),
    'rtxpt1a': ('rtxpt1a', {'ENGINE_RTXPT1A': '1'}, 'RTXPT.1A|FAIL|exit=',
        ['PASS: RTXPT.1A investigation complete']),
    'rtxpt1a1': ('rtxpt1a1', {'ENGINE_RTXPT1A': '1'}, 'RTXPT.1A|FAIL|exit=',
        ['PASS: RTXPT.1A.1 bundled table uploaded', 'PASS: RTXPT.1A investigation complete']),
    'rtxpt1a2-investigation': ('rtxpt1a2', {'ENGINE_RTXPT1A': '1', 'ENGINE_RTXPT1A2': '1'}, 'RTXPT.1A|FAIL|exit=',
        ['PASS: RTXPT.1A.1 bundled table uploaded', 'PASS: RTXPT.1A investigation complete']),
}

if len(sys.argv) < 3 or sys.argv[1] not in SUITES:
    fail(__doc__)

suite, name, extra = sys.argv[1], sys.argv[2], sys.argv[3:]
directory, environment, pattern, markers = SUITES[suite]
environment = dict(environment)

if suite == 'rtxpt1a1':
    if not extra:
        fail('rtxpt1a1 needs the table file to validate.')
    require([extra[0]], 'table file')
    environment['ENGINE_RTXPT1A1_TABLE'] = extra.pop(0)

for assignment in extra:
    key, separator, value = assignment.partition('=')
    if not separator:
        fail('expected NAME=VALUE, got ' + assignment)
    environment[key] = value

require(TEST_SOURCES + ['assets/MaterialTables/GGXSingleScatterComplement.emt', GPU_PROJECT], 'RTXPT test inputs')
require_diagnostics_editor()

work = BUILD / directory
fixture = work / 'fixtures/gpu-project'
shutil.rmtree(fixture, ignore_errors=True)
shutil.copytree(GPU_PROJECT, fixture)
(work / 'run').mkdir(parents=True, exist_ok=True)
output = work / 'run' / (name + '.out')

with output.open('w') as log:
    try:
        code = subprocess.run([str(EDITOR), '--verify-scene-hierarchy-gpu', str(fixture / 'Scenes/Hierarchy.scene')],
            cwd=work / 'run', env={**os.environ, **environment}, stdout=log, stderr=subprocess.STDOUT,
            timeout=5400).returncode
    except subprocess.TimeoutExpired:
        code = 'timeout'

with output.open('a') as log:
    log.write('exit=' + str(code) + '\n')

text = output.read_text(errors='replace')
matcher = re.compile(pattern)

for number, line in enumerate(text.splitlines(), 1):
    if matcher.search(line) and 'asymmetric pair' not in line:
        print(str(number) + ':' + line[:600])

absent = [marker for marker in markers if marker not in text]

if absent:
    print('FAIL: suite did not run to completion; missing markers: ' + '; '.join(absent), flush=True)

raise SystemExit(0 if code == 0 and not absent else 1)
