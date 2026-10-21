#!/usr/bin/env python3
"""Normalize ORCA's documented PBR channels and a declared time-zero static pose.

Requires NumPy and Pillow (available in the bundled Codex Python runtime).
The full original FBX, glTF export, animation and material metadata remain intact.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import shutil

import numpy as np
from PIL import Image

from import_gltf_scene import ROOT, accessor_values, digest, read_json, write_json


def main():
    root = ROOT/'Examples/RenderingTests/SourceAssets/RenderingBenchmarks/Bistro'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exported',type=Path,default=root/'converted/BistroExterior.gltf')
    parser.add_argument('--source-root',type=Path,default=root/'extracted/Bistro_v5_2')
    parser.add_argument('--output',type=Path,default=root/'converted/StaticPose/BistroExteriorStaticPose.gltf')
    parser.add_argument('--reuse-images',action='store_true')
    args = parser.parse_args()
    document = read_json(args.exported)
    metadata = read_json(args.exported.with_suffix('.fbxmetadata.json'))
    original_animation = copy.deepcopy(document.get('animations',[]))
    outdir = args.output.resolve().parent
    outdir.mkdir(parents=True,exist_ok=True)
    previous = read_json(outdir/'normalization.json') if args.reuse_images else None
    if previous and previous['sourceExportSha256'] != digest(args.exported):
        raise ValueError('Image cache belongs to a different source export')
    buffers = [(args.exported.parent/item['uri']).read_bytes() for item in document['buffers']]
    pose_samples = []
    for animation in original_animation:
        for channel in animation['channels']:
            sampler = animation['samplers'][channel['sampler']]
            if sampler.get('interpolation','LINEAR') not in ['LINEAR','STEP']:
                raise ValueError('Unsupported time-zero interpolation')
            times = list(accessor_values(document,buffers,sampler['input']))
            if not times:
                raise ValueError('Empty source animation sampler')
            outputs = list(accessor_values(document,buffers,sampler['output']))
            lower = max([i for i,t in enumerate(times) if t[0] <= 0] or [0])
            upper = min(lower+1,len(times)-1)
            weight = 0.0 if upper == lower else max(0.0,min(1.0,-times[lower][0]/(times[upper][0]-times[lower][0])))
            if sampler.get('interpolation','LINEAR') == 'STEP':
                weight = 0.0
            left,right = list(outputs[lower]),list(outputs[upper])
            if channel['target']['path'] == 'rotation':
                dot = sum(a*b for a,b in zip(left,right))
                if dot < 0:
                    right = [-v for v in right]
                    dot = -dot
                if dot < .9995:
                    theta = math.acos(max(-1.0,min(1.0,dot)))
                    value = [(math.sin((1-weight)*theta)*a+math.sin(weight*theta)*b)/math.sin(theta) for a,b in zip(left,right)]
                else:
                    value = [(1-weight)*a+weight*b for a,b in zip(left,right)]
                length = math.sqrt(sum(v*v for v in value))
                value = [v/length for v in value]
            else:
                value = [(1-weight)*a+weight*b for a,b in zip(left,right)]
            node = channel['target']['node']
            path = channel['target']['path']
            if path not in ['translation','rotation','scale']:
                raise ValueError('Non-node animation cannot be represented as this static-pose benchmark')
            document['nodes'][node][path] = value
            pose_samples.append({'node':node,'name':document['nodes'][node]['name'],'path':path,'time':0,'value':value})
    document.pop('animations',None)
    source_materials = read_json(args.exported.with_suffix('.source.json'))['materials']
    if [m['name'] for m in source_materials] != [m['name'] for m in document['materials']]:
        raise ValueError('Exported material ordering differs from the audited source')
    normal_images, base_images = set(), set()
    material_mappings = []
    for material in document['materials']:
        pbr = material['pbrMetallicRoughness']
        specular = material.get('extensions',{}).get('KHR_materials_specular',{})
        packed = copy.deepcopy(specular.get('specularTexture'))
        if packed:
            pbr['metallicRoughnessTexture'] = packed
            pbr['metallicFactor'] = 1.0
            pbr['roughnessFactor'] = 1.0
        material.pop('extensions',None)
        normal_image = document['textures'][material['normalTexture']['index']]['source']
        normal_images.add(normal_image)
        base_image = document['textures'][pbr['baseColorTexture']['index']]['source']
        base_images.add(base_image)
        material['doubleSided'] = '.doublesided' in material['name'].lower()
        material['alphaCutoff'] = 0.5
        material_mappings.append({'name':material['name'],'baseImage':base_image,'normalImage':normal_image,
            'packedTexture':packed['index'] if packed else None,'doubleSided':material['doubleSided'],
            'metallicRoughness':'Source Specular G=roughness, B=metallic; multiply factors=1' if packed else 'No source Specular map: retain Assimp scalar conversion from FBX (metal=0, roughness from shininess)',
            'occlusionPolicy':'Source R retained in packed texture but unused, matching Falcor documented unsupported AO channel'})
    if normal_images & base_images:
        raise ValueError('Source image has ambiguous color and normal semantics')
    image_inventory, alpha_ranges, converted_cache = [], {}, {}
    for index,image in enumerate(document['images']):
        original_uri = image['uri'].replace('\\','/')
        original = (args.source_root/original_uri).resolve()
        original.relative_to(args.source_root.resolve())
        key = (str(original).lower(),index in normal_images)
        if key in converted_cache:
            cached = converted_cache[key]
            image['uri'] = cached['output']
            alpha_ranges[index] = cached['alphaRange']
            image_inventory.append(dict(cached,index=index))
            continue
        target = outdir/'textures'/('orca_image_%03d.png'%index)
        target.parent.mkdir(parents=True,exist_ok=True)
        if previous:
            info = previous['images'][index]
            target = outdir/info['output']
            if info['sourceSha256'] != digest(original) or info['outputSha256'] != digest(target):
                raise ValueError('Normalized texture cache content changed')
            image_inventory.append(info)
            alpha_ranges[index] = info['alphaRange']
            converted_cache[key] = info
            image['uri'] = info['output']
            continue
        with Image.open(str(original)) as decoded:
            rgba = np.array(decoded.convert('RGBA'),dtype=np.uint8)
        alpha_range = [int(rgba[:,:,3].min()),int(rgba[:,:,3].max())]
        alpha_ranges[index] = alpha_range
        if index in normal_images:
            # ORCA normal maps use BC5/ATI2 XY. Pillow decodes B as zero; recover
            # the positive tangent Z hemisphere, then convert DirectX -Y to glTF +Y.
            xy = rgba[:,:,:2].astype(np.float32)*(2.0/255.0)-1.0
            z = np.sqrt(np.maximum(0.0,1.0-np.sum(xy*xy,axis=2)))
            rgba[:,:,1] = 255-rgba[:,:,1]
            rgba[:,:,2] = np.rint((z*.5+.5)*255.0).astype(np.uint8)
            rgba[:,:,3] = 255
        Image.fromarray(rgba).save(str(target),compress_level=3)
        info = {'index':index,'source':original_uri,'sourceSha256':digest(original),
            'output':target.relative_to(outdir).as_posix(),'outputSha256':digest(target),
            'width':rgba.shape[1],'height':rgba.shape[0],'alphaRange':alpha_range,
            'normalConversion':'BC5 XY -> positive Z; G=255-G (DirectX to glTF)' if index in normal_images else 'DDS decoded; original channel bytes retained'}
        image_inventory.append(info)
        converted_cache[key] = info
        image['uri'] = info['output']
        if (index+1)%50 == 0:
            print('normalized ORCA textures %d/%d'%(index+1,len(document['images'])),flush=True)
    for material,mapping in zip(document['materials'],material_mappings):
        alpha = alpha_ranges[mapping['baseImage']]
        # Falcor BasicMaterial::updateAlphaMode uses Mask when min alpha is less
        # than its threshold, otherwise Opaque. Keep the asset's authored opacity.
        material['alphaMode'] = 'MASK' if alpha[0] < 128 else 'OPAQUE'
        mapping['sourceAlphaRange'] = alpha
        mapping['alphaMode'] = material['alphaMode']
        mapping['alphaCutoff'] = 0.5
    for index,item in enumerate(document['buffers']):
        filename = 'BistroExteriorStaticPose_%d.bin'%index
        (outdir/filename).write_bytes(buffers[index])
        item['uri'] = filename
    for mesh in document['meshes']:
        for primitive in mesh['primitives']:
            primitive.pop('extensions',None)
    camera_attribute = next(a for a in metadata['attributes'] if a['identity'][-1]=='Camera')
    camera = camera_attribute['properties']
    aspect = camera['AspectWidth'][0]/camera['AspectHeight'][0]
    hfov = 2*math.atan2(camera['FilmWidth'][0]*25.4*.5,camera['FocalLength'][0])
    yfov = 2*math.atan(math.tan(hfov*.5)/aspect)
    document['cameras'] = [{'name':'ORCA source camera','type':'perspective',
        'perspective':{'aspectRatio':aspect,'yfov':yfov,'znear':camera['NearPlane'][0],'zfar':camera['FarPlane'][0]}}]
    camera_parent = next(i for i,n in enumerate(document['nodes']) if n['name']=='Camera')
    camera_child = len(document['nodes'])
    document['nodes'].append({'name':'ORCA Camera glTF Axis Adapter','camera':0,'rotation':[0,-math.sqrt(.5),0,math.sqrt(.5)]})
    document['nodes'][camera_parent].setdefault('children',[]).append(camera_child)
    light_attribute = next(a for a in metadata['attributes'] if a['identity'][-1]=='Light')
    light = light_attribute['properties']
    if light['LightType'][0] != 1:
        raise ValueError('Expected the authored exterior directional light')
    document['extensions'] = {'KHR_lights_punctual':{'lights':[{'name':'ORCA authored directional light',
        'type':'directional','color':light['Color'],'intensity':light['Intensity'][0]/100.0}]}}
    document['extensionsUsed'] = ['KHR_lights_punctual']
    document.pop('extensionsRequired',None)
    light_parent = next(i for i,n in enumerate(document['nodes']) if n['name']=='directionalLight1')
    light_child = len(document['nodes'])
    document['nodes'].append({'name':'ORCA Light glTF Axis Adapter','rotation':[-math.sqrt(.5),0,0,math.sqrt(.5)],
        'extensions':{'KHR_lights_punctual':{'light':0}}})
    document['nodes'][light_parent].setdefault('children',[]).append(light_child)
    # Recover exact shared mesh instances that the FBX reader duplicates.
    # Identity includes all geometry attributes, topology, and material bindings.
    # Non-identical meshes remain separate, even if their names look similar.
    mesh_hashes, mesh_map, shared_meshes = {}, {}, []
    for mesh_index,mesh in enumerate(document['meshes']):
        h = hashlib.sha256()
        for primitive in mesh['primitives']:
            h.update(json.dumps({'material':primitive.get('material'),'mode':primitive.get('mode',4)},sort_keys=True).encode())
            for name,accessor_index in sorted(list(primitive['attributes'].items())+[('INDICES',primitive['indices'])]):
                accessor = document['accessors'][accessor_index]
                view = document['bufferViews'][accessor['bufferView']]
                count = {'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4}[accessor['type']]
                size = {5121:1,5123:2,5125:4,5126:4}[accessor['componentType']]*count
                start = view.get('byteOffset',0)+accessor.get('byteOffset',0)
                length = view.get('byteStride',size)*(accessor['count']-1)+size
                h.update(name.encode())
                h.update(json.dumps({'count':accessor['count'],'type':accessor['type'],'componentType':accessor['componentType'],
                                     'normalized':accessor.get('normalized',False)},sort_keys=True).encode())
                h.update(buffers[view['buffer']][start:start+length])
        key = h.hexdigest()
        if key not in mesh_hashes:
            mesh_hashes[key] = len(shared_meshes)
            shared_meshes.append(mesh)
        mesh_map[mesh_index] = mesh_hashes[key]
    for node in document['nodes']:
        if 'mesh' in node:
            node['mesh'] = mesh_map[node['mesh']]
    original_mesh_count = len(document['meshes'])
    document['meshes'] = shared_meshes
    write_json(args.output,document)
    report = {'sourceExport':str(args.exported.resolve()),'sourceExportSha256':digest(args.exported),
        'normalizedGltfSha256':digest(args.output),'staticPose':{'sampleTimeSeconds':0,'sampledChannels':pose_samples,
            'sourceAnimationCount':len(original_animation),'animationPlaybackImported':False,
            'policy':'Named static-pose benchmark only; original camera and wind animation remain in unmodified source export'},
        'sourceUnits':metadata['globalSettings'],'unitPolicy':'FBX UnitScaleFactor=100 cm (metres); all authored root/node scaling preserved, no extra global scaling',
        'camera':{'originalProperties':camera,'yfovRadians':yfov,'axisAdapter':'glTF -Z -> FBX/Assimp local +X'},
        'light':{'originalProperties':light,'axisAdapter':'glTF -Z -> FBX/Assimp local -Y'},
        'materialMappings':material_mappings,'images':image_inventory,
        'falcorReference':'https://github.com/NVIDIAGameWorks/Falcor/blob/master/docs/usage/scene-formats.md',
        'alphaReference':'https://github.com/NVIDIAGameWorks/Falcor/blob/master/Source/Falcor/Scene/Material/BasicMaterial.cpp',
        'doubleSidedPolicy':'Authored material-name .DoubleSided flag, documented in ORCA v3a changelog',
        'meshSharing':{'sourceMeshCount':original_mesh_count,'sharedMeshCount':len(shared_meshes),
                       'sourceMeshToSharedMesh':mesh_map,'policy':'Exact geometry/topology/material byte identity only; all scene nodes retained'},
        'rendererValidation':'Pending; source normalization is not image quality/performance evidence'}
    write_json(outdir/'normalization.json',report)
    print(json.dumps({'nodes':len(document['nodes']),'meshes':len(document['meshes']),
        'materials':len(document['materials']),'uniqueImages':len(converted_cache),'staticPoseTime':0,
        'doubleSided':sum(m['doubleSided'] for m in document['materials']),
        'maskMaterials':sum(m['alphaMode']=='MASK' for m in document['materials'])},indent=2))


if __name__ == '__main__':
    main()
