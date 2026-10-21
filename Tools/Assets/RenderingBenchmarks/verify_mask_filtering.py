#!/usr/bin/env python3
"""Measure point and periodic bilinear alpha coverage, without rendered claims."""
import argparse
from pathlib import Path
import numpy as np
from PIL import Image
from compress_benchmark_textures import read,write,PROJECT
from repair_mask_mips import decode


def bilinear_coverage(alpha, cutoff, grid=4):
    covered = 0
    for iy in range(grid):
        v=(iy+.5)/grid-.5
        sy=-1 if v >= 0 else 1
        rows=alpha*(1-abs(v))+np.roll(alpha,sy,axis=0)*abs(v)
        for ix in range(grid):
            u=(ix+.5)/grid-.5
            sx=-1 if u >= 0 else 1
            sample=rows*(1-abs(u))+np.roll(rows,sx,axis=1)*abs(u)
            covered += int(np.sum(sample >= cutoff))
    return covered/(alpha.size*grid*grid)


def sample_periodic(alpha, sample_width, sample_height):
    height,width=alpha.shape
    x=(np.arange(sample_width,dtype=np.float32)+.5)/sample_width*width-.5
    y=(np.arange(sample_height,dtype=np.float32)+.5)/sample_height*height-.5
    x0,y0=np.floor(x).astype(int),np.floor(y).astype(int)
    tx,ty=x-x0,y-y0
    top=alpha[y0%height][:,x0%width]*(1-tx)+alpha[y0%height][:,(x0+1)%width]*tx
    bottom=alpha[(y0+1)%height][:,x0%width]*(1-tx)+alpha[(y0+1)%height][:,(x0+1)%width]*tx
    return top*(1-ty[:,None])+bottom*ty[:,None]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--variant',type=Path,required=True)
    args=parser.parse_args(); output=args.variant.resolve()
    report=read(output/'compression.json'); results=[]
    for repair in report['maskRepair']['textures']:
        record=report['textures'][repair['texture']]
        cutoff=record['qualityMip0']['alphaCutoff']
        source=np.asarray(Image.open(record['source']).convert('RGBA'),dtype=np.float32)[...,3]/255
        reference=bilinear_coverage(source,cutoff)
        original=(PROJECT/record['baselineOutput']).read_bytes()
        fixed=(PROJECT/record['output']).read_bytes()
        offset=148; rows=[]; sampled_mips={}
        point_target=record['qualityMip0']['sourceMip0Coverage']
        for mip in repair['mips']:
            w,h=mip['width'],mip['height']; size=((w+3)//4)*((h+3)//4)*16
            before=decode(original[:148],original[offset:offset+size],w,h)[...,3].astype(np.float32)/255
            after=decode(fixed[:148],fixed[offset:offset+size],w,h)[...,3].astype(np.float32)/255
            row={'mip':mip['mip'],'width':w,'height':h,
                 'referenceSourceMip0Bilinear4x4':reference,
                 'beforePointCoverage':float(np.mean(before>=cutoff)),
                 'afterPointCoverage':float(np.mean(after>=cutoff)),
                 'beforeBilinear4x4Coverage':bilinear_coverage(before,cutoff),
                 'afterBilinear4x4Coverage':bilinear_coverage(after,cutoff)}
            if min(w,h)<=64:
                row['beforeBilinear8x8Coverage']=bilinear_coverage(before,cutoff,8)
                row['afterBilinear8x8Coverage']=bilinear_coverage(after,cutoff,8)
            if min(w,h)<=32:
                nearest_error=abs(round(point_target*w*h)/(w*h)-point_target)
                if abs(row['afterPointCoverage']-point_target)>nearest_error+1e-12 or abs(row['afterBilinear8x8Coverage']-reference)>max(.01,.5/(w*h)):
                    raise ValueError('Point/filtered coverage failed independently: '+repair['texture']+' mip '+str(mip['mip']))
            elif original[offset:offset+size] != fixed[offset:offset+size]:
                raise ValueError('Large mip payload changed')
            rows.append(row); offset+=size
            if max(w,h)<=128:sampled_mips[mip['mip']]=(before,after)
        transitions=[]
        for mip,(before,after) in sampled_mips.items():
            if mip+1 not in sampled_mips:continue
            h,w=after.shape; before_next,after_next=sampled_mips[mip+1]
            a0=sample_periodic(after,w*8,h*8); a1=sample_periodic(after_next,w*8,h*8)
            b0=sample_periodic(before,w*8,h*8); b1=sample_periodic(before_next,w*8,h*8)
            for fraction in [.25,.5,.75]:
                original_area=float(np.mean((b0*(1-fraction)+b1*fraction)>=cutoff))
                repaired_area=float(np.mean((a0*(1-fraction)+a1*fraction)>=cutoff))
                transitions.append({'mip':mip,'nextMip':mip+1,'fraction':fraction,'width':w,'height':h,
                                    'beforeCoverage':original_area,'afterCoverage':repaired_area,
                                    'absoluteError':abs(repaired_area-reference),
                                    'screening':'PASS' if abs(repaired_area-reference)<=.02 else 'FAIL_AREA_DRIFT'})
        results.append({'texture':repair['texture'],'mips':rows,'trilinearTransitions':transitions})
    transition_failures=[{'texture':r['texture'],**t} for r in results for t in r['trilinearTransitions'] if t['width']>=32 and t['screening']!='PASS']
    write(output/'mask_filtering_validation.json',{'evidenceType':'unit-test','status':'PASS_POINT_AND_FILTERED_COVERAGE' if not transition_failures else 'FAIL_TRILINEAR_AREA',
          'command':'python Tools/Assets/RenderingBenchmarks/verify_mask_filtering.py --variant '+str(args.variant),
          'sampling':'Repeat address; 4x4 evenly spaced subpixel bilinear positions per texel, 8x8 convergence check for <=64px',
          'trilinearVisibleScaleFailures':transition_failures,
          'limitation':'Point, periodic bilinear and trilinear area estimates; does not prove anisotropic camera-motion quality.', 'textures':results})
    for record in results:
        print(record['texture'],[m for m in record['mips'] if m['width']==32])
    if transition_failures:raise ValueError('Visible-scale trilinear area drift failed')


if __name__=='__main__':main()
