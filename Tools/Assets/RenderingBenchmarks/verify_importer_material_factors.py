#!/usr/bin/env python3
"""Exercise the compiled importer with non-default and zero glTF texture factors."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from import_gltf_scene import PROJECT, ROOT, digest, write_json, read_json


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--importer', type=Path, required=True)
    args = parser.parse_args()
    importer = args.importer.resolve()
    if not importer.is_file():
        raise ValueError('Importer executable does not exist')
    source = PROJECT/'SourceAssets/RenderingBenchmarks/ImporterRegression'
    source.mkdir(parents=True, exist_ok=True)
    from PIL import Image
    Image.new('RGBA', (2,2), (128,128,255,255)).save(str(source/'normal.png'))
    Image.new('L', (2,2), 128).save(str(source/'ao.png'))
    binary = struct.pack('<9f3H', 0,0,0, 1,0,0, 0,1,0, 0,1,2)
    (source/'texture_factors.bin').write_bytes(binary)
    cases = [('NonDefault', 0.3, 0.4), ('Zero', 0.0, 0.0), ('Default', 1.0, 1.0)]
    document = {'asset':{'version':'2.0'},'scene':0,'scenes':[{'nodes':[0]}], 'nodes':[{'mesh':0}],
        'meshes':[{'primitives':[{'attributes':{'POSITION':0},'indices':1,'material':i} for i in range(len(cases))]}],
        'buffers':[{'uri':'texture_factors.bin','byteLength':len(binary)}],
        'bufferViews':[{'buffer':0,'byteOffset':0,'byteLength':36},{'buffer':0,'byteOffset':36,'byteLength':6}],
        'accessors':[{'bufferView':0,'componentType':5126,'count':3,'type':'VEC3','min':[0,0,0],'max':[1,1,0]},
                     {'bufferView':1,'componentType':5123,'count':3,'type':'SCALAR'}],
        'images':[{'uri':'normal.png'},{'uri':'ao.png'}], 'textures':[{'source':0},{'source':1}],
        'materials':[{'name':name,'normalTexture':{'index':0,'scale':normal},
                     'occlusionTexture':{'index':1,'strength':ao}} for name,normal,ao in cases]}
    fixture = source/'texture_factors.gltf'
    write_json(fixture, document)
    assets = PROJECT/'Assets/RenderingBenchmarks'
    assets.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='.material-factors-', dir=str(assets))).resolve()
    output.relative_to(assets.resolve())
    command = [str(importer),str(PROJECT.resolve()),str(fixture.resolve()),str(output)]
    evidence = {'command':command,'importerSha256':digest(importer),'evidenceType':'unit-test','cases':[]}
    try:
        with (source/'import.log').open('w', encoding='utf-8') as stream:
            result = subprocess.run(command,cwd=str(ROOT),stdout=stream,stderr=subprocess.STDOUT,timeout=60)
        evidence['exitCode'] = result.returncode
        if result.returncode:
            raise ValueError('Compiled importer failed; see '+str(source/'import.log'))
        for name,normal,ao in cases:
            material = read_json(output/(name+'.hmat'))
            actual_normal = material['Parameters']['g_NormalStrength']['Value']
            actual_ao = material['Parameters']['g_AoStrength']['Value']
            passed = abs(actual_normal-normal)<1e-6 and abs(actual_ao-ao)<1e-6
            evidence['cases'].append({'name':name,'expectedNormalScale':normal,'normalScale':actual_normal,
                                     'expectedOcclusionStrength':ao,'occlusionStrength':actual_ao,'pass':passed})
        evidence['status'] = 'PASS' if all(case['pass'] for case in evidence['cases']) else 'FAIL'
        write_json(source/'validation.json',evidence)
        write_json(source/('validation-'+evidence['importerSha256'][:12]+'.json'),evidence)
        print(json.dumps({'status':evidence['status'],'cases':evidence['cases']},indent=2))
        if evidence['status'] != 'PASS':
            raise ValueError('glTF normal scale / AO strength was not preserved')
    finally:
        # This unique output was created immediately above and was not user data.
        # Delete only individual regular files; never recursively remove a tree.
        for path in output.iterdir():
            if not path.is_file() or path.is_symlink():
                raise ValueError('Unexpected entry in owned regression output: '+str(path))
        for path in output.iterdir():
            path.unlink()
        output.rmdir()


if __name__ == '__main__':
    main()
