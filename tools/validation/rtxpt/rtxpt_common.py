"""Shared paths and preflight checks for the RTXPT validation runners.

The runners live in source control, but the checks they drive are local Git-ignored test sources, and
their fixtures and outputs are in the ignored build/ tree. Every runner therefore checks its inputs
before it launches anything. If one is missing it names it and exits with MISSING_INPUT_EXIT, so an
explicitly requested validation can never pass vacuously.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / 'build'
DIAGNOSTICS_BUILD = BUILD / 'diagnostics-on'
EDITOR = DIAGNOSTICS_BUILD / 'Debug/Engine.exe'
DEBUG_RUNTIME = DIAGNOSTICS_BUILD / 'Game/Debug/EngineRuntime.exe'
# The assembled EnginePortable runtime (AssembleEnginePortable writes <portable build>/Portable/Release). Not
# build/Portable/Release, which Windows resolves to the portable build tree's own, unassembled Release output.
PORTABLE_RUNTIME = BUILD / 'portable/Portable/Release/Game/EngineRuntime.exe'
GPU_PROJECT = BUILD / 'h7/gpu-project'
MISSING_INPUT_EXIT = 2

# The Git-ignored sources the RTXPT GPU suites compile or copy at run time.
TEST_SOURCES = [
    'src/Engine/tests/SceneHierarchyVerification.cpp',
    'src/Engine/tests/PathTraceReferenceVerification.inl',
    'src/Engine/tests/MaterialModelInvestigation.inl',
    'src/Engine/tests/shaders/PathTraceBSDFValidation.hlsl',
    'src/Engine/tests/shaders/PathTraceRasterPrimaryValidation.hlsl',
    'src/Engine/tests/shaders/PathTraceBSDFRTXPT1.hlsli',
    'src/Engine/tests/shaders/PathTraceBSDFCandidate.hlsli',
    'src/Engine/tests/shaders/MaterialInvestigationBody.hlsli',
    'src/Engine/tests/shaders/MaterialInvestigationConstants.hlsli',
    'src/Engine/tests/shaders/MaterialInvestigationCurrent.hlsl',
    'src/Engine/tests/shaders/MaterialInvestigationCandidate.hlsl',
]


def fail(message):
    print('RTXPT validation unavailable: ' + message, file=sys.stderr, flush=True)
    raise SystemExit(MISSING_INPUT_EXIT)


def require(paths, what):
    """Exits with MISSING_INPUT_EXIT, listing every absent path, unless all of them exist."""
    missing = [str(Path(path)) for path in paths if not (ROOT / path).exists()]

    if missing:
        fail('missing ' + what + ':\n  ' + '\n  '.join(missing))


def require_diagnostics_editor():
    """The verification entry points exist only in a Debug build configured with ENGINE_BUILD_DIAGNOSTICS=ON."""
    require([EDITOR], 'diagnostics Engine.exe (configure build/diagnostics-on with ENGINE_BUILD_DIAGNOSTICS=ON)')
    cache = DIAGNOSTICS_BUILD / 'CMakeCache.txt'
    lines = cache.read_text(errors='replace').splitlines() if cache.exists() else []

    if not any(line.startswith('ENGINE_BUILD_DIAGNOSTICS:') and line.endswith('=ON') for line in lines):
        fail(str(cache) + ' does not set ENGINE_BUILD_DIAGNOSTICS=ON; Engine.exe has no verification entry points.')
