#!/usr/bin/env python3
"""Summarize measured decoded texture errors without claiming rendered QA."""
import argparse
import math
from pathlib import Path
import numpy as np
from PIL import Image
from compress_benchmark_textures import read,write,PROJECT


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--variant',type=Path,required=True)
    args=parser.parse_args()
    output=args.variant.resolve()
    compression=read(output/'compression.json')
    records=list(compression['textures'].values())
    masks=[]
    for record in records:
        quality=record['qualityMip0']
        if 'alphaCutoff' not in quality:
            continue
        source=np.asarray(Image.open(record['source']).convert('RGBA'),dtype=np.float32)/255
        actual=np.asarray(Image.open(PROJECT/record['output']).convert('RGBA'),dtype=np.float32)/255
        accepted=source[...,3]>=quality['alphaCutoff']
        mse=float(np.mean((source[...,:3][accepted]-actual[...,:3][accepted])**2)) if np.any(accepted) else 0
        coverage=quality['sourceMip0Coverage']
        coverage_rows=record['alphaMipCoverage']
        masks.append({'texture':record['output'],'acceptedRgbPsnrDb':10*math.log10(1/mse) if mse else None,
                      'mip0CoverageAbsoluteDelta':abs(quality['compressedMip0Coverage']-coverage),
                      'maxCoverageAbsoluteDeltaAt16pxOrAbove':max((abs(row['coverage']-coverage) for row in coverage_rows if min(row['width'],row['height'])>=16),default=0),
                      'allMipCoverage':coverage_rows})
        deviations=[abs(row['coverage']-coverage) for row in coverage_rows if min(row['width'],row['height'])>=32]
        masks[-1]['maxCoverageAbsoluteDeltaAt32pxOrAbove']=max(deviations,default=0)
        masks[-1]['coverageScreening']='FAIL' if masks[-1]['mip0CoverageAbsoluteDelta']>0.005 or max(deviations,default=0)>0.02 else 'PASS'
    rgb=[r for r in records if r['qualityMip0'].get('rgbPsnrDb') is not None and 'alphaCutoff' not in r['qualityMip0']]
    normals=[r for r in records if 'normalDegreesMean' in r['qualityMip0']]
    summary={'evidenceType':'unit-test','kind':'decoded texture-space error measurements; no scene rendering or temporal QA claim',
             'command':'python Tools/Assets/RenderingBenchmarks/summarize_compressed_quality.py --variant '+str(args.variant),
             'textureCount':len(records),'payloadBytes':compression['texturePayloadBytes'],
             'nonMaskRgbMinimumPsnrDb':min((r['qualityMip0']['rgbPsnrDb'] for r in rgb),default=None),
             'normalMaximumPerTextureMeanDegrees':max((r['qualityMip0']['normalDegreesMean'] for r in normals),default=None),
             'normalMaximumPerTextureP99Degrees':max((r['qualityMip0']['normalDegreesP99'] for r in normals),default=None),
             'maskTextures':masks,'maskPolicy':'Rejected RGB is deliberately filled; visible-texel RGB error and alpha coverage are reported separately.',
             'maskCoverageScreening':{'mip0ToleranceAbsolute':0.005,'mipsAtLeast32pxToleranceAbsolute':0.02,
                                      'failures':[m['texture'] for m in masks if m['coverageScreening']=='FAIL'],
                                      'interpretation':'Declared offline screening limits, not a rendered temporal stability test. Coverage deltas are fractions (0.02 = two percentage points).'},
             'tinyMipPolicy':'Smallest mip alpha quantization can only represent discrete coverage fractions; all values remain disclosed.',
             'runtimeValidation':'Pending separate HGame capture and motion comparison'}
    write(output/'texture_quality_summary.json',summary)
    print({k:v for k,v in summary.items() if k not in ['maskTextures']})
    print('Mask textures:',len(masks),'max mip0 coverage delta:',max((m['mip0CoverageAbsoluteDelta'] for m in masks),default=0))


if __name__=='__main__':
    main()
