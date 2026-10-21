#!/usr/bin/env python3
"""Validate LOD index assets independently against immutable source LOD0 data."""
import argparse
import copy
import hashlib
import math
import struct
from pathlib import Path
from compress_benchmark_textures import read,write,sha,asset,remap,PROJECT


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--variant',type=Path,default=PROJECT/'Assets/RenderingBenchmarks/IntelSponza2048BCGPULOD')
    args=parser.parse_args()
    output=args.variant.resolve()
    generation=read(output/'lod_generation.json')
    source=PROJECT/generation['source']
    mesh_reverse={}
    counts=[0,0,0,0]
    primitive_count=0
    actual_levels=0
    for record in generation['meshes']:
        original_path=source/'Meshes'/record['name']
        original=read(original_path)
        path=output/'Meshes'/record['name']
        modified=read(path)
        if sha(original_path)!=record['sourceMetadataSha256']:
            raise ValueError('Baseline mesh metadata changed after generation')
        for buffer in record['sourceBuffers']:
            if sha(PROJECT/buffer['uri'])!=buffer['sha256']:
                raise ValueError('Baseline LOD0 vertex/index bytes changed')
        original_only=copy.deepcopy(modified)
        original_only['accessorVec']=original_only['accessorVec'][:len(original['accessorVec'])]
        original_only['bufferViewVec']=original_only['bufferViewVec'][:len(original['bufferViewVec'])]
        original_only['bufferVec']=original_only['bufferVec'][:len(original['bufferVec'])]
        for primitive in original_only['primitives']:
            primitive.pop('lods',None)
        if original_only!=original:
            raise ValueError('LOD0 geometry, material, accessor or mesh metadata modified')
        primitive_records=record.get('primitives')
        if not isinstance(primitive_records,list) or len(primitive_records)!=len(modified['primitives']):
            raise ValueError('Generation primitive inventory missing or inconsistent')
        for primitive_index,primitive in enumerate(modified['primitives']):
            primitive_record=primitive_records[primitive_index]
            if primitive_record.get('primitive')!=primitive_index:
                raise ValueError('Generation primitive identity mismatch')
            primitive_count+=1
            base_count=modified['accessorVec'][primitive['indices']]['count']
            vertex_count=modified['accessorVec'][primitive['attributes']['POSITION']]['count']
            previous_count,previous_error=base_count,0.0
            levels=primitive.get('lods',[])
            if len(levels)>8:
                raise ValueError('LOD count exceeds the resource contract')
            expected_levels=primitive_record.get('levels')
            if not isinstance(expected_levels,list) or len(expected_levels)!=len(levels):
                raise ValueError('Generation LOD level inventory missing or inconsistent')
            local_counts=[base_count]
            for level_index,lod in enumerate(levels):
                expected=expected_levels[level_index]
                expected_hash=expected.get('sha256')
                if not isinstance(expected_hash,str) or len(expected_hash)!=64 or any(c not in '0123456789abcdefABCDEF' for c in expected_hash):
                    raise ValueError('Generation LOD SHA256 missing or invalid')
                if type(lod['indices']) is not int or not len(original['accessorVec'])<=lod['indices']<len(modified['accessorVec']):
                    raise ValueError('LOD accessor index is not appended or out of bounds')
                accessor=modified['accessorVec'][lod['indices']]
                view_index=accessor['bufferViewIndex']
                if type(view_index) is not int or not len(original['bufferViewVec'])<=view_index<len(modified['bufferViewVec']):
                    raise ValueError('LOD bufferView index is not appended or out of bounds')
                view=modified['bufferViewVec'][view_index]
                buffer_index=view['bufferIndex']
                if type(buffer_index) is not int or not len(original['bufferVec'])<=buffer_index<len(modified['bufferVec']):
                    raise ValueError('LOD buffer index is not appended or out of bounds')
                buffer=modified['bufferVec'][buffer_index]
                count=accessor['count']
                if accessor['type']!='SCALAR' or accessor['componentType']!=5125 or type(count) is not int or count<=0 or count%3 or count>=previous_count:
                    raise ValueError('LOD index type/count invalid or not strictly decreasing')
                error=lod['objectSpaceError']
                if not math.isfinite(error) or error<previous_error:
                    raise ValueError('LOD error is not finite, nonnegative and monotonic')
                if accessor['bufferViewIndex']<len(original['bufferViewVec']) or view['bufferIndex']<len(original['bufferVec']):
                    raise ValueError('LOD indices did not use appended storage')
                if expected.get('indices')!=count or expected.get('objectSpaceError')!=error:
                    raise ValueError('LOD count/error differs from generation record')
                buffer_path=(PROJECT/buffer['uri']).resolve()
                if not buffer_path.is_relative_to(output):
                    raise ValueError('Appended LOD buffer escaped the generated variant')
                data=buffer_path.read_bytes()
                values=(view['byteOffset'],view['byteLength'],accessor['byteOffset'],buffer['byteLength'])
                if any(type(value) is not int or value<0 for value in values):
                    raise ValueError('LOD offset/length must be nonnegative integers')
                if accessor.get('normalized',False) or view.get('byteStride',0)!=0:
                    raise ValueError('LOD indices must use tightly packed unnormalized storage')
                view_offset,view_length,accessor_offset,buffer_length=values
                offset=view_offset+accessor_offset
                if len(data)!=buffer_length or view_offset+view_length>len(data) or accessor_offset+count*4>view_length:
                    raise ValueError('LOD index byte range exceeds bufferView or buffer')
                level_bytes=data[offset:offset+count*4]
                if hashlib.sha256(level_bytes).hexdigest()!=expected_hash.lower():
                    raise ValueError('Appended LOD bytes do not match generation SHA256')
                if any(index[0]>=vertex_count for index in struct.iter_unpack('<I',level_bytes)):
                    raise ValueError('LOD index does not address the original shared vertex range')
                local_counts.append(count)
                previous_count,previous_error=count,error
                actual_levels+=1
            for level in range(4):
                counts[level]+=local_counts[min(level,len(local_counts)-1)]
        mesh_reverse[asset(path)]=asset(original_path)
    scenes=[]
    scene_records=generation.get('scenes')
    if scene_records is None:
        scene_records=[{'source':asset(source/path.name),'output':path.name} for path in output.glob('*.he')]
    expected_names={record['output'] for record in scene_records}
    if len(expected_names)!=len(scene_records) or expected_names!={path.name for path in output.glob('*.he')}:
        raise ValueError('LOD output scene inventory changed')
    for record in scene_records:
        if Path(record['output']).name!=record['output']:
            raise ValueError('LOD scene output escaped the variant')
        original_scene=(PROJECT/record['source']).resolve()
        if not original_scene.is_relative_to(PROJECT.resolve()):
            raise ValueError('LOD scene source escaped the project')
        if 'sourceSha256' in record and sha(original_scene)!=record['sourceSha256']:
            raise ValueError('Original scene changed after LOD generation')
        path=output/record['output']
        scene=read(path)
        if remap(scene,mesh_reverse)!=read(original_scene):
            raise ValueError('LOD scene hierarchy, transforms, instances, light or camera changed')
        scenes.append({'scene':path.name,'source':record['source'],'nodes':len(scene),'meshInstances':sum('MeshComponent' in n for n in scene.values())})
    report={'status':'PASS','evidenceType':'unit-test','commands':['node Tools/Assets/RenderingBenchmarks/generate_mesh_lods.mjs','python Tools/Assets/RenderingBenchmarks/verify_mesh_lods.py --variant '+str(args.variant)],
            'validation':'Immutable LOD0 buffer hashes and exact scene/mesh prefix comparison; appended bounded index ranges, per-level generation SHA256 identity and monotonic finite errors',
            'meshes':len(generation['meshes']),'primitives':primitive_count,'authoredLodLevels':actual_levels,
            'uniqueTrianglesAtAvailableLevels':[n//3 for n in counts],
            'levelCountPolicy':'For a primitive with fewer levels, the last available level is used in these aggregate counts',
            'additionalIndexBytes':generation['additionalIndexBytes'],'scenes':scenes,
            'runtimeStatus':'Not verified by this tool; HGame selection, image error and performance evidence required'}
    write(output/'lod_validation.json',report)
    print(report)


if __name__=='__main__':
    main()
