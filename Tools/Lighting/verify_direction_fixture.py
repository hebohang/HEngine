"""Analytic end-to-end check of Cycles -> cube -> SH -> HEngine coordinates."""
import argparse
import json
from pathlib import Path

import numpy as np

from bake import verify
from lighting_math import sh_basis

parser=argparse.ArgumentParser()
parser.add_argument('--project',type=Path,required=True)
args=parser.parse_args()
root=args.project.resolve()
folder=root/'Assets/Generated/LightingFixture/DirectionBaked'
verify(root,folder/'manifest.json')
scene=json.loads((folder/'Scene.he').read_text())
coefficients=next(e['BakedIrradianceVolumeComponent']['IrradianceSH']
                  for e in scene.values() if 'BakedIrradianceVolumeComponent' in e)
directions=np.array([[1,0,0],[-1,0,0],[0,1,0],[0,-1,0],[0,0,1],[0,0,-1]])
actual=sh_basis(directions)@coefficients
expected=np.pi*.5+np.pi/3*directions
error=float(np.max(np.abs(actual-expected)))
if error>.035: raise RuntimeError(f'World/capture/SH direction or energy mismatch: {error:.6f}')
print(f'PASS: RGB-axis cosine irradiance, six normals, maximum absolute error {error:.6f}')
