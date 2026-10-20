"""RTXPT portable Release runtime smoke: alive 8 s, closed by WM_CLOSE, runtime log captured.

Usage: portable_smoke.py <milestone> <game-dir> <scene-id> <name>
Output: build/<milestone>/portable/<name>.json and <name>-runtime.log. Exit 0 only if the runtime stayed
alive for 8 s and then exited 0.
"""
from pathlib import Path
import ctypes, json, shutil, subprocess, sys, tempfile, time
from ctypes import wintypes
from rtxpt_common import BUILD, PORTABLE_RUNTIME, fail, require

if len(sys.argv) != 5:
    fail(__doc__)

milestone, game, scene_id, name = sys.argv[1], Path(sys.argv[2]).resolve(), sys.argv[3], sys.argv[4]
require([PORTABLE_RUNTIME, game / 'Game.toc', game / 'Game.eap'], 'portable runtime or game archive')
out = BUILD / milestone / 'portable'
out.mkdir(parents=True, exist_ok=True)
command = [str(PORTABLE_RUNTIME), scene_id, str(game / 'Game.toc'), str(game / 'Game.eap')]
process = subprocess.Popen(command, cwd=out, creationflags=subprocess.CREATE_NO_WINDOW)
time.sleep(8)
alive = process.poll() is None
user32 = ctypes.WinDLL('user32', use_last_error=True)
callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

@callback_type
def close_window(window, _):
    owner = wintypes.DWORD()
    user32.GetWindowThreadProcessId(window, ctypes.byref(owner))
    if owner.value == process.pid:
        user32.PostMessageW(window, 0x0010, 0, 0)
    return True

user32.EnumWindows(close_window, 0)
try:
    code = process.wait(timeout=30)
except subprocess.TimeoutExpired:
    process.terminate()
    code = process.wait(timeout=10)
log = Path(tempfile.gettempdir()) / ('EngineRuntime-' + str(process.pid) + '.log')
tables = []
if log.exists():
    shutil.copy2(log, out / (name + '-runtime.log'))
    tables = [line.strip() for line in log.read_text(errors='replace').splitlines() if 'material tables' in line]
result = {'command': command, 'aliveAfter8s': alive, 'exit': code, 'materialTableLog': tables}
(out / (name + '.json')).write_text(json.dumps(result, indent=2))
print(json.dumps(result))
raise SystemExit(0 if alive and code == 0 else 1)
