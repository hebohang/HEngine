#!/usr/bin/env python3
"""Repair post-compression Mask coverage in a separate benchmark variant.

Only failing <=32 pixel mips are regenerated. Large mips remain byte-identical.
Deterministic alpha-rank tie breaking targets the nearest representable texel
coverage, followed by decoded BC7 feedback. No GPU or engine process is used.
"""
import argparse
import copy
import io
from pathlib import Path
import struct
import subprocess

import numpy as np
from PIL import Image
from compress_benchmark_textures import read, write, sha, asset, remap, PROJECT, alpha_mip_coverage


def decode(header, payload, width, height):
    h = bytearray(header)
    struct.pack_into('<II', h, 12, height, width)
    struct.pack_into('<I', h, 28, 1)
    return np.asarray(Image.open(io.BytesIO(bytes(h)+payload)).convert('RGBA'))


def bilinear_coverage(alpha, cutoff, grid=4):
    covered = 0
    for iy in range(grid):
        v=(iy+.5)/grid-.5
        rows=alpha*(1-abs(v))+np.roll(alpha,-1 if v >= 0 else 1,axis=0)*abs(v)
        for ix in range(grid):
            u=(ix+.5)/grid-.5
            sample=rows*(1-abs(u))+np.roll(rows,-1 if u >= 0 else 1,axis=1)*abs(u)
            covered += int(np.sum(sample >= cutoff))
    return covered/(alpha.size*grid*grid)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reuse-repairs', type=Path, help='Reuse byte-verified identical Mask repairs across normal-format variants')
    parser.add_argument('--texconv', type=Path, default=PROJECT/'SourceAssets/RenderingBenchmarks/Tools/DirectXTexMay2026/texconv.exe')
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if source == output or PROJECT/'Assets/RenderingBenchmarks' not in output.parents:
        raise ValueError('Expected an independent owned benchmark variant')
    report = read(source/'compression.json')
    reusable = read(args.reuse_repairs/'compression.json') if args.reuse_repairs else None
    if report['status'] != 'PASS_OFFLINE_CONVERSION':
        raise ValueError('Source conversion incomplete')
    output.mkdir(parents=True, exist_ok=True)
    repairs, mapping = [], {}
    for key, record in report['textures'].items():
        quality = record['qualityMip0']
        if 'alphaCutoff' not in quality:
            continue
        cutoff, target = quality['alphaCutoff'], quality['sourceMip0Coverage']
        failing = [r for r in record['alphaMipCoverage'] if min(r['width'],r['height']) >= 32 and abs(r['coverage']-target) > .02]
        if not failing:
            continue
        if reusable:
            fixed_record = reusable['textures'][key]
            repair = next(r for r in reusable['maskRepair']['textures'] if r['texture']==key)
            if repair['baselineSha256'] != record['outputSha256'] or sha(PROJECT/fixed_record['output']) != repair['fixedSha256']:
                raise ValueError('Mask repair reuse input/output checksums differ')
            mapping[record['output']]=fixed_record['output']
            record['baselineOutput']=record['output']; record['output']=fixed_record['output']
            record['outputSha256']=fixed_record['outputSha256']; record['alphaMipCoverage']=fixed_record['alphaMipCoverage']
            repairs.append(copy.deepcopy(repair))
            continue
        original = PROJECT/record['output']
        if sha(original) != record['outputSha256']:
            raise ValueError('Source DDS drift')
        data = original.read_bytes()
        header, offset = data[:148], 148
        # PreparedMasks RGB was already dilated while alpha remained immutable.
        original_png = Path(record['command'][-1])
        pixels = np.asarray(Image.open(original_png).convert('RGBA'),dtype=np.float32)/255
        area_target = bilinear_coverage(pixels[...,3],cutoff)
        pixels[...,:3] = np.where(pixels[...,:3] <= .04045, pixels[...,:3]/12.92, ((pixels[...,:3]+.055)/1.055)**2.4)
        payloads, rows = [], []
        for mip in range(record['mipLevels']):
            height, width = pixels.shape[:2]
            count = width*height
            size = ((width+3)//4)*((height+3)//4)*16
            original_payload = data[offset:offset+size]
            offset += size
            actual_before = float(np.mean(decode(header,original_payload,width,height)[...,3]/255 >= cutoff))
            chosen = original_payload
            mip_report = {'mip':mip,'width':width,'height':height,'beforeCoverage':actual_before,
                          'targetCoverage':target,'texelCoverageQuantum':1/count,
                          'bestRepresentableAbsoluteError':abs(round(target*count)/count-target)}
            if min(width,height) <= 32:
                required = int(round(target*count))
                alpha = pixels[...,3]
                indices = np.argsort(-alpha.reshape(-1),kind='stable')
                accepted = np.zeros(count,dtype=bool); accepted[indices[:required]]=True
                accepted = accepted.reshape(height,width)
                sorted_alpha = alpha.reshape(-1)[indices]
                split = (sorted_alpha[required-1]+sorted_alpha[required])*.5 if 0 < required < count else .5
                scale = cutoff/max(float(split),1e-8)
                base_alpha = np.clip(alpha*scale,0,1)
                rgb = np.where(pixels[...,:3] <= .0031308, pixels[...,:3]*12.92, 1.055*np.maximum(pixels[...,:3],0)**(1/2.4)-.055)
                attempts, best = [], None
                area_tolerance = max(.01, .5/count)
                candidates = [(offset,margin) for offset in [0,8,16,32,64,-8,-16,-32,-64]
                              for margin in [0,2,4,8,16,32,64,127]]
                # Try smaller alpha adjustments first. Meeting point coverage
                # by prematurely binarizing a coarse mip can harm trilinear
                # transitions even when the per-level area passes.
                candidates.sort(key=lambda item: abs(item[0])+item[1])
                for area_offset, margin in candidates:
                    rgba = np.empty((height,width,4),dtype=np.uint8)
                    rgba[...,:3] = np.rint(np.clip(rgb,0,1)*255).astype(np.uint8)
                    alpha_bytes = np.clip(np.rint(base_alpha*255)+area_offset,0,255)
                    threshold = int(np.ceil(cutoff*255))
                    alpha_bytes[accepted] = np.maximum(alpha_bytes[accepted],min(threshold+margin,255))
                    alpha_bytes[~accepted] = np.minimum(alpha_bytes[~accepted],max(threshold-1-margin,0))
                    rgba[...,3] = alpha_bytes.astype(np.uint8)
                    folder = output/'MaskRepairWork'/key/('mip%02d-offset%+04d-margin%03d' % (mip,area_offset,margin))
                    folder.mkdir(parents=True,exist_ok=True)
                    png, dds = folder/'level.png', folder/'level.dds'
                    Image.fromarray(rgba,'RGBA').save(png)
                    command = [str(args.texconv.resolve()),'-nologo','-y','-nogpu','-singleproc',
                               '-f','BC7_UNORM_SRGB','-bc','x','-m','1','-dx10','-nowic','-sepalpha','-srgb','-o',str(folder),str(png)]
                    run = subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=60)
                    (folder/'encoder.log').write_text(run.stdout,encoding='utf-8')
                    if run.returncode:
                        raise RuntimeError('Small CPU mip encoding failed: '+str(folder))
                    candidate = dds.read_bytes()[148:]
                    decoded = decode(header,candidate,width,height)
                    covered = int(np.sum(decoded[...,3]/255 >= cutoff))
                    area_coverage = bilinear_coverage(decoded[...,3].astype(np.float32)/255,cutoff,8)
                    attempt = {'marginByte':margin,'scale':scale,'coveredTexels':covered,'requiredTexels':required,
                               'areaOffsetByte':area_offset,'bilinear8x8Coverage':area_coverage,
                               'command':command,'inputSha256':sha(png),'outputSha256':sha(dds)}
                    attempts.append(attempt)
                    error = (abs(covered-required),abs(area_coverage-area_target))
                    if best is None or error < best[0]:
                        best = (error,candidate,covered,margin,area_offset,area_coverage)
                    if error[0] == 0 and error[1] <= area_tolerance:
                        break
                chosen = best[1]
                mip_report.update({'attempts':attempts,'chosenMarginByte':best[3],
                                   'chosenAreaOffsetByte':best[4],'afterBilinear8x8Coverage':best[5],
                                   'sourceMip0Bilinear4x4Coverage':area_target,'bilinearCoverageTolerance':area_tolerance,
                                   'postCompressionAlphaRankChanges':int(np.sum((decode(header,chosen,width,height)[...,3]/255>=cutoff) != accepted)),
                                   'status':'PASS_POINT_AND_FILTERED_COVERAGE' if best[0][0]==0 and best[0][1]<=area_tolerance else 'FAIL_EXTRA_COVERAGE_ERROR'})
                if best[0][0] != 0 or best[0][1] > area_tolerance:
                    raise ValueError('BC7 could not preserve nearest representable coverage: '+key+' mip '+str(mip))
            else:
                mip_report['status']='PASS_UNCHANGED_LARGE_MIP_BYTES'
            mip_report['afterCoverage']=float(np.mean(decode(header,chosen,width,height)[...,3]/255 >= cutoff))
            mip_report['payloadUnchanged']=chosen==original_payload
            rows.append(mip_report); payloads.append(chosen)
            if width > 1: pixels=(pixels[:,0::2]+pixels[:,1::2])*.5
            if height > 1: pixels=(pixels[0::2]+pixels[1::2])*.5
        destination = output/'Textures'/original.name
        destination.parent.mkdir(exist_ok=True)
        destination.write_bytes(header+b''.join(payloads))
        if destination.stat().st_size != len(data): raise ValueError('Mip layout changed')
        mapping[record['output']]=asset(destination)
        repairs.append({'texture':key,'baselineSha256':record['outputSha256'],'fixedSha256':sha(destination),
                        'cutoff':cutoff,'mips':rows})
        record['baselineOutput']=record['output']; record['output']=asset(destination)
        record['outputSha256']=sha(destination); record['alphaMipCoverage']=alpha_mip_coverage(destination,cutoff)
        print('Repaired '+key,flush=True)
    material_map, mesh_map = {}, {}
    for path in (source/'Materials').glob('*.hmat'):
        dest=output/'Materials'/path.name; write(dest,remap(read(path),mapping)); material_map[asset(path)]=asset(dest)
    for path in (source/'Meshes').glob('*.hmesh'):
        dest=output/'Meshes'/path.name; write(dest,remap(read(path),material_map)); mesh_map[asset(path)]=asset(dest)
    for path in source.glob('*.he'): write(output/path.name,remap(read(path),mesh_map))
    for name in ['LICENSE.txt','README.txt','CHANGELOG.txt']:
        if (source/name).is_file(): (output/name).write_bytes((source/name).read_bytes())
    report['maskRepair']={'source':asset(source),'policy':'Preserve mip0..5 bytes; mip6+ deterministic alpha ranking/scale, then decoded CPU BC7 maximum-mode feedback',
                          'textures':repairs,'status':'PASS_POINT_AND_FILTERED_COVERAGE'}
    write(output/'compression.json',report)
    config=read(source/'benchmark_config.json'); config['maskRepair']=report['maskRepair']['policy']
    write(output/'benchmark_config.json',config)
    print('Mask repair PASS: '+str(len(repairs))+' independent DDS textures')


if __name__ == '__main__': main()
