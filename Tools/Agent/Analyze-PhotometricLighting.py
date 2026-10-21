"""Check paired rendered photometric fixtures (tone-mapped sRGB evidence)."""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('directory', type=Path)
args = parser.parse_args()
root = args.directory
def read(name):
    return np.asarray(Image.open(root/(name+'.png')).convert('RGB'), dtype=np.float32)/255.0
def compare(first, second, tolerance, changed_tolerance):
    a, b = read(first), read(second)
    error = np.abs(a-b)
    changed = float(np.mean(np.max(error, axis=2) > tolerance))
    return {'first': first, 'second': second, 'meanAbsoluteSrgbError': float(error.mean()),
            'maximumSrgbError': float(error.max()), 'pixelTolerance': tolerance,
            'fractionBeyondTolerance': changed, 'allowedFraction': changed_tolerance,
            'pass': bool(changed <= changed_tolerance)}
checks = [compare('local-cd', 'local-lm', 1.0/255.0+1e-6, 0.001),
          compare('local-cd', 'local-unitless', 1.0/255.0+1e-6, 0.001),
          compare('local-cd', 'local-forward', 3.0/255.0+1e-6, 0.01),
          compare('sun-reference', 'sun-daylight', 3.0/255.0+1e-6, 0.01)]
local = read('local-cd')
checks.append({'case': 'Local-light equivalence has visible signal',
               'nonblackFraction': float(np.mean(local.max(axis=2)>4.0/255.0)),
               'pass': bool(np.mean(local.max(axis=2)>4.0/255.0)>0.01)})
high_ev = read('sun-high-ev-bloom')
checks.append({'case': 'EV100=20 with bloom remains nonblack', 'maximumSrgb': float(high_ev.max()),
               'nonblackFraction': float(np.mean(high_ev.max(axis=2)>0)), 'pass': bool(high_ev.max()>0)})
report = {'evidenceType': 'runtime-manual', 'interpretation': 'Paired rendered pixels; this does not establish cross-renderer image equivalence.',
          'checks': checks, 'pass': all(c['pass'] for c in checks)}
(root/'pixel-report.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
print(json.dumps(report, indent=2))
raise SystemExit(0 if report['pass'] else 1)
