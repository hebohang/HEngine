#!/usr/bin/env python3
"""Import a static glTF scene through HAssetImporter without flattening its nodes.

The original geometry and source images are immutable. Each shared mesh is
imported once at identity, while the .he scene retains the authored hierarchy.
The converter rejects unsupported meaningful attributes instead of losing them.
Python 3.7+ and Pillow are required; no engine or generated source is edited.
"""
import argparse
import collections
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[3]
PROJECT = ROOT / 'Examples/RenderingTests'
SOURCE_ROOT = PROJECT / 'SourceAssets' / 'RenderingBenchmarks' / 'IntelSponza'
DEFAULT_SOURCE = SOURCE_ROOT / 'extracted' / 'main_sponza' / 'NewSponza_Main_glTF_003.gltf'
NAMESPACE = uuid.UUID('a0387ce7-b962-48fc-bebc-ea2563fe98b0')


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def relative_asset(path):
    return path.resolve().relative_to(PROJECT.resolve()).as_posix()


def source_uri(source, uri):
    from urllib.parse import unquote
    if ':' in uri or '\\' in uri:
        raise ValueError('Only local relative URI references are supported: ' + uri)
    path = (source.parent / unquote(uri)).resolve()
    path.relative_to(source.parent.resolve())
    if not path.is_file():
        raise ValueError('Missing source dependency: ' + str(path))
    return path


def accessor_values(document, buffers, index):
    accessor = document['accessors'][index]
    if 'sparse' in accessor:
        raise ValueError('Sparse accessors require an explicit conversion')
    view = document['bufferViews'][accessor['bufferView']]
    count = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[accessor['type']]
    kind = {5120: 'b', 5121: 'B', 5122: 'h', 5123: 'H', 5125: 'I', 5126: 'f'}[accessor['componentType']]
    fmt = '<' + kind * count
    size = struct.calcsize(fmt)
    offset = view.get('byteOffset', 0) + accessor.get('byteOffset', 0)
    stride = view.get('byteStride', size)
    data = buffers[view['buffer']]
    for i in range(accessor['count']):
        yield struct.unpack_from(fmt, data, offset + stride * i)


def quaternion_matrix(q):
    x, y, z, w = q
    n = math.sqrt(sum(v * v for v in q))
    if n < 1e-12:
        raise ValueError('Invalid zero quaternion')
    x, y, z, w = [v / n for v in (x, y, z, w)]
    return [[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
            [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
            [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]]


def quaternion_euler(q):
    # GLM quat(vec3) uses the same intrinsic XYZ / matrix Rz * Ry * Rx order.
    m = quaternion_matrix(q)
    y = math.asin(max(-1.0, min(1.0, -m[2][0])))
    if abs(math.cos(y)) > 1e-7:
        x, z = math.atan2(m[2][1], m[2][2]), math.atan2(m[1][0], m[0][0])
    else:
        x, z = math.atan2(-m[1][2], m[1][1]), 0.0
    cx, sx, cy, sy, cz, sz = math.cos(x/2), math.sin(x/2), math.cos(y/2), math.sin(y/2), math.cos(z/2), math.sin(z/2)
    reconstructed = quaternion_matrix([sx*cy*cz-cx*sy*sz, cx*sy*cz+sx*cy*sz,
                                       cx*cy*sz-sx*sy*cz, cx*cy*cz+sx*sy*sz])
    error = max(abs(m[r][c] - reconstructed[r][c]) for r in range(3) for c in range(3))
    if error > 2e-6:
        raise ValueError('Quaternion/Euler round-trip failed: ' + str(error))
    return [x, y, z]


def audit_source(source, document, buffers):
    if document.get('skins') or document.get('animations'):
        raise ValueError('This static scene converter does not silently discard skinning or animation')
    supported_extensions = {'KHR_lights_punctual', 'KHR_materials_emissive_strength'}
    unsupported = set(document.get('extensionsUsed', [])) - supported_extensions
    if unsupported:
        raise ValueError('Unsupported glTF extensions: ' + ', '.join(sorted(unsupported)))
    attrs, colors, triangles, vertices = collections.Counter(), [], 0, 0
    mesh_triangles = []
    for mesh_index, mesh in enumerate(document['meshes']):
        mesh_triangle_count = 0
        for prim in mesh['primitives']:
            if prim.get('mode', 4) != 4 or prim.get('targets'):
                raise ValueError('Only static triangle primitives are supported')
            attrs.update(prim['attributes'].keys())
            triangles += document['accessors'][prim['indices']]['count'] // 3
            mesh_triangle_count += document['accessors'][prim['indices']]['count'] // 3
            vertices += document['accessors'][prim['attributes']['POSITION']]['count']
            for attribute in prim['attributes']:
                if attribute not in {'POSITION', 'NORMAL', 'TANGENT', 'TEXCOORD_0', 'TEXCOORD_1', 'COLOR_0'}:
                    raise ValueError('Unsupported attribute: ' + attribute)
            if 'COLOR_0' in prim['attributes']:
                accessor = document['accessors'][prim['attributes']['COLOR_0']]
                white = {5121: 255, 5123: 65535, 5126: 1.0}[accessor['componentType']]
                if any(any(value != white for value in row) for row in accessor_values(document, buffers, prim['attributes']['COLOR_0'])):
                    raise ValueError('Non-white COLOR_0 requires engine vertex-color support')
                colors.append({'mesh': mesh_index, 'count': accessor['count'], 'allWhite': True})
        mesh_triangles.append(mesh_triangle_count)
    materials = []
    for index, material in enumerate(document['materials']):
        pbr = material.get('pbrMetallicRoughness', {})
        slots = []
        for name, container, space, channels in [
                ('baseColorTexture', pbr, 'SRGB', 'RGB base color; A linear opacity'),
                ('metallicRoughnessTexture', pbr, 'Linear', 'G roughness; B metallic'),
                ('normalTexture', material, 'Linear', 'XYZ tangent-space normal'),
                ('occlusionTexture', material, 'Linear', 'R ambient occlusion'),
                ('emissiveTexture', material, 'SRGB', 'RGB emission')]:
            if name not in container:
                continue
            texture = container[name]
            if texture.get('texCoord', 0) != 0 or texture.get('extensions'):
                raise ValueError('Non-UV0 or transformed texture mapping needs an explicit engine adapter')
            image_index = document['textures'][texture['index']]['source']
            slots.append({'slot': name, 'image': image_index, 'uri': document['images'][image_index]['uri'],
                          'colorSpace': space, 'channels': channels})
        materials.append({'index': index, 'name': material.get('name', ''), 'source': material,
                          'slots': slots, 'alphaMode': material.get('alphaMode', 'OPAQUE'),
                          'doubleSided': material.get('doubleSided', False)})
    for node in document['nodes']:
        if 'matrix' in node:
            raise ValueError('Authored node matrices must be decomposed and shear-checked explicitly')
        quaternion_euler(node.get('rotation', [0, 0, 0, 1]))
    return {'sourceSha256': digest(source), 'generator': document.get('asset'),
            'nodes': len(document['nodes']), 'meshes': len(document['meshes']),
            'meshInstances': sum('mesh' in node for node in document['nodes']),
            'triangles':sum(mesh_triangles[node['mesh']] for node in document['nodes'] if 'mesh' in node),
            'uniqueTriangles':triangles, 'vertices': vertices, 'attributes': dict(attrs),
            'whiteVertexColorPrimitives': colors, 'materials': materials,
            'coordinateSystem': 'glTF right-handed, +Y up; metres; unchanged local mesh positions and node TRS',
            'discardedUnusedAttributes': 'TEXCOORD_1 (no material references it); COLOR_0 only after every component is verified white',
            'lights': document.get('extensions', {}).get('KHR_lights_punctual', {}).get('lights', [])}


def resize_image(source, output, max_dimension, srgb):
    from PIL import Image
    with Image.open(str(source)) as original:
        image = original.convert('RGBA')
        dimensions = image.size
        scale = min(1.0, float(max_dimension) / max(dimensions))
        target = tuple(max(1, int(round(value * scale))) for value in dimensions)
        if target != dimensions:
            if srgb:
                # Decode RGB before filtering. Alpha always remains linear.
                lookup = [(v/255.0)/12.92 if v/255.0 <= .04045 else ((v/255.0+.055)/1.055)**2.4 for v in range(256)]
                channels = []
                for channel in image.split()[:3]:
                    linear = channel.point(lookup, 'F').resize(target, Image.LANCZOS)
                    # Pillow's float point callable cannot express piecewise sRGB;
                    # quantize linear values to 16 bits, then apply a 65536-entry LUT.
                    quantized = linear.point(lambda value: value * 65535.0).convert('I')
                    lut = [int(round(255.0 * (12.92*(v/65535.0) if v/65535.0 <= .0031308 else 1.055*(v/65535.0)**(1/2.4)-.055))) for v in range(65536)]
                    raw = quantized.tobytes()
                    try:
                        import numpy as np
                    except ImportError:
                        np = None
                    if np is None:
                        encoded = bytes(lut[max(0,min(65535,x[0]))] for x in struct.iter_unpack('<i',raw))
                    else:
                        encoded = np.asarray(lut,dtype=np.uint8)[np.clip(np.frombuffer(raw,dtype='<i4'),0,65535)].tobytes()
                    channels.append(Image.frombytes('L',target,encoded))
                channels.append(image.getchannel('A').resize(target, Image.LANCZOS))
                image = Image.merge('RGBA', channels)
            else:
                image = image.resize(target, Image.LANCZOS)
        output.parent.mkdir(parents=True, exist_ok=True)
        image.save(str(output), compress_level=3)
        return {'sourceDimensions': list(dimensions), 'outputDimensions': list(target),
                'filter': 'Lanczos3; RGB filtered in linear light' if srgb else 'Lanczos3; linear data',
                'colorSpace': 'SRGB' if srgb else 'Linear'}


def compact_mesh(document, buffers, mesh_index, directory, prefix='sponza'):
    mesh = copy.deepcopy(document['meshes'][mesh_index])
    accessors, views, blob, accessor_map = [], [], bytearray(), {}
    def remap(index):
        if index in accessor_map:
            return accessor_map[index]
        accessor = copy.deepcopy(document['accessors'][index])
        # Exporters can share a few bufferViews across the entire scene. Copy
        # this accessor's exact byte range instead of its whole bufferView.
        old_view = document['bufferViews'][accessor['bufferView']]
        component_size = {5120:1,5121:1,5122:2,5123:2,5125:4,5126:4}[accessor['componentType']]
        component_count = {'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4}[accessor['type']]
        element_size = component_size*component_count
        stride = old_view.get('byteStride', element_size)
        start = old_view.get('byteOffset', 0)+accessor.get('byteOffset', 0)
        length = stride*(accessor['count']-1)+element_size
        while len(blob) % 4:
            blob.append(0)
        view = {'buffer':0,'byteOffset':len(blob),'byteLength':length}
        if 'byteStride' in old_view:
            view['byteStride'] = stride
        if 'target' in old_view:
            view['target'] = old_view['target']
        blob.extend(buffers[old_view['buffer']][start:start+length])
        accessor['bufferView'] = len(views)
        accessor['byteOffset'] = 0
        views.append(view)
        accessor_map[index] = len(accessors)
        accessors.append(accessor)
        return accessor_map[index]
    for primitive in mesh['primitives']:
        primitive['indices'] = remap(primitive['indices'])
        primitive['attributes'] = {name: remap(index) for name, index in primitive['attributes'].items()
                                   if name in {'POSITION', 'NORMAL', 'TANGENT', 'TEXCOORD_0'}}
    name = prefix+'_mesh_%03d' % mesh_index
    path = directory / (name + '.gltf')
    material_stubs = [{'name': '_geometry_material_%03d' % i} for i in range(len(document['materials']))]
    fixture = {'asset': {'version': '2.0', 'generator': 'HEngine node-preserving static scene importer v1'},
               'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0}], 'meshes': [mesh],
               'materials': material_stubs, 'accessors': accessors, 'bufferViews': views,
               'buffers': [{'uri': name + '.bin', 'byteLength': len(blob)}]}
    write_json(path, fixture)
    path.with_suffix('.bin').write_bytes(blob)


def prepare(arguments):
    source, directory = arguments.source.resolve(), arguments.prepared.resolve()
    document = read_json(source)
    buffers = [source_uri(source, entry['uri']).read_bytes() for entry in document['buffers']]
    audit = audit_source(source, document, buffers)
    previous_conversion = None
    if arguments.reuse_prepared_textures:
        previous_conversion = read_json(directory/'conversion.json')
        if previous_conversion['sourceSha256'] != digest(source) or previous_conversion['textureMaximumDimension'] != arguments.texture_size:
            raise ValueError('Prepared texture cache belongs to a different source or configuration')
    directory.mkdir(parents=True, exist_ok=True)
    write_json(directory / 'source_audit.json', audit)
    fixture = {'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}],
               'nodes': [{'mesh': 0}], 'meshes': [{'primitives': []}],
               'materials': copy.deepcopy(document['materials']),
               'textures': copy.deepcopy(document.get('textures', [])),
               'samplers': copy.deepcopy(document.get('samplers', [])),
               'images': copy.deepcopy(document.get('images', [])),
               'bufferViews': [{'buffer': 0, 'byteOffset': 0, 'byteLength': 36},
                               {'buffer': 0, 'byteOffset': 36, 'byteLength': 6}],
               'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3',
                              'min': [0, 0, 0], 'max': [1, 1, 0]},
                             {'bufferView': 1, 'componentType': 5123, 'count': 3, 'type': 'SCALAR'}],
               'buffers': [{'uri': 'material_fixture.bin', 'byteLength': 42}]}
    for index, material in enumerate(fixture['materials']):
        material['name'] = arguments.mesh_prefix+'_material_%03d' % index
        fixture['meshes'][0]['primitives'].append({'attributes': {'POSITION': 0}, 'indices': 1, 'material': index})
    srgb_images = {slot['image'] for material in audit['materials'] for slot in material['slots'] if slot['colorSpace'] == 'SRGB'}
    linear_images = {slot['image'] for material in audit['materials'] for slot in material['slots'] if slot['colorSpace'] == 'Linear'}
    if srgb_images & linear_images:
        raise ValueError('An image used with both color spaces requires per-semantic texture variants')
    inventory = []
    for index, entry in enumerate(fixture['images']):
        original = source_uri(source, entry['uri'])
        target = directory / 'textures' / ('image_%03d.png' % index)
        if arguments.reuse_prepared_textures and target.is_file():
            previous_texture = previous_conversion['textures'][index]
            if previous_texture['sourceSha256'] != digest(original) or previous_texture['outputSha256'] != digest(target):
                raise ValueError('Prepared texture cache source or output hash changed')
            from PIL import Image
            with Image.open(str(original)) as original_image, Image.open(str(target)) as prepared_image:
                scale = min(1.0, float(arguments.texture_size)/max(original_image.size))
                expected_size = tuple(max(1,int(round(value*scale))) for value in original_image.size)
                if prepared_image.size != expected_size:
                    raise ValueError('Existing prepared texture dimensions differ from requested quality')
                metadata = {'sourceDimensions':list(original_image.size), 'outputDimensions':list(prepared_image.size),
                    'filter':'Lanczos3; RGB filtered in linear light' if index in srgb_images else 'Lanczos3; linear data',
                    'colorSpace':'SRGB' if index in srgb_images else 'Linear'}
        else:
            metadata = resize_image(original, target, arguments.texture_size, index in srgb_images)
        metadata.update({'index': index, 'source': document['images'][index]['uri'],
                         'sourceSha256': digest(original), 'output': str(target.relative_to(directory)).replace('\\', '/'),
                         'outputSha256': digest(target)})
        inventory.append(metadata)
        entry['uri'] = target.relative_to(directory).as_posix()
        if (index+1) % 12 == 0:
            print('prepared textures %d/%d' % (index+1, len(fixture['images'])), flush=True)
    write_json(directory / 'material_fixture.gltf', fixture)
    (directory / 'material_fixture.bin').write_bytes(struct.pack('<9f3H', 0,0,0, 1,0,0, 0,1,0, 0,1,2))
    for index in range(len(document['meshes'])):
        compact_mesh(document, buffers, index, directory / 'meshes', arguments.mesh_prefix)
    write_json(directory / 'conversion.json', {'version': 1, 'source': str(source), 'sourceSha256': digest(source),
        'sceneName':arguments.scene_name,'meshPrefix':arguments.mesh_prefix,
        'textureMaximumDimension': arguments.texture_size, 'textures': inventory,
        'sourceTextureBytesRGBA8WithMips': sum(v['sourceDimensions'][0]*v['sourceDimensions'][1]*4*4//3 for v in inventory),
        'outputTextureBytesRGBA8WithMips': sum(v['outputDimensions'][0]*v['outputDimensions'][1]*4*4//3 for v in inventory),
        'nodes': document['nodes'], 'scenes': document['scenes'], 'defaultScene': document.get('scene', 0),
        'meshCount': len(document['meshes']), 'materialCount': len(document['materials']),
        'cameras': document.get('cameras', []), 'extensions': document.get('extensions', {})})
    print('prepare PASS: %d meshes, %d nodes, %d textures; size=%d' % (len(document['meshes']), len(document['nodes']), len(inventory), arguments.texture_size))


def run_importer(importer, source, output, log):
    output.mkdir(parents=True, exist_ok=True)
    command = [str(importer.resolve()), str(PROJECT.resolve()), str(source.resolve()), str(output.resolve())]
    start = time.monotonic()
    with log.open('w', encoding='utf-8') as stream:
        result = subprocess.run(command, cwd=str(ROOT), stdout=stream, stderr=subprocess.STDOUT, timeout=180)
    if result.returncode:
        raise ValueError('Importer failed (%d); log: %s' % (result.returncode, log))
    return {'command': command, 'seconds': time.monotonic()-start, 'exitCode': result.returncode}


def node_scene(conversion, output):
    scene_name = conversion.get('sceneName','IntelSponza')
    mesh_prefix = conversion.get('meshPrefix','sponza')
    parents = {}
    for index, node in enumerate(conversion['nodes']):
        for child in node.get('children', []):
            if child in parents:
                raise ValueError('Node has multiple parents')
            parents[child] = index
    identifiers = [str(uuid.uuid5(NAMESPACE, scene_name+':node:%d' % index)) for index in range(len(conversion['nodes']))]
    result = {}
    first_camera = True
    for index, node in enumerate(conversion['nodes']):
        entity = {'TagComponent': {'Tag': node.get('name', 'Node %d' % index)},
                  'TransformComponent': {'Translation': node.get('translation', [0,0,0]),
                     'Rotation': quaternion_euler(node.get('rotation', [0,0,0,1])), 'Scale': node.get('scale', [1,1,1]),
                     'ParentUuid': identifiers[parents[index]] if index in parents else ''}}
        if 'mesh' in node:
            path = relative_asset(output / 'Meshes' / (mesh_prefix+'_mesh_%03d.hmesh' % node['mesh']))
            entity['MeshComponent'] = {'Path': path, 'meshResPath': path}
        if 'camera' in node:
            camera = conversion['cameras'][node['camera']]
            if camera['type'] != 'perspective':
                raise ValueError('Unsupported authored camera type')
            perspective = camera['perspective']
            entity['CameraComponent'] = {'Camera': {'mProjectionType': 0, 'mAspectRatio': 16.0/9.0,
                'mPerspectiveFOV': perspective['yfov'], 'mPerspectiveNear': perspective['znear'],
                'mPerspectiveFar': perspective.get('zfar', 10000.0)}, 'FixedAspectRatio': False, 'Primary': first_camera}
            first_camera = False
        light_ref = node.get('extensions',{}).get('KHR_lights_punctual',{})
        if 'light' in light_ref:
            light = conversion.get('extensions',{}).get('KHR_lights_punctual',{}).get('lights',[])[light_ref['light']]
            if light.get('intensity',1.0) > 0:
                if light['type'] != 'directional':
                    raise ValueError('Positive local source lights require an explicit radius/spot engine adapter')
                entity['DirectionalLightComponent'] = {'Intensity':light.get('intensity',1.0),'Color':light.get('color',[1,1,1]),
                    'bCastShadows':True,'CascadeCount':4,'ShadowMapSize':2048,'ShadowDistance':80.0,
                    'CascadeLambda':0.75,'DepthBias':0.0015,'NormalBias':0.05,'bUsePCF':True,'PCFRadius':1.5,'ShadowStrength':1.0}
        result[identifiers[index]] = entity
    return result


def daylight_scene(conversion, output):
    scene = node_scene(conversion, output)
    for entity in scene.values():
        if entity.get('CameraComponent', {}).get('Primary'):
            entity['CameraComponent']['Camera']['mPerspectiveNear'] = 0.1
            entity['CameraComponent']['Camera']['mPerspectiveFar'] = 100.0
    identity = {'Translation':[0,0,0], 'Rotation':[0,0,0], 'Scale':[1,1,1], 'ParentUuid':''}
    sun_transform = dict(identity)
    sun_transform['Rotation'] = [-2.55, 0.75, 0.2]
    scene[str(uuid.uuid5(NAMESPACE, 'IntelSponza:DaylightSun'))] = {
        'TagComponent':{'Tag':'Benchmark Daylight Sun (3.0)'}, 'TransformComponent':sun_transform,
        'DirectionalLightComponent':{'Intensity':3.0,'Color':[1.0,0.95,0.85], 'bCastShadows':True,
            'CascadeCount':4,'ShadowMapSize':2048,'ShadowDistance':80.0,'CascadeLambda':0.75,
            'DepthBias':0.0015,'NormalBias':0.05,'bUsePCF':True,'PCFRadius':1.5,'ShadowStrength':1.0}}
    scene[str(uuid.uuid5(NAMESPACE, 'IntelSponza:DaylightEnvironment'))] = {
        'TagComponent':{'Tag':'Benchmark PBRTest Daylight Environment'}, 'TransformComponent':identity,
        'EnvironmentLightComponent':{'HdrTexturePath':'Assets/Textures/Hdr/snowy_field_2k.hdr',
            'ExposureEV100':1.6896598793878495,'ExposureCompensation':0.0,'IBLIntensity':1.0}}
    return scene


def import_scene(arguments):
    directory, output = arguments.prepared.resolve(), arguments.output.resolve()
    output.relative_to((PROJECT / 'Assets' / 'RenderingBenchmarks').resolve())
    conversion = read_json(directory / 'conversion.json')
    scene_name = conversion.get('sceneName','IntelSponza')
    mesh_prefix = conversion.get('meshPrefix','sponza')
    output.mkdir(parents=True, exist_ok=True)
    marker = output / 'import_progress.json'
    if not marker.exists() and any(output.iterdir()):
        raise ValueError('Refusing to overwrite an unowned existing output directory')
    progress = read_json(marker) if marker.exists() else {'sourceSha256': conversion['sourceSha256'],
        'textureMaximumDimension': conversion['textureMaximumDimension'], 'importerSha256': digest(arguments.importer),
        'materialsImported': False, 'meshes': {}}
    if progress['sourceSha256'] != conversion['sourceSha256'] or progress['textureMaximumDimension'] != conversion['textureMaximumDimension']:
        raise ValueError('Use a separate output directory for a different source/configuration')
    write_json(marker, progress)
    logs = directory / 'logs'
    logs.mkdir(exist_ok=True)
    previous_material_directory = progress.get('materialDirectory', 'Materials')
    if arguments.refresh_materials:
        progress['materialDirectory'] = 'Materials-' + digest(arguments.importer)[:12]
        if progress['materialDirectory'] != previous_material_directory:
            progress.setdefault('materialImportHistory', []).append(progress.get('materialImport', {}))
            progress['materialsImported'] = False
    materials = output / progress.get('materialDirectory', 'Materials')
    if not progress['materialsImported']:
        progress['materialImport'] = run_importer(arguments.importer, directory/'material_fixture.gltf', materials, logs/'materials.log')
        progress['materialImporterSha256'] = digest(arguments.importer)
        progress['materialsImported'] = True
        write_json(marker, progress)
    for index in range(conversion['materialCount']):
        material = materials / (mesh_prefix+'_material_%03d.hmat' % index)
        if not material.is_file():
            raise ValueError('Expected deterministic imported material: ' + str(material))
    for index in range(conversion['meshCount']):
        if str(index) in progress['meshes']:
            if previous_material_directory != progress.get('materialDirectory', 'Materials'):
                path = output / 'Meshes' / (mesh_prefix+'_mesh_%03d.hmesh' % index)
                mesh = read_json(path)
                for primitive in mesh['primitives']:
                    primitive['materialResPath'] = relative_asset(materials/Path(primitive['materialResPath']).name)
                write_json(path, mesh)
                progress['meshes'][str(index)]['meshSha256'] = digest(path)
            continue
        stem = mesh_prefix+'_mesh_%03d' % index
        meshes = output / 'Meshes'
        evidence = run_importer(arguments.importer, directory/'meshes'/(stem+'.gltf'), meshes, logs/(stem+'.log'))
        path = meshes / (stem+'.hmesh')
        mesh = read_json(path)
        temporary_materials = set()
        for primitive in mesh['primitives']:
            stub = PROJECT / primitive['materialResPath']
            name = read_json(stub)['Properties']['name']
            if not name.startswith('_geometry_material_'):
                raise ValueError('Unexpected geometry material name: ' + name)
            material_index = int(name.rsplit('_', 1)[-1])
            primitive['materialResPath'] = relative_asset(materials / (mesh_prefix+'_material_%03d.hmat' % material_index))
            temporary_materials.add(stub.resolve())
        write_json(path, mesh)
        for stub in temporary_materials:
            stub.relative_to(meshes.resolve())
            stub.unlink()
        evidence['meshSha256'] = digest(path)
        progress['meshes'][str(index)] = evidence
        write_json(marker, progress)
        if (index+1) % 10 == 0:
            print('imported meshes %d/%d' % (index+1, conversion['meshCount']), flush=True)
    write_json(marker, progress)
    scene = node_scene(conversion, output)
    write_json(output / (scene_name+'.he'), scene)
    if scene_name == 'IntelSponza':
        write_json(output / 'IntelSponzaDaylight.he', daylight_scene(conversion, output))
    environment = PROJECT/'Assets/Textures/Hdr/snowy_field_2k.hdr'
    if scene_name == 'IntelSponza':
        write_json(output/'benchmark_config.json', {'version':1,'scene':'IntelSponzaDaylight.he',
        'mainView':{'width':3840,'height':2160,'resolutionScale':1.0,'dynamicResolution':False,
                    'upscaling':False,'frameGeneration':False,'vrs':False},
        'materialTextureMaximumDimension':conversion['textureMaximumDimension'],
        'camera':'Original PhysCamera001; unchanged pose and FOV; near=0.1, far=100 metres',
        'originalCameras':conversion['cameras'],
        'directionalLight':{'linearColor':[1.0,0.95,0.85],'intensity':3.0,'eulerRadians':[-2.55,0.75,0.2],
            'castShadows':True,'cascades':4,'mapSizePerCascade':2048,'shadowDistanceMetres':80.0,
            'pcfRadius':1.5},
        'environment':{'asset':relative_asset(environment),'sha256':digest(environment),
            'exposureEV100':1.6896598793878495,'exposureCompensation':0.0,'iblIntensity':1.0,'source':'Benchmark environment'},
        'originalLightCount':len(conversion.get('extensions',{}).get('KHR_lights_punctual',{}).get('lights',[])),
        'originalLightPolicy':'Zero-intensity source light nodes preserved; separate declared nonzero benchmark sun added',
        'measurementStatus':'Configuration only; runtime resolution, AA/effects, warmup, power state and performance must be recorded by capture/profiler'})
    shutil.copyfile(directory/'source_audit.json', output/'source_audit.json')
    shutil.copyfile(directory/'conversion.json', output/'conversion.json')
    license_source = arguments.source.resolve().parent / 'credits_license.txt'
    if license_source.is_file():
        shutil.copyfile(license_source, output/'credits_license.txt')
    print('import PASS: scene=' + str(output/(scene_name+'.he')))


def verify(arguments):
    output, directory = arguments.output.resolve(), arguments.prepared.resolve()
    audit, conversion = read_json(directory/'source_audit.json'), read_json(directory/'conversion.json')
    scene_name = conversion.get('sceneName','IntelSponza')
    mesh_prefix = conversion.get('meshPrefix','sponza')
    materials_directory = read_json(output/'import_progress.json').get('materialDirectory', 'Materials')
    scene = read_json(output/(scene_name+'.he'))
    expected = node_scene(conversion, output)
    if scene != expected:
        raise ValueError('Generated scene nodes, references, camera or transforms differ from source conversion')
    if scene_name == 'IntelSponza' and read_json(output/'IntelSponzaDaylight.he') != daylight_scene(conversion, output):
        raise ValueError('Daylight scene differs from its declared lighting/camera configuration')
    triangles = 0
    mesh_triangle_counts = {}
    source = read_json(arguments.source.resolve())
    source_buffers = [source_uri(arguments.source.resolve(), item['uri']).read_bytes() for item in source['buffers']]
    position_bounds = []
    for index in range(conversion['meshCount']):
        mesh_path = output/'Meshes'/(mesh_prefix+'_mesh_%03d.hmesh' % index)
        mesh = read_json(mesh_path)
        mesh_triangle_counts[relative_asset(mesh_path)] = sum(mesh['accessorVec'][primitive['indices']]['count']//3 for primitive in mesh['primitives'])
        cooked_document = {'accessors': [], 'bufferViews': []}
        for accessor in mesh['accessorVec']:
            converted = dict(accessor)
            converted['bufferView'] = converted.pop('bufferViewIndex')
            cooked_document['accessors'].append(converted)
        for view in mesh['bufferViewVec']:
            converted = dict(view)
            converted['buffer'] = converted.pop('bufferIndex')
            if not converted.get('byteStride'):
                converted.pop('byteStride', None)
            cooked_document['bufferViews'].append(converted)
        cooked_buffers = [(PROJECT/item['uri']).read_bytes() for item in mesh['bufferVec']]
        def bounds(document, buffers, primitives):
            low, high = [float('inf')]*3, [-float('inf')]*3
            for primitive in primitives:
                for point in accessor_values(document, buffers, primitive['attributes']['POSITION']):
                    for axis in range(3):
                        if not math.isfinite(point[axis]):
                            raise ValueError('Non-finite vertex position')
                        low[axis], high[axis] = min(low[axis], point[axis]), max(high[axis], point[axis])
            return low+high
        source_bounds = bounds(source, source_buffers, source['meshes'][index]['primitives'])
        cooked_bounds = bounds(cooked_document, cooked_buffers, mesh['primitives'])
        error = max(abs(a-b) for a,b in zip(source_bounds, cooked_bounds))
        if error > 0.0002:
            raise ValueError('Imported mesh local bounds changed; mesh=%d error=%f' % (index,error))
        position_bounds.append({'mesh': index, 'source': source_bounds, 'imported': cooked_bounds, 'maxError': error})
        for primitive in mesh['primitives']:
            triangles += mesh['accessorVec'][primitive['indices']]['count']//3
            for attribute in ['POSITION', 'NORMAL', 'TANGENT', 'BITANGENT', 'TEXCOORD_0']:
                if attribute not in primitive['attributes']:
                    raise ValueError('Missing imported geometry attribute: ' + attribute)
            if not (PROJECT/primitive['materialResPath']).is_file():
                raise ValueError('Missing shared material')
        for buffer in mesh['bufferVec']:
            path = PROJECT/buffer['uri']
            if not path.is_file() or path.stat().st_size != buffer['byteLength']:
                raise ValueError('Invalid mesh binary reference: ' + str(path))
    scene_triangles = sum(mesh_triangle_counts[entity['MeshComponent']['meshResPath']] for entity in scene.values() if 'MeshComponent' in entity)
    if triangles != audit.get('uniqueTriangles',audit['triangles']) or scene_triangles != audit['triangles']:
        raise ValueError('Unique or instanced triangle count changed: unique=%d instanced=%d' % (triangles,scene_triangles))
    material_reports = []
    slot_names = {'baseColorTexture': 'g_AlbedoMap', 'metallicRoughnessTexture': 'g_MetallicRoughnessMap',
                  'normalTexture': 'g_NormalMap', 'occlusionTexture': 'g_AoMap', 'emissiveTexture': 'g_EmissiveMap'}
    for material in audit['materials']:
        path = output/materials_directory/(mesh_prefix+'_material_%03d.hmat' % material['index'])
        imported = read_json(path)
        if imported['BlendMode'] != {'OPAQUE':'Opaque','MASK':'Mask','BLEND':'Transparent'}[material['alphaMode']]:
            raise ValueError('Alpha mode mismatch: '+str(path))
        if imported['CullMode'] != ('None' if material['doubleSided'] else 'Back'):
            raise ValueError('Double-sided mismatch: '+str(path))
        source_material = material['source']
        source_pbr = source_material.get('pbrMetallicRoughness', {})
        emission_strength = source_material.get('extensions', {}).get('KHR_materials_emissive_strength', {}).get('emissiveStrength', 1.0)
        expected_parameters = {
            'g_BaseColorFactor': source_pbr.get('baseColorFactor', [1,1,1,1]),
            'g_MetallicFactor': source_pbr.get('metallicFactor', 1.0),
            'g_RoughnessFactor': source_pbr.get('roughnessFactor', 1.0),
            'g_NormalStrength': source_material.get('normalTexture', {}).get('scale', 1.0),
            'g_AoStrength': source_material.get('occlusionTexture', {}).get('strength', 1.0),
            'g_AlphaCutoff': source_material.get('alphaCutoff', 0.5),
            'g_EmissiveFactor': [v*emission_strength for v in source_material.get('emissiveFactor', [0,0,0])]+[1.0]}
        for name, value in expected_parameters.items():
            actual = imported['Parameters'][name]['Value']
            left, right = (actual,value) if isinstance(value,list) else ([actual],[value])
            if len(left) != len(right) or any(abs(a-b)>2e-5 for a,b in zip(left,right)):
                raise ValueError('Material factor mismatch: '+str(path)+' '+name)
        for slot in material['slots']:
            actual = imported['Textures'][slot_names[slot['slot']]]
            texture = PROJECT/actual['Path']
            expected_texture = directory/conversion['textures'][slot['image']]['output']
            if actual['ColorSpace'] != slot['colorSpace'] or not texture.is_file() or digest(texture) != digest(expected_texture):
                raise ValueError('Texture data/color-space mapping mismatch: '+str(path)+' '+slot['slot'])
        material_reports.append({'material': material['index'], 'pass': True})
    report = {'status': 'PASS', 'evidenceType': 'unit-test', 'validation': 'source-to-import structural and resource checks; not rendered visual QA',
        'nodes': len(scene), 'meshInstances': sum('MeshComponent' in value for value in scene.values()),
        'sharedMeshResources': conversion['meshCount'], 'triangles': scene_triangles, 'uniqueTriangles':triangles, 'materials': material_reports,
        'textureMaximumDimension': conversion['textureMaximumDimension'],
        'rgba8MipTextureBytesEstimate': conversion['outputTextureBytesRGBA8WithMips'],
        'meshPositionBounds': position_bounds,
        'limitations': ['No runtime render/performance verification by this tool.',
                       'Light/camera conversion is recorded in conversion.json; benchmark effects and exposure require runtime validation.',
                       'Only texture UV0 and white-only vertex colors are accepted; unconsumed source attributes remain in the original asset.']}
    write_json(output/'validation.json', report)
    print(json.dumps({k:v for k,v in report.items() if k not in {'materials','limitations','meshPositionBounds'}}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['prepare','import','verify'])
    parser.add_argument('--source', type=Path, default=DEFAULT_SOURCE)
    parser.add_argument('--prepared', type=Path, default=SOURCE_ROOT/'prepared'/'textures2048')
    parser.add_argument('--output', type=Path, default=PROJECT/'Assets'/'RenderingBenchmarks'/'IntelSponza2048')
    parser.add_argument('--texture-size', type=int, default=2048)
    parser.add_argument('--scene-name', default='IntelSponza')
    parser.add_argument('--mesh-prefix', default='sponza')
    parser.add_argument('--reuse-prepared-textures', action='store_true', help='Resume using already generated, dimension-checked textures')
    parser.add_argument('--refresh-materials', action='store_true', help='Import materials with a new executable into a versioned directory and relink owned meshes')
    parser.add_argument('--importer', type=Path)
    arguments = parser.parse_args()
    if arguments.texture_size < 1 or arguments.texture_size > 16384:
        parser.error('--texture-size must be between 1 and 16384')
    if arguments.action == 'import' and (not arguments.importer or not arguments.importer.is_file()):
        parser.error('--importer must name a built HAssetImporter executable')
    {'prepare':prepare, 'import':import_scene, 'verify':verify}[arguments.action](arguments)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError, subprocess.SubprocessError) as error:
        print('FAIL: ' + str(error), file=sys.stderr)
        sys.exit(1)
