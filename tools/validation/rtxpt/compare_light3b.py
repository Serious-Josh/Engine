"""RTXPT.1: compare reference ROI means with the LIGHT.3B legacy composed-HDR baselines (same fixtures/ROI).

Reads build/rtxpt1/scene-validation.csv (written by `run_gpu.py rtxpt1`) and build/light3b/baseline-*.csv;
writes build/rtxpt1/light3b-comparison.csv.
"""
import csv, io
from rtxpt_common import BUILD, require

root = BUILD / 'rtxpt1'
baselines = BUILD / 'light3b'
scenes = ('outdoor', 'opening1', 'opening5')
require([root / 'scene-validation.csv'] + [baselines / f'baseline-{scene}.csv' for scene in scenes], 'comparison inputs')
text = (root / 'scene-validation.csv').read_text()
section = text[text.index('scene,environment'):].split('\n\n')[0]
reference = {(r['scene'], r['environment'], r['model'], float(r['roughness'])): float(r['ptR'])
             for r in csv.DictReader(io.StringIO(section))}
rows = []
for scene in scenes:
    for r in csv.DictReader(open(baselines / f'baseline-{scene}.csv')):
        key = (scene, r['environment'], r['model'], float(r['roughness']))
        if key in reference:
            legacy = float(r['composedHDR'])
            rows.append((*key, legacy, reference[key], legacy / reference[key] if reference[key] else float('nan')))
out = open(root / 'light3b-comparison.csv', 'w')
out.write('scene,environment,model,roughness,legacyComposedR,referenceR,legacyOverReference\n')
for row in rows:
    out.write(','.join(str(v) for v in row) + '\n')
for scene in scenes:
    for env in ('Night', 'Woods'):
        ratios = [row[6] for row in rows if row[0] == scene and row[1] == env]
        print(f'{scene:9s} {env:6s} n={len(ratios):2d} legacy/reference min {min(ratios):.3f} max {max(ratios):.3f}')
