"""Verify LOD derivatives against immutable source topology/vertex buffers."""
import argparse
import hashlib
import json
import math
import struct
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('project', type=Path)
parser.add_argument('manifest', type=Path)
args = parser.parse_args()
read = lambda p: json.loads(p.read_text(encoding='utf-8-sig'))
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
manifest = read(args.project / args.manifest)
levels = 0
for entry in manifest['meshes']:
    original_path = args.project / entry['source']
    assert digest(original_path) == entry['sourceHash']
    original = read(original_path)
    derived = read(args.project / manifest['mapping'][entry['source']])
    for key in ['accessorVec', 'bufferVec', 'bufferViewVec']:
        assert derived[key][:len(original[key])] == original[key], (entry['source'], key)
    for buffer in entry['buffers']:
        assert digest(args.project / buffer['uri']) == buffer['hash']
    assert len(original['primitives']) == len(derived['primitives'])
    for old, new in zip(original['primitives'], derived['primitives']):
        assert {k: v for k, v in old.items() if k != 'lods'} == {k: v for k, v in new.items() if k != 'lods'}
        vertex_count = original['accessorVec'][old['attributes']['POSITION']]['count']
        count = original['accessorVec'][old['indices']]['count']
        error = 0
        for lod in new.get('lods', []):
            accessor = derived['accessorVec'][lod['indices']]
            view = derived['bufferViewVec'][accessor['bufferViewIndex']]
            assert accessor['componentType'] == 5125 and accessor['count'] % 3 == 0
            assert 0 < accessor['count'] < count
            assert math.isfinite(lod['objectSpaceError']) and lod['objectSpaceError'] >= error
            buffer = (args.project / derived['bufferVec'][view['bufferIndex']]['uri']).read_bytes()
            start = view['byteOffset'] + accessor['byteOffset']
            indices = struct.unpack_from('<' + 'I' * accessor['count'], buffer, start)
            assert max(indices) < vertex_count
            count, error = accessor['count'], lod['objectSpaceError']
            levels += 1
print(json.dumps({'evidenceType': 'static-check', 'result': 'PASS', 'meshes': len(manifest['meshes']),
                  'lodLevels': levels, 'preserved': 'LOD0, vertex streams including UV1, material slots, source hashes'}))
