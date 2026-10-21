"""Run only in Blender background mode, via bake.py. No project-specific rules."""
import copy
import json
import math
import re
from pathlib import Path
import sys
import uuid

import bpy
from mathutils import Euler, Matrix, Vector
import numpy as np

sys.path.insert(0,str(Path(__file__).parent))
from bake import asset, digest, read
from lighting_math import FACES, blender_uv_rows, cube_irradiance_sh, cube_to_panorama, write_dds, write_hdr

request=read(Path(sys.argv[sys.argv.index('--')+1]))
root=Path(request['project']).resolve()
stage=Path(request['stage'])
output=request['output']
inputs={}
engine_inputs={}
engine_root=Path(__file__).resolve().parents[2]
warnings=set()
json_cache={}
buffer_cache={}


def input_path(relative):
    path=asset(root,relative)
    if path.is_file():
        if relative not in inputs: inputs[relative]=digest(path)
    else:
        path=asset(engine_root,'Engine/Content/'+relative)
        if relative not in engine_inputs: engine_inputs[relative]=digest(path)
    return path


def load(relative):
    if relative not in json_cache: json_cache[relative]=read(input_path(relative))
    return json_cache[relative]


config=load(request['config'])
scene_data=load(config['scene'])
if config.get('version',1)!=1: raise ValueError('Unsupported bake configuration')
samples=int(config.get('samples',64))
size=int(config.get('probeResolution',64))
if not 1<=samples<=4096 or not 8<=size<=512: raise ValueError('Invalid bake quality')
if len(config.get('probes',[]))>16: raise ValueError('First-generation volume budget is 16')
probe_ids=[p['id'] for p in config.get('probes',[])]
target_ids=[p['entity'] for p in config.get('lightmaps',[])]
if len(set(probe_ids))!=len(probe_ids) or len(set(target_ids))!=len(target_ids):
    raise ValueError('Duplicate probe or lightmap target')
if not 1<=int(config.get('bounces',4))<=16: raise ValueError('Bake bounces must be 1..16')
for probe in config.get('probes',[]):
    fade=float(probe.get('blendDistance',1))
    if not math.isfinite(fade) or fade<=0: raise ValueError('Blend distance must be finite and positive')
conversion=Matrix(((1,0,0,0),(0,0,-1,0),(0,1,0,0),(0,0,0,1)))
bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete(use_global=False)
scene=bpy.context.scene
scene.render.engine='CYCLES'
scene.cycles.samples=samples
scene.cycles.seed=int(config.get('seed',1729))
scene.cycles.use_animated_seed=False
scene.cycles.max_bounces=int(config.get('bounces',4))
scene.cycles.diffuse_bounces=scene.cycles.max_bounces
scene.cycles.use_denoising=True
scene.render.image_settings.file_format='OPEN_EXR'
scene.render.image_settings.color_depth='32'
scene.render.film_transparent=False
scene.view_settings.view_transform='Standard'
scene.view_settings.exposure=0
scene.view_settings.gamma=1
device=config.get('device','CPU')
if device!='CPU':
    preferences=bpy.context.preferences.addons['cycles'].preferences
    preferences.compute_device_type=device
    preferences.get_devices()
    available=[d for d in preferences.devices if d.type==device]
    if not available: raise ValueError('Requested Cycles device unavailable: '+device)
    for d in preferences.devices: d.use=d in available
    scene.cycles.device='GPU'

world_matrices={}


def world_matrix(identifier,visited=None):
    if identifier in world_matrices: return world_matrices[identifier]
    visited=set() if visited is None else visited
    if identifier in visited: raise ValueError('Transform hierarchy cycle')
    visited.add(identifier)
    t=scene_data[identifier].get('TransformComponent',{})
    matrix=Matrix.Translation(Vector(t.get('Translation',[0,0,0]))) @ Euler(t.get('Rotation',[0,0,0]),'XYZ').to_matrix().to_4x4() @ Matrix.Diagonal((*t.get('Scale',[1,1,1]),1))
    parent=t.get('ParentUuid','')
    if parent and parent in scene_data: matrix=world_matrix(parent,visited)@matrix
    world_matrices[identifier]=matrix
    return matrix


def dynamic(identifier,visited=None):
    visited=set() if visited is None else visited
    if identifier in visited: raise ValueError('Hierarchy cycle')
    visited.add(identifier)
    entity=scene_data[identifier]
    if any(k in entity for k in ('AnimatorComponent','CharacterMovementComponent','BoneAttachmentComponent')): return True
    if entity.get('Rigidbody3DComponent',{}).get('Type',0) not in (0,'Static'): return True
    if identifier in config.get('excludeEntities',[]): return True
    tag=entity.get('TagComponent',{}).get('Tag','')
    if any(tag.startswith(prefix) for prefix in config.get('excludeTagPrefixes',[])): return True
    parent=entity.get('TransformComponent',{}).get('ParentUuid','')
    return bool(parent and parent in scene_data and dynamic(parent,visited))


def accessor(mesh,index):
    a=mesh['accessorVec'][index]; view=mesh['bufferViewVec'][a['bufferViewIndex']]
    buffer=mesh['bufferVec'][view['bufferIndex']]
    if buffer['uri'] not in buffer_cache: buffer_cache[buffer['uri']]=input_path(buffer['uri']).read_bytes()
    raw=buffer_cache[buffer['uri']]
    dtype={5126:'<f4',5125:'<u4',5123:'<u2',5121:'u1'}.get(a['componentType'])
    columns={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4}.get(a['type'])
    if not dtype or not columns or a.get('normalized'): raise ValueError('Unsupported accessor')
    width=np.dtype(dtype).itemsize
    offset=view.get('byteOffset',0)+a.get('byteOffset',0)
    stride=view.get('byteStride',0) or columns*width
    count=a['count']
    if count<1 or stride<columns*width or offset+(count-1)*stride+columns*width>len(raw): raise ValueError('Accessor out of bounds')
    result=np.ndarray((count,columns),dtype=dtype,buffer=raw,offset=offset,strides=(stride,width)).copy()
    if not np.isfinite(result).all(): raise ValueError('Non-finite mesh')
    return result


material_cache={}
image_cache={}
shader_seen=set()


def track_shader(path):
    path=path.resolve()
    if path in shader_seen: return
    shader_seen.add(path)
    if path.is_relative_to(root): inputs[path.relative_to(root).as_posix()]=digest(path)
    elif path.is_relative_to(engine_root/'Engine/Content'):
        engine_inputs[path.relative_to(engine_root/'Engine/Content').as_posix()]=digest(path)
    else: raise ValueError('Shader include escapes content roots')
    for include in re.findall(r'^\s*#\s*include\s+"([^"]+)"',path.read_text(encoding='utf-8-sig'),re.MULTILINE):
        candidates=[path.parent/include,root/'Shaders'/include,engine_root/'Engine/Content/Shaders'/include]
        dependency=next((candidate for candidate in candidates if candidate.is_file()),None)
        if dependency is None: raise ValueError('Missing shader dependency: '+include)
        track_shader(dependency)


def material(relative):
    if relative in material_cache: return material_cache[relative]
    data=load(relative) if relative else {}
    params=data.get('Parameters',{})
    value=lambda name,default: params.get(name,{}).get('Value',default)
    mat=bpy.data.materials.new(relative or 'DefaultDiffuse'); mat.use_nodes=True
    nodes=mat.node_tree.nodes; links=mat.node_tree.links
    shader=nodes.get('Principled BSDF')
    shader.inputs['Base Color'].default_value=value('g_BaseColorFactor',[1,1,1,1])
    shader.inputs['Metallic'].default_value=value('g_MetallicFactor',0)
    shader.inputs['Roughness'].default_value=value('g_RoughnessFactor',.5)
    shader.inputs['Emission Color'].default_value=value('g_EmissiveFactor',[0,0,0,1])
    shader.inputs['Emission Strength'].default_value=1
    if data.get('BlendMode')=='Transparent':
        warnings.add('Transparent surface approximated with cutout alpha: '+relative)
    def math_node(operation, first, second):
        node=nodes.new('ShaderNodeMath'); node.operation=operation
        for i,operand in enumerate((first,second)):
            if isinstance(operand,(int,float,np.floating)): node.inputs[i].default_value=float(operand)
            else: links.new(operand,node.inputs[i])
        return node.outputs[0]

    def texture_node(key):
        texture=data.get('Textures',{}).get(key,{})
        path=texture.get('Path','')
        if not path: return None
        color=texture.get('ColorSpace','SRGB' if key in ('g_AlbedoMap','g_EmissiveMap') else 'Linear')
        image_key=(path,color)
        if image_key not in image_cache:
            image=bpy.data.images.load(str(input_path(path)),check_existing=False)
            image.colorspace_settings.name='sRGB' if color.lower()=='srgb' else 'Non-Color'
            image_cache[image_key]=image
        node=nodes.new('ShaderNodeTexImage'); node.image=image_cache[image_key]
        sampling=texture.get('Sampling',{})
        coordinate=int(sampling.get('texCoord',0))
        if coordinate not in (0,1): raise ValueError('Only UV0/UV1 texture sampling is supported')
        uv=nodes.new('ShaderNodeUVMap'); uv.uv_map='UV'+str(coordinate)
        split=nodes.new('ShaderNodeSeparateXYZ'); links.new(uv.outputs['UV'],split.inputs[0])
        combine=nodes.new('ShaderNodeCombineXYZ')
        wrap_s=sampling.get('wrapS',10497); wrap_t=sampling.get('wrapT',10497)
        for i,row in enumerate(blender_uv_rows(sampling)):
            transformed=math_node('ADD', math_node('ADD',math_node('MULTIPLY',split.outputs[0],row[0]),
                math_node('MULTIPLY',split.outputs[1],row[1])),row[2])
            wrap=(wrap_s,wrap_t)[i]
            if wrap==33648: transformed=math_node('PINGPONG',transformed,1)
            elif wrap==33071: transformed=math_node('MINIMUM',math_node('MAXIMUM',transformed,0),1)
            elif wrap!=10497: raise ValueError('Unsupported texture wrap mode')
            links.new(transformed,combine.inputs[i])
        links.new(combine.outputs[0],node.inputs['Vector'])
        node.extension='EXTEND' if wrap_s==wrap_t==33071 else 'REPEAT'
        node.interpolation='Closest' if sampling.get('magFilter',9729)==9728 else 'Linear'
        if wrap_s!=wrap_t or wrap_s==33648:
            warnings.add('Mixed/mirrored texture wrap filtering at seams approximated: '+relative+':'+key)
        return node

    # Match BasePBR's packed B/G channels, independent overrides and factors.
    packed=texture_node('g_MetallicRoughnessMap')
    channels=None
    if packed:
        channels=nodes.new('ShaderNodeSeparateColor'); channels.mode='RGB'
        links.new(packed.outputs['Color'],channels.inputs[0])
    for key,socket,index,factor in [('g_MetallicMap','Metallic',2,'g_MetallicFactor'),
                                   ('g_RoughnessMap','Roughness',1,'g_RoughnessFactor')]:
        result=math_node('MULTIPLY',channels.outputs[index] if channels else 1,
                         value(factor,0 if socket=='Metallic' else .5))
        override=texture_node(key)
        if override:
            split=nodes.new('ShaderNodeSeparateColor'); split.mode='RGB'
            links.new(override.outputs['Color'],split.inputs[0])
            result=math_node('MULTIPLY',result,split.outputs[0])
        result=math_node('MINIMUM',math_node('MAXIMUM',result,.04 if socket=='Roughness' else 0),1)
        links.new(result,shader.inputs[socket])
    ao_path=data.get('Textures',{}).get('g_AoMap',{}).get('Path','')
    if ao_path:
        input_path(ao_path)
        warnings.add('Material AO omitted from path-traced bounce reflectance: '+relative)
    for key,socket in [('g_AlbedoMap','Base Color'),('g_EmissiveMap','Emission Color'),('g_NormalMap','Normal')]:
        node=texture_node(key)
        if node is None: continue
        if key=='g_NormalMap':
            normal=nodes.new('ShaderNodeNormalMap')
            normal.uv_map='UV'+str(data.get('Textures',{}).get(key,{}).get('Sampling',{}).get('texCoord',0))
            normal.inputs['Strength'].default_value=value('g_NormalStrength',1)
            links.new(node.outputs['Color'],normal.inputs['Color']); links.new(normal.outputs['Normal'],shader.inputs[socket])
        else:
            factor=value('g_BaseColorFactor' if key=='g_AlbedoMap' else 'g_EmissiveFactor',[1,1,1,1])
            multiply=nodes.new('ShaderNodeMixRGB'); multiply.blend_type='MULTIPLY'; multiply.inputs[0].default_value=1
            multiply.inputs[2].default_value=factor
            links.new(node.outputs['Color'],multiply.inputs[1]); links.new(multiply.outputs[0],shader.inputs[socket])
        if key=='g_AlbedoMap' and (value('g_AlphaClip',0)>=.5 or data.get('BlendMode','Opaque')!='Opaque'):
            alpha=nodes.new('ShaderNodeMath'); alpha.operation='GREATER_THAN'
            alpha.inputs[1].default_value=value('g_AlphaCutoff',data.get('AlphaCutoff',.5))
            links.new(math_node('MULTIPLY',node.outputs['Alpha'],value('g_BaseColorFactor',[1,1,1,1])[3]),alpha.inputs[0])
            links.new(alpha.outputs[0],shader.inputs['Alpha'])
    shader_path=data.get('ShaderPath','')
    if shader_path: track_shader(input_path(shader_path))
    if shader_path and 'BasePBR' not in shader_path:
        warnings.add('Common PBR approximation for authored shader: '+shader_path)
    material_cache[relative]=mat
    return mat


objects={}; mesh_cache={}; skipped=0
targets={item['entity']:item for item in config.get('lightmaps',[])}
estimated_lightmap_bytes=sum(int(item.get('resolution',256))**2*8*4//3 for item in targets.values())
if estimated_lightmap_bytes>int(config.get('maxLightmapBytes',256*1024*1024)):
    raise ValueError('Requested lightmaps exceed the explicit offline memory budget')
for identifier,entity in scene_data.items():
    mesh_component=entity.get('MeshComponent')
    if not mesh_component or dynamic(identifier): continue
    relative=mesh_component.get('Path','')
    if not relative or relative=='None': continue
    mesh=load(relative)
    if mesh.get('skeletonPath') or any('JOINTS_0' in p['attributes'] or p.get('targets') for p in mesh['primitives']):
        skipped+=1; continue
    overrides=mesh_component.get('MaterialOverrides',[])
    key=(relative,tuple(overrides))
    if key not in mesh_cache:
        vertices=[]; triangles=[]; uv0=[]; uv1=[]; normals=[]; slots=[]; materials=[]
        for index,primitive in enumerate(mesh['primitives']):
            if primitive.get('mode',4)!=4: raise ValueError('Only triangle meshes can bake')
            attrs=primitive['attributes']; positions=accessor(mesh,attrs['POSITION'])
            indices=accessor(mesh,primitive['indices']).reshape(-1,3)
            if indices.max()>=len(positions): raise ValueError('Invalid triangle index')
            offset=len(vertices); vertices.extend(positions.tolist()); triangles.extend((indices+offset).tolist())
            uv0.extend(accessor(mesh,attrs['TEXCOORD_0']).tolist() if 'TEXCOORD_0' in attrs else [[0,0]]*len(positions))
            uv1.extend(accessor(mesh,attrs['TEXCOORD_1']).tolist() if 'TEXCOORD_1' in attrs else [[0,0]]*len(positions))
            normals.extend(accessor(mesh,attrs['NORMAL']).tolist() if 'NORMAL' in attrs else [[0,1,0]]*len(positions))
            slots.extend([index]*len(indices))
            path=overrides[index] if index<len(overrides) and overrides[index] else primitive.get('materialResPath','')
            materials.append(material(path))
        geometry=bpy.data.meshes.new(relative)
        geometry.from_pydata(vertices,[],triangles); geometry.update()
        for name,coords in [('UV0',uv0),('UV1',uv1)]:
            layer=geometry.uv_layers.new(name=name)
            values=np.array(coords,dtype=np.float32)[np.array([loop.vertex_index for loop in geometry.loops])]
            values[:,1]=1-values[:,1]
            layer.data.foreach_set('uv',values.ravel())
        for mat in materials: geometry.materials.append(mat)
        geometry.polygons.foreach_set('material_index',slots)
        geometry.polygons.foreach_set('use_smooth',[True]*len(slots))
        geometry.normals_split_custom_set_from_vertices(normals)
        mesh_cache[key]=geometry
    geometry=mesh_cache[key]
    if identifier in targets: geometry=geometry.copy()
    obj=bpy.data.objects.new(identifier,geometry); scene.collection.objects.link(obj)
    obj.matrix_world=conversion@world_matrix(identifier)
    objects[identifier]=obj
print('HE bake scene:',len(objects),'static instances;',len(mesh_cache),'unique meshes;',skipped,'skinned skipped',flush=True)

lights=[]
for identifier,entity in scene_data.items():
    for component,kind in [('DirectionalLightComponent','SUN'),('PointLightComponent','POINT'),('SpotLightComponent','SPOT')]:
        if component not in entity: continue
        source=entity[component]; lamp=bpy.data.lights.new(identifier,kind)
        lamp.color=source.get('Color',source.get('LightColor',[1,1,1]))
        intensity=max(float(source.get('Intensity',0)),0)
        if kind=='SUN': lamp.energy=intensity
        else:
            units=source.get('IntensityUnits',1)
            solid=4*math.pi if kind=='POINT' else 2*math.pi*(1-math.cos(math.radians(source.get('OuterConeAngle',30))))
            candela=intensity/625 if units==0 else intensity/solid if units==2 else intensity
            lamp.energy=candela*4*math.pi
            lamp.shadow_soft_size=0.02
            if kind=='SPOT':
                lamp.spot_size=math.radians(source.get('OuterConeAngle',30)*2)
                lamp.spot_blend=1-source.get('InnerConeAngle',20)/max(source.get('OuterConeAngle',30),.1)
            warnings.add('Local light range/spot attenuation approximated by Cycles')
        obj=bpy.data.objects.new(identifier+'_light',lamp); scene.collection.objects.link(obj)
        matrix=world_matrix(identifier)
        direction=matrix.to_3x3()@Vector(source.get('LocalDirection',[0,1,0]) if kind=='SUN' else [0,0,-1])
        obj.location=conversion@matrix.translation
        obj.rotation_euler=(conversion.to_3x3()@direction).to_track_quat('-Z','Y').to_euler()
        lights.append(obj)

world=bpy.data.worlds.new('BakeWorld'); world.use_nodes=True; scene.world=world
background=world.node_tree.nodes.get('Background'); background.inputs['Color'].default_value=(0,0,0,1)
for entity in scene_data.values():
    environment=entity.get('EnvironmentLightComponent',{})
    path=environment.get('HdrTexturePath','')
    if not path: continue
    node=world.node_tree.nodes.new('ShaderNodeTexEnvironment'); node.image=bpy.data.images.load(str(input_path(path)))
    # World Normal is opposite the outgoing ray. This sign convention is
    # verified end-to-end with the RGB-axis environment fixture, not inferred
    # from camera Euler angles or display-transformed screenshots.
    texcoord=world.node_tree.nodes.new('ShaderNodeTexCoord')
    mapping=world.node_tree.nodes.new('ShaderNodeVectorMath'); mapping.operation='MULTIPLY'
    mapping.inputs[1].default_value=(-1,-1,-1)
    world.node_tree.links.new(texcoord.outputs['Normal'],mapping.inputs[0])
    world.node_tree.links.new(mapping.outputs[0],node.inputs['Vector'])
    world.node_tree.links.new(node.outputs['Color'],background.inputs['Color'])
    background.inputs['Strength'].default_value=max(environment.get('IBLIntensity',1),0)
    break

if warnings and not config.get('allowMaterialApproximation',False):
    raise ValueError('Bake requires explicit approximation policy:\n'+'\n'.join(sorted(warnings)))

baked=copy.deepcopy(scene_data)
camera_data=bpy.data.cameras.new('LightingCapture'); camera_data.type='PERSP'; camera_data.angle=math.pi/2
camera_data.clip_start=.03; camera_data.clip_end=float(config.get('farDistance',1000))
camera=bpy.data.objects.new('LightingCapture',camera_data); scene.collection.objects.link(camera); scene.camera=camera
scene.render.resolution_x=size; scene.render.resolution_y=size; scene.render.resolution_percentage=100
for probe in config.get('probes',[]):
    name=probe['id']
    if not name.replace('_','').replace('-','').isalnum(): raise ValueError('Invalid probe ID')
    position=np.array(probe['position'],dtype=float)
    lo=np.array(probe['boundsMin'],dtype=float); hi=np.array(probe['boundsMax'],dtype=float)
    if not np.isfinite([position,lo,hi]).all() or not np.all(hi>lo) or np.any(position<=lo) or np.any(position>=hi):
        raise ValueError('Capture must lie inside finite positive bounds')
    faces=[]
    camera.location=conversion@Vector(position)
    for index,(forward,right,up) in enumerate(FACES):
        rotation=Matrix((Vector(right),Vector(up),-Vector(forward))).transposed()
        camera.rotation_euler=(conversion.to_3x3()@rotation).to_euler()
        filepath=stage/f'{name}-face-{index}.exr'; scene.render.filepath=str(filepath)
        bpy.ops.render.render(write_still=True)
        image=bpy.data.images.load(str(filepath),check_existing=False)
        pixels=np.array(image.pixels[:],dtype=np.float32).reshape(size,size,4)[...,:3]
        if not np.isfinite(pixels).all(): raise ValueError('Non-finite probe capture')
        faces.append(pixels); bpy.data.images.remove(image); filepath.unlink()
    faces=np.array(faces)
    coefficients=cube_irradiance_sh(faces)
    hdr_path=f'{output}/{name}.hdr'
    write_hdr(stage/f'{name}.hdr',cube_to_panorama(faces,size*4))
    identifier=str(uuid.uuid5(uuid.NAMESPACE_URL,'hengine:baked-lighting:'+config['scene']+':'+name))
    entity={'TagComponent':{'Tag':'BakedLighting_'+name},
        'TransformComponent':{'Translation':((lo+hi)*.5).tolist(),'Rotation':[0,0,0],'Scale':[1,1,1]},
        'BakedIrradianceVolumeComponent':{'BoundsMin':lo.tolist(),'BoundsMax':hi.tolist(),
            'IrradianceSH':coefficients.tolist(),'BlendDistance':probe.get('blendDistance',1),
            'Intensity':1,'bEnabled':True}}
    if probe.get('reflection',True):
        entity['ReflectionProbeComponent']={'HdrTexturePath':hdr_path,'BoxHalfExtents':((hi-lo)*.5).tolist(),
            'CaptureOffset':(position-(lo+hi)*.5).tolist(),'BlendDistance':probe.get('blendDistance',1),
            'Intensity':1,'bEnabled':True,'bAffectDiffuse':False}
    baked[identifier]=entity
    print('HE baked probe:',name,flush=True)

def unwrap_derivative(identifier,obj,source,resolution,padding):
    obj.data.uv_layers.active=obj.data.uv_layers['UV1']
    bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(angle_limit=1.151917306, island_margin=padding/resolution)
    bpy.ops.object.mode_set(mode='OBJECT')
    uv=np.array([item.uv[:] for item in obj.data.uv_layers['UV1'].data],dtype='<f4')
    uv[:,1]=1-uv[:,1]
    derived=copy.deepcopy(source); raw=bytearray(); views=[]; accessors=[]
    def append(values,kind,shape):
        values=np.asarray(values,dtype={5126:'<f4',5125:'<u4',5123:'<u2',5121:'u1'}[kind])
        offset=len(raw); raw.extend(values.tobytes())
        views.append({'bufferIndex':0,'byteOffset':offset,'byteLength':len(raw)-offset})
        accessors.append({'bufferViewIndex':len(views)-1,'byteOffset':0,'count':len(values),
            'componentType':kind,'type':shape,'normalized':False})
        return len(accessors)-1
    loop=0
    for old,new in zip(source['primitives'],derived['primitives']):
        indices=accessor(source,old['indices']).ravel()
        new['attributes']={}
        for semantic,index in old['attributes'].items():
            if semantic=='TEXCOORD_1': continue
            values=accessor(source,index)[indices]
            original=source['accessorVec'][index]
            new['attributes'][semantic]=append(values,original['componentType'],original['type'])
        new['attributes']['TEXCOORD_1']=append(uv[loop:loop+len(indices)],5126,'VEC2')
        new['indices']=append(np.arange(len(indices),dtype='<u4'),5125,'SCALAR')
        new.pop('lods',None); loop+=len(indices)
    name='mesh-'+identifier
    (stage/(name+'.bin')).write_bytes(raw)
    derived['bufferVec']=[{'uri':output+'/'+name+'.bin','byteLength':len(raw)}]
    derived['bufferViewVec']=views; derived['accessorVec']=accessors
    (stage/(name+'.hmesh')).write_text(json.dumps(derived,indent=2)+'\n',encoding='utf-8')
    baked[identifier]['MeshComponent']['Path']=output+'/'+name+'.hmesh'


def validate_uv(obj,resolution):
    """Reject interior overlap and out-of-bounds charts before producing a map."""
    uv=np.array([item.uv[:] for item in obj.data.uv_layers['UV1'].data])
    if not np.isfinite(uv).all() or np.any(uv<0) or np.any(uv>1): raise ValueError('UV1 outside [0,1]')
    occupied=np.zeros((resolution,resolution),dtype=bool)
    covered=0
    for polygon in obj.data.polygons:
        triangle=uv[list(polygon.loop_indices)]*resolution
        if len(triangle)!=3: raise ValueError('Non-triangular bake mesh')
        a,b,c=triangle
        determinant=np.cross(b-a,c-a).item()
        if abs(determinant)<1e-8: raise ValueError('Degenerate UV1 triangle; use generateUV1')
        minimum=np.maximum(np.floor(triangle.min(axis=0)).astype(int),0)
        maximum=np.minimum(np.ceil(triangle.max(axis=0)).astype(int),resolution-1)
        xs,ys=np.meshgrid(np.arange(minimum[0],maximum[0]+1)+.5,np.arange(minimum[1],maximum[1]+1)+.5)
        dx=xs-a[0]; dy=ys-a[1]
        u=(dx*(c-a)[1]-dy*(c-a)[0])/determinant
        v=((b-a)[0]*dy-(b-a)[1]*dx)/determinant
        mask=(u>1e-6)&(v>1e-6)&(u+v<1-1e-6)
        region=occupied[minimum[1]:maximum[1]+1,minimum[0]:maximum[0]+1]
        if np.any(region&mask): raise ValueError('Overlapping UV1 charts; use generateUV1')
        region|=mask; covered+=mask.sum()
    if not covered: raise ValueError('No lightmap texel coverage')


for identifier,target in targets.items():
    if identifier not in objects: raise ValueError('Lightmap target missing or dynamic: '+identifier)
    obj=objects[identifier]
    mesh=load(scene_data[identifier]['MeshComponent']['Path'])
    if not target.get('generateUV1',False) and any('TEXCOORD_1' not in p['attributes'] for p in mesh['primitives']):
        raise ValueError('Lightmap target requires authored nonoverlapping UV1: '+identifier)
    resolution=int(target.get('resolution',256))
    if resolution<16 or resolution>4096 or resolution&(resolution-1): raise ValueError('Invalid lightmap resolution')
    bpy.ops.object.select_all(action='DESELECT'); obj.select_set(True); bpy.context.view_layer.objects.active=obj
    if target.get('generateUV1',False):
        unwrap_derivative(identifier,obj,mesh,resolution,int(target.get('padding',8)))
    validate_uv(obj,resolution)
    obj.data.uv_layers.active=obj.data.uv_layers['UV1']
    image=bpy.data.images.new('BakeTarget',width=resolution,height=resolution,float_buffer=True)
    # Copy materials only for this receiver; other instances keep the same bounce
    # reflectance. The bake excludes receiver albedo via use_pass_color=False.
    for slot in obj.material_slots:
        slot.material=slot.material.copy()
        node=slot.material.node_tree.nodes.new('ShaderNodeTexImage'); node.image=image
        slot.material.node_tree.nodes.active=node
    scene.render.bake.use_pass_color=False
    scene.render.bake.margin=int(target.get('padding',8))
    scene.render.bake.use_clear=True
    scene.render.bake.use_pass_direct=False; scene.render.bake.use_pass_indirect=True
    bpy.ops.object.bake(type='DIFFUSE')
    indirect=np.array(image.pixels[:],dtype=np.float32).reshape(resolution,resolution,4)[...,:3].copy()
    for light in lights: light.hide_render=True
    scene.render.bake.use_pass_direct=True; scene.render.bake.use_pass_indirect=False
    bpy.ops.object.bake(type='DIFFUSE')
    sky=np.array(image.pixels[:],dtype=np.float32).reshape(resolution,resolution,4)[...,:3]
    for light in lights: light.hide_render=False
    name='lightmap-'+identifier
    write_dds(stage/(name+'.dds'),(indirect+sky)[::-1])
    component=baked[identifier]['MeshComponent']
    component.update({'StaticLightmapPath':output+'/'+name+'.dds','StaticLightmapEncoding':1,
        'StaticLightmapBlend':1,'StaticLightmapUvScale':[1,1],'StaticLightmapUvBias':[0,0]})
    bpy.data.images.remove(image)
    print('HE baked lightmap:',identifier,flush=True)

baked_name='Scene.he'
(stage/baked_name).write_text(json.dumps(baked,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
files=[p for p in stage.iterdir() if p.suffix in ('.hdr','.dds','.he','.hmesh','.bin')]
if sum(p.stat().st_size for p in files)>int(config.get('maxOutputBytes',512*1024*1024)):
    raise ValueError('Bake exceeds maxOutputBytes; reduce density/resolution or split regions')
manifest={'schema':'hengine.lighting-bake','version':1,'scene':config['scene'],
    'bakedScene':output+'/'+baked_name,'inputs':inputs,'engineInputs':engine_inputs,
    'outputs':{output+'/'+p.name:digest(p) for p in files},
    'tools':{name:digest(Path(__file__).with_name(name)) for name in ('bake.py','cycles_bake.py','lighting_math.py')},
    'solver':{'name':'Cycles','version':bpy.app.version_string,'samples':samples,
        'bounces':scene.cycles.max_bounces,'device':device,'seed':scene.cycles.seed},
    'statistics':{'staticInstances':len(objects),'uniqueMeshes':len(mesh_cache),
        'probes':len(config.get('probes',[])),'lightmaps':len(targets),
        'outputBytes':sum(p.stat().st_size for p in files)},
    'limitations':sorted(warnings)}
(stage/'manifest.json').write_text(json.dumps(manifest,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
print('HE lighting bake completed',flush=True)
