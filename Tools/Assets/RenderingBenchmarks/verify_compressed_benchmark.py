#!/usr/bin/env python3
"""Verify an offline compressed variant against its imported geometry/material baseline."""
import argparse
from pathlib import Path
from compress_benchmark_textures import read,write,sha,asset,remap,dds_info,PROJECT


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--variant',type=Path,required=True)
    args=parser.parse_args()
    output=args.variant.resolve()
    report=read(output/'compression.json')
    if report['status']!='PASS_OFFLINE_CONVERSION':
        raise ValueError('Conversion incomplete')
    source=Path(report['config']['sourceAssets'])
    progress=read(source/'import_progress.json')
    material_dir=source/progress.get('materialDirectory','Materials')
    textures={r['output']:r for r in report['textures'].values()}
    payload=0
    for path,record in textures.items():
        dds=PROJECT/path
        if sha(dds)!=record['outputSha256'] or sha(Path(record['source']))!=record['sourceSha256']:
            raise ValueError('Texture data drift: '+path)
        info=dds_info(dds)
        if any(info[k]!=record[k] for k in info):
            raise ValueError('DDS metadata drift: '+path)
        payload+=info['payloadBytes']
    for material_path in (output/'Materials').glob('*.hmat'):
        material=read(material_path)
        baseline=read(material_dir/material_path.name)
        reverse={}
        if set(material['Textures'])!=set(baseline['Textures']):
            raise ValueError('Material texture slots changed')
        for slot,texture in material['Textures'].items():
            if texture['Path'].startswith('Assets/'):
                record=textures[texture['Path']]
                if Path(record['source']).name!=Path(baseline['Textures'][slot]['Path']).name:
                    raise ValueError('Material texture image changed')
                expected='normal' if slot=='g_NormalMap' else 'color' if texture['ColorSpace']=='SRGB' else 'data'
                if record['role']!=expected:
                    raise ValueError('Material compression/color-space semantic mismatch')
                reverse[texture['Path']]=baseline['Textures'][slot]['Path']
        if remap(material,reverse)!=baseline:
            raise ValueError('Non-texture material state, factors or metadata changed')
    material_reverse={asset(p):asset(material_dir/p.name) for p in (output/'Materials').glob('*.hmat')}
    mesh_reverse={}
    geometry_buffers=set()
    triangles=0
    for mesh_path in (output/'Meshes').glob('*.hmesh'):
        mesh=read(mesh_path)
        baseline_path=source/'Meshes'/mesh_path.name
        if remap(mesh,material_reverse)!=read(baseline_path):
            raise ValueError('Local geometry or mesh metadata changed')
        for buffer in mesh['bufferVec']:
            p=PROJECT/buffer['uri']
            if not p.is_file() or p.stat().st_size!=buffer['byteLength']:
                raise ValueError('Shared geometry buffer missing/truncated')
            geometry_buffers.add(buffer['uri'])
        triangles+=sum(mesh['accessorVec'][p['indices']]['count']//3 for p in mesh['primitives'])
        mesh_reverse[asset(mesh_path)]=asset(baseline_path)
    scenes=[]
    for path in output.glob('*.he'):
        scene=read(path)
        if remap(scene,mesh_reverse)!=read(source/path.name):
            raise ValueError('Scene transforms, camera, lighting, hierarchy or instances changed')
        scenes.append({'name':path.name,'nodes':len(scene),'meshInstances':sum('MeshComponent' in n for n in scene.values())})
    validation={'status':'PASS','evidenceType':'unit-test','kind':'offline source-to-variant resource contract; not runtime rendering',
                'command':'python Tools/Assets/RenderingBenchmarks/verify_compressed_benchmark.py --variant '+str(args.variant),
                'textures':len(textures),'texturePayloadBytes':payload,'sharedGeometryBuffers':len(geometry_buffers),
                'uniqueTriangles':triangles,'scenes':scenes,'qualityEvidence':'compression.json per-texture decoded mip0 metrics and Mask per-mip coverage'}
    write(output/'compression_validation.json',validation)
    print(validation)


if __name__=='__main__':
    main()
