#!/usr/bin/env python3
"""Create a separate BC7/BC5 benchmark variant with immutable shared geometry.

Requires official Microsoft texconv and numpy/Pillow; never executes the engine.
Source material values, transforms and full geometry remain identical. PNG input
comes from an explicitly named prepared quality configuration, never resized here.
"""
import argparse
import copy
import hashlib
import io
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import time

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[3]
PROJECT = ROOT / 'Examples/RenderingTests'


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True)+'\n', encoding='utf-8')


def sha(path):
    with path.open('rb') as stream:
        h = hashlib.sha256()
        for block in iter(lambda: stream.read(1024*1024), b''):
            h.update(block)
        return h.hexdigest()


def asset(path):
    return path.resolve().relative_to(PROJECT).as_posix()


def remap(value, mapping):
    if isinstance(value, str):
        return mapping.get(value, value)
    if isinstance(value, list):
        return [remap(v, mapping) for v in value]
    if isinstance(value, dict):
        return {k: remap(v, mapping) for k, v in value.items()}
    return value


def dds_info(path):
    with path.open('rb') as stream:
        header = stream.read(148)
    if header[:4] != b'DDS ' or header[84:88] != b'DX10':
        raise ValueError('Expected explicit DX10-format DDS header: '+str(path))
    height, width = struct.unpack_from('<II', header, 12)
    mips = struct.unpack_from('<I', header, 28)[0]
    fmt = struct.unpack_from('<I', header, 128)[0]
    payload = 0
    w, h = width, height
    for unused in range(mips):
        payload += ((w+3)//4)*((h+3)//4)*16
        w, h = max(1,w//2), max(1,h//2)
    if mips != 1+int(math.log2(max(width,height))) or path.stat().st_size != 148+payload:
        raise ValueError('Incomplete compressed mip chain or payload size mismatch')
    return {'width':width,'height':height,'mipLevels':mips,'encodedFormat':fmt,'payloadBytes':payload}


def quality(source, compressed, role, cutoff):
    a = np.asarray(Image.open(source).convert('RGBA'), dtype=np.float32)/255
    b = np.asarray(Image.open(compressed).convert('RGBA'), dtype=np.float32)/255
    if a.shape != b.shape:
        raise ValueError('Compressed image changed dimensions')
    if role == 'normal':
        def normal(p):
            xy = p[...,:2]*2-1
            z = np.sqrt(np.maximum(1-np.sum(xy*xy,axis=-1), 1e-4))
            n = np.dstack((xy,z))
            return n/np.linalg.norm(n,axis=-1)[...,None]
        angles = np.degrees(np.arccos(np.clip(np.sum(normal(a)*normal(b),axis=-1),-1,1)))
        result = {'normalDegreesMean':float(np.mean(angles)), 'normalDegreesP99':float(np.percentile(angles,99))}
    else:
        mse = float(np.mean((a[...,:3]-b[...,:3])**2))
        result = {'rgbPsnrDb':10*math.log10(1/mse) if mse else None, 'rgbMse':mse}
    if cutoff is not None:
        result.update({'alphaCutoff':cutoff,'sourceMip0Coverage':float(np.mean(a[...,3]>=cutoff)),
                       'compressedMip0Coverage':float(np.mean(b[...,3]>=cutoff))})
    return result


def alpha_mip_coverage(path, cutoff):
    data = path.read_bytes()
    header = bytearray(data[:148])
    height,width = struct.unpack_from('<II',header,12)
    mips = struct.unpack_from('<I',header,28)[0]
    offset = 148
    result = []
    for mip in range(mips):
        size = ((width+3)//4)*((height+3)//4)*16
        struct.pack_into('<II',header,12,height,width)
        struct.pack_into('<I',header,28,1)
        pixels = np.asarray(Image.open(io.BytesIO(bytes(header)+data[offset:offset+size])).convert('RGBA'))
        result.append({'mip':mip,'width':width,'height':height,'coverage':float(np.mean(pixels[...,3]/255>=cutoff))})
        offset += size
        width,height=max(1,width//2),max(1,height//2)
    return result


def dilate_mask_rgb(source, destination, cutoff):
    """Fill rejected RGB from a progressively coarser valid-color pyramid.

    Alpha and every accepted RGB texel remain byte-identical. Normalized color
    filtering avoids introducing black into straight-alpha compressed mips.
    """
    pixels = np.asarray(Image.open(source).convert('RGBA')).copy()
    valid = pixels[...,3].astype(np.float32)/255 >= cutoff
    if not np.any(valid) or np.all(valid):
        return source
    target = pixels[...,:3].astype(np.float32)
    weights = valid.astype(np.float32)
    colors = target*weights[...,None]
    pending = ~valid
    while np.any(pending):
        h,w = weights.shape
        if h == 1 and w == 1:
            break
        pad_h,pad_w = h % 2,w % 2
        weights = np.pad(weights,((0,pad_h),(0,pad_w))).reshape((h+1)//2,2,(w+1)//2,2).sum(axis=(1,3))
        colors = np.pad(colors,((0,pad_h),(0,pad_w),(0,0))).reshape((h+1)//2,2,(w+1)//2,2,3).sum(axis=(1,3))
        level = np.clip(colors/np.maximum(weights[...,None],1e-12),0,255).astype(np.uint8)
        expanded = np.asarray(Image.fromarray(level,'RGB').resize((pixels.shape[1],pixels.shape[0]),Image.Resampling.NEAREST))
        covered = np.asarray(Image.fromarray((weights>0).astype(np.uint8)*255).resize((pixels.shape[1],pixels.shape[0]),Image.Resampling.NEAREST))>0
        fill = pending & covered
        target[fill] = expanded[fill]
        pending[fill] = False
    pixels[...,:3] = target.astype(np.uint8)
    destination.parent.mkdir(parents=True,exist_ok=True)
    Image.fromarray(pixels,'RGBA').save(destination)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input-assets', type=Path, required=True)
    parser.add_argument('--prepared', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--texconv', type=Path, default=PROJECT/'SourceAssets/RenderingBenchmarks/Tools/DirectXTexMay2026/texconv.exe')
    parser.add_argument('--gpu',type=int,help='Explicit GPU conversion window, isolated from renderer profiling')
    parser.add_argument('--bc7-mode6',action='store_true',help='Explicit fast encoder quality variant')
    parser.add_argument('--normal-format',choices=['BC5','BC7'],default='BC5',help='BC7 preserves averaged XYZ length for normal mip variance at the same 8 bpp')
    parser.add_argument('--reuse-compressed',type=Path,help='Reuse byte-verified compatible non-normal DDS via hard links; only changed normal formats are encoded')
    args = parser.parse_args()
    source, prepared, output = args.input_assets.resolve(), args.prepared.resolve(), args.output.resolve()
    if source == output or PROJECT/'Assets/RenderingBenchmarks' not in output.parents:
        raise ValueError('Output must be a separate owned benchmark directory')
    progress = read(source/'import_progress.json')
    conversion = read(prepared/'conversion.json')
    materials = sorted((source/progress.get('materialDirectory','Materials')).glob('*.hmat'))
    texconv_hash = sha(args.texconv)
    marker = output/'compression.json'
    config = {'sourceAssets':str(source),'prepared':str(prepared),'sourceHash':conversion['sourceSha256'],
              'textureMaximumDimension':conversion['textureMaximumDimension'],'texconvSha256':texconv_hash,
              'texturePolicy':'BC7_UNORM_SRGB color/emissive; BC7_UNORM data; '+
                  ('BC5_UNORM normal XY' if args.normal_format=='BC5' else 'BC7_UNORM normal XYZ with unnormalized linear mip averages')+'; complete offline mips',
              'maskPolicy':'rejected RGB filled from normalized valid-color pyramid; separate alpha; --keep-coverage authored cutoff; BC7 continuous alpha',
              'compression':('GPU adapter '+str(args.gpu) if args.gpu is not None else 'CPU, single process/thread')+
                  ('; BC7 mode6 encoder' if args.bc7_mode6 else '; default BC7 encoder')+'; no resolution change'}
    old = read(marker) if marker.exists() else {'config':config,'textures':{}}
    reusable = read(args.reuse_compressed/'compression.json')['textures'] if args.reuse_compressed else {}
    if old['config'] != config:
        raise ValueError('Existing compression variant has a different configuration')
    output.mkdir(parents=True,exist_ok=True)
    (output/'Textures').mkdir(exist_ok=True)
    records = old['textures']
    material_map = {}
    for material_index, material_path in enumerate(materials):
        material = read(material_path)
        mapping = {}
        for name, texture in material['Textures'].items():
            old_path = texture['Path']
            if not old_path.startswith('Assets/'):
                continue
            filename = Path(old_path).name
            png = prepared/'textures'/filename
            if not png.is_file():
                raise ValueError('Prepared texture missing: '+str(png))
            role = 'normal' if name == 'g_NormalMap' else 'color' if texture['ColorSpace'] == 'SRGB' else 'data'
            cutoff = float(material['Parameters']['g_AlphaCutoff']['Value']) if name == 'g_AlbedoMap' and material['BlendMode']=='Mask' else None
            key = png.stem+'_'+role+('_mask'+str(cutoff).replace('.','p') if cutoff is not None else '')
            dds = output/'Textures'/(key+'.dds')
            source_hash = sha(png)
            fmt = ('BC5_UNORM' if args.normal_format=='BC5' else 'BC7_UNORM') if role=='normal' else 'BC7_UNORM_SRGB' if role=='color' else 'BC7_UNORM'
            expected_format = {'BC5_UNORM':83,'BC7_UNORM':98,'BC7_UNORM_SRGB':99}[fmt]
            reused = reusable.get(key)
            if key not in records and reused and reused['sourceSha256']==source_hash and reused['encodedFormat']==expected_format:
                reused_path = PROJECT/reused['output']
                if sha(reused_path) != reused['outputSha256']:
                    raise ValueError('Reusable compressed texture checksum mismatch')
                if not dds.exists():
                    os.link(str(reused_path),str(dds))
                if sha(dds) != reused['outputSha256']:
                    raise ValueError('Reused DDS output checksum mismatch')
                records[key] = dict(reused,output=asset(dds),reusedFrom=reused['output'])
            if key not in records or records[key]['sourceSha256'] != source_hash or not dds.exists() or records[key]['outputSha256'] != sha(dds):
                # A later source edit must never let texconv truncate a reused
                # baseline through this output's hard link. Unlink only this
                # verified, nonrecursive variant path before encoding anew.
                if dds.exists() and dds.stat().st_nlink > 1:
                    if dds.resolve().parent != output/'Textures':
                        raise ValueError('Compressed output escaped owned texture directory')
                    dds.unlink()
                # texconv derives output filename from input; a prefix avoids role/cutoff collisions.
                suffix = key[len(png.stem):]
                compression_input = dilate_mask_rgb(png,output/'PreparedMasks'/key/png.name,cutoff) if cutoff is not None else png
                command = [str(args.texconv.resolve()),'-nologo','-y','-singleproc','-f',fmt,'-m','0',
                           '-dx10','-nowic','-sepalpha','-if','BOX','-sx',suffix,'-o',str(output/'Textures')]
                command += ['-gpu',str(args.gpu)] if args.gpu is not None else ['-nogpu']
                if args.bc7_mode6 and fmt.startswith('BC7'):
                    command += ['-bc','q']
                command += ['-srgb'] if role=='color' else ['--ignore-srgb']
                if cutoff is not None:
                    command += ['--keep-coverage',str(cutoff)]
                command += [str(compression_input)]
                start = time.monotonic()
                result = subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=600)
                log = output/'Logs'/(key+'.log')
                log.parent.mkdir(exist_ok=True)
                log.write_text(result.stdout,encoding='utf-8')
                if result.returncode or not dds.is_file():
                    raise RuntimeError('texconv failed; see '+str(log))
                info = dds_info(dds)
                if info['encodedFormat'] != expected_format:
                    raise ValueError('Unexpected encoded texture format')
                records[key] = {'source':str(png),'sourceSha256':source_hash,'output':asset(dds),'outputSha256':sha(dds),
                                'command':command,'elapsedSeconds':time.monotonic()-start,'role':role,
                                'qualityMip0':quality(png,dds,role,cutoff),**info}
                if cutoff is not None:
                    records[key]['alphaMipCoverage'] = alpha_mip_coverage(dds,cutoff)
                write(marker,{'config':config,'textures':records,'status':'CONVERTING'})
                print('compressed %d textures; material %d/%d' % (len(records),material_index+1,len(materials)),flush=True)
            if old_path in mapping and mapping[old_path] != asset(dds):
                raise ValueError('One material uses the same texture with conflicting semantics')
            mapping[old_path] = asset(dds)
        destination = output/'Materials'/material_path.name
        write(destination,remap(material,mapping))
        material_map[asset(material_path)] = asset(destination)
    mesh_map = {}
    for mesh_path in sorted((source/'Meshes').glob('*.hmesh')):
        destination = output/'Meshes'/mesh_path.name
        write(destination,remap(read(mesh_path),material_map))
        mesh_map[asset(mesh_path)] = asset(destination)
    for scene_path in source.glob('*.he'):
        write(output/scene_path.name,remap(read(scene_path),mesh_map))
    benchmark = read(source/'benchmark_config.json') if (source/'benchmark_config.json').exists() else {}
    benchmark['materialTextureMaximumDimension'] = conversion['textureMaximumDimension']
    benchmark['textureCompression'] = config
    benchmark['texturePayloadBytes'] = sum(t['payloadBytes'] for t in records.values())
    benchmark['geometryPolicy'] = 'Identical shared geometry buffers referenced from '+asset(source)
    write(output/'benchmark_config.json',benchmark)
    write(marker,{'config':config,'textures':records,'status':'PASS_OFFLINE_CONVERSION',
                  'texturePayloadBytes':benchmark['texturePayloadBytes'],'materials':len(materials),'meshes':len(mesh_map),
                  'runtimeValidation':'Not yet performed; parent agent runs HGame and GPU validation'})
    print('compression PASS: %d textures, %d payload bytes' % (len(records),benchmark['texturePayloadBytes']))


if __name__ == '__main__':
    main()
