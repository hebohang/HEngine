"""Isolated 16-influence importer/GPU fixture; generation never launches the engine.

--importer runs an already-built importer, verifies native groups numerically,
then writes PBR/Skin scenes. --compare compares a moved capture to its same-
material rigid reference (Pillow required); it does not establish shadow/motion QA.
"""
import argparse
import copy
import json
from pathlib import Path
import struct
import subprocess
import uuid


def save(path, value):
    path.write_text(json.dumps(value, indent=2), encoding='utf-8')


def source_glb(path):
    data = bytearray()
    views, accessors = [], []
    def stream(values, fmt, kind, count, component=5126):
        data.extend(b'\0' * (-len(data) % 4))
        offset = len(data)
        data.extend(struct.pack('<' + fmt * len(values), *values))
        views.append(dict(buffer=0, byteOffset=offset, byteLength=len(data)-offset))
        accessors.append(dict(bufferView=len(views)-1, componentType=component, count=count, type=kind))
        return len(accessors)-1
    positions, normals, indices = [], [], []
    for normal, u, v in [((1,0,0),(0,1,0),(0,0,1)),((-1,0,0),(0,0,1),(0,1,0)),
                          ((0,1,0),(0,0,1),(1,0,0)),((0,-1,0),(1,0,0),(0,0,1)),
                          ((0,0,1),(1,0,0),(0,1,0)),((0,0,-1),(0,1,0),(1,0,0))]:
        base = len(positions)//3
        for a,b in [(-1,-1),(1,-1),(1,1),(-1,1)]:
            positions.extend((normal[i]+a*u[i]+b*v[i])*.4 for i in range(3))
            normals.extend(normal)
        indices.extend(base+i for i in (0,1,2,0,2,3))
    attrs = dict(POSITION=stream(positions,'f','VEC3',24), NORMAL=stream(normals,'f','VEC3',24))
    accessors[attrs['POSITION']].update(min=[-.4,-.4,-.4],max=[.4,.4,.4])
    for group in range(4):
        attrs['JOINTS_'+str(group)] = stream(list(range(group*4,group*4+4))*24,'H','VEC4',24,5123)
        attrs['WEIGHTS_'+str(group)] = stream([1/16]*96,'f','VEC4',24)
    index = stream(indices,'H','SCALAR',36,5123)
    ibm = stream([1 if row==col else 0 for _ in range(16) for col in range(4) for row in range(4)],'f','MAT4',16)
    times = stream([0,2],'f','SCALAR',2)
    accessors[times].update(min=[0],max=[2])
    animations=[]
    for name,joint in [('Neutral',0),('Slot5',4),('Slot9',8),('Slot16',15),('Motion16',15)]:
        x=0 if name=='Neutral' else 8
        values=[0,0,0,x,0,0] if name=='Motion16' else [x,0,0,x,0,0]
        translations=stream(values,'f','VEC3',2)
        animations.append(dict(name=name,samplers=[dict(input=times,output=translations,interpolation='LINEAR')],
            channels=[dict(sampler=0,target=dict(node=joint,path='translation'))]))
    gltf=dict(asset=dict(version='2.0'),scene=0,scenes=[dict(nodes=list(range(17)))],
        nodes=[dict(name='Joint%02d'%i) for i in range(16)]+[dict(name='InfluenceCube',mesh=0,skin=0)],
        skins=[dict(joints=list(range(16)),inverseBindMatrices=ibm)],
        meshes=[dict(name='InfluenceCube',primitives=[dict(attributes=attrs,indices=index,material=0)])],
        materials=[dict(name='FixtureBlue',pbrMetallicRoughness=dict(baseColorFactor=[.025,.1,.8,1],metallicFactor=0,roughnessFactor=.65))],
        animations=animations,buffers=[dict(byteLength=len(data))],bufferViews=views,accessors=accessors)
    encoded=json.dumps(gltf).encode('utf-8'); encoded+=b' '*(-len(encoded)%4);data+=b'\0'*(-len(data)%4)
    path.write_bytes(struct.pack('<III',0x46546c67,2,28+len(encoded)+len(data))+struct.pack('<II',len(encoded),0x4e4f534a)+encoded+struct.pack('<II',len(data),0x004e4942)+data)


def imported(project, folder, importer):
    native=folder/'Native';native.mkdir(exist_ok=True)
    with (folder/'import.log').open('wb') as log:
        result=subprocess.run([str(importer.resolve()),str(project),str(folder/'InfluenceCube.glb'),str(native)],stdout=log,stderr=subprocess.STDOUT,timeout=180)
    if result.returncode: raise RuntimeError('Importer failed; inspect import.log')
    mesh_path=native/'InfluenceCube.hmesh'
    mesh=json.loads(mesh_path.read_text(encoding='utf-8-sig'))
    def values(index):
        acc=mesh['accessorVec'][index];view=mesh['bufferViewVec'][acc['bufferViewIndex']]
        data=(project/mesh['bufferVec'][view['bufferIndex']]['uri']).read_bytes()
        return struct.unpack_from('<'+'f'*(acc['count']*4),data,view['byteOffset']+acc['byteOffset'])
    clip_paths={}
    clip_data={}
    for file in native.glob('*.hanim'):
        animation=json.loads(file.read_text(encoding='utf-8-sig'))
        for clip in animation['clips']:
            clip_paths[clip['name']]=file.relative_to(project).as_posix()
            clip_data[clip['name']]=(animation,clip)
    required=['Neutral','Slot5','Slot9','Slot16','Motion16']
    if not set(required)<=set(clip_paths): raise ValueError('Importer clip names differ: '+str(clip_paths))
    for primitive in mesh['primitives']:
        all_joints=[]
        for group in range(4):
            weights=values(primitive['attributes']['WEIGHTS_'+str(group)])
            if any(abs(w-1/16)>1e-6 for w in weights): raise ValueError('Importer changed positive sixteen weights')
            joints=values(primitive['attributes']['JOINTS_'+str(group)])
            all_joints.append(joints)
            if any(j<0 or j!=int(j) for j in joints): raise ValueError('Invalid native joints')
        for vertex in range(len(all_joints[0])//4):
            if len({group[vertex*4+lane] for group in all_joints for lane in range(4)})!=16:
                raise ValueError('Native vertex no longer has sixteen distinct positive influences')
        for name,joint_name in [('Slot5','Joint04'),('Slot9','Joint08'),('Slot16','Joint15')]:
            animation,clip=clip_data[name]
            bone=next(b for b in animation['boneInfos'] if b['name']==joint_name)
            channel=next(c for c in clip['channels'] if c['name']==joint_name)
            if any(abs(k['px']-8)>1e-6 or abs(k['py'])>1e-6 or abs(k['pz'])>1e-6 for k in channel['positionKeys']):
                raise ValueError('Native constant animation translation differs')
            contribution=[0.0]*(len(values(primitive['attributes']['WEIGHTS_0']))//4)
            for group in range(4):
                joints=values(primitive['attributes']['JOINTS_'+str(group)])
                weights=values(primitive['attributes']['WEIGHTS_'+str(group)])
                for vertex in range(len(contribution)):
                    contribution[vertex]+=sum(weights[vertex*4+lane]*8 for lane in range(4) if joints[vertex*4+lane]==bone['id'])
            if any(abs(x-.5)>1e-6 for x in contribution):raise ValueError('Native high influence contributes other than 0.5m')
    prefix=folder.relative_to(project).as_posix()
    template=json.loads((project/'Assets/Scenes/UpgradeCourtyard.he').read_text(encoding='utf-8-sig'))
    light=copy.deepcopy(next(v for v in template.values() if 'DirectionalLightComponent'in v))
    light['DirectionalLightComponent']['Intensity']=2
    environment=copy.deepcopy(next(v for v in template.values() if 'EnvironmentLightComponent'in v))
    camera=copy.deepcopy(next(v for v in template.values() if 'CameraComponent'in v and v['CameraComponent'].get('Primary')))
    camera={k:v for k,v in camera.items() if k in ('TagComponent','TransformComponent','CameraComponent')}
    camera['TransformComponent'].update(Translation=[0,2.2,6],Rotation=[-.1974,0,0])
    def entity(tag,mesh,position,scale,animation=None,material=None):
        value=dict(TagComponent=dict(Tag=tag),TransformComponent=dict(Translation=position,Rotation=[0,0,0],Scale=scale,ParentUuid=''),
            MeshComponent=dict(Path=mesh,meshResPath=mesh))
        if material:value['MeshComponent']['MaterialOverrides']=[material]
        if animation:value['AnimatorComponent']=dict(animResPath=animation,controllerPath='None',bPlay=True,bLoop=True,playSpeed=1,bEnableRootMotion=False,bLockRootMotionXZ=False)
        return value
    # Rigid ground uses the same cube topology with skin attributes removed.
    ground=copy.deepcopy(mesh)
    for primitive in ground['primitives']:
        primitive['attributes']={k:v for k,v in primitive['attributes'].items() if not k.startswith(('JOINTS_','WEIGHTS_'))}
    ground['skeletonPath']='';save(folder/'Ground.hmesh',ground)
    skin=json.loads((project/'Assets/Characters/Rain/Rain_body_1001.hmat').read_text(encoding='utf-8-sig'))
    for texture in skin.get('Textures',{}).values():texture['Path']=''
    for key in skin['Properties']:
        if key.startswith('bUse') and key.endswith('Map'):skin['Properties'][key]=False
    skin['Parameters']['g_BaseColorFactor']['Value']=[.025,.1,.8,1]
    skin['Properties']['ExposedProperties']['g_BaseColorFactor']=[.025,.1,.8,1]
    for name in list(skin['Properties']['ExposedProperties']):
        if isinstance(skin['Properties']['ExposedProperties'][name],str):skin['Properties']['ExposedProperties'][name]='normal' if 'Normal' in name else 'white'
    save(folder/'FixtureSkin.hmat',skin)
    for material in ['PBR','Skin']:
        override=prefix+'/FixtureSkin.hmat' if material=='Skin' else None
        for name in required+['Reference']:
            scene={str(uuid.uuid5(uuid.NAMESPACE_URL,'skin16/'+key)):value for key,value in [('camera',camera),('light',light),('environment',environment)]}
            cube=entity('Sixteen influence cube',mesh_path.relative_to(project).as_posix(),[.5 if name=='Reference' else 0,1,0],[1,1,1],clip_paths['Neutral' if name=='Reference' else name],override)
            scene[str(uuid.uuid5(uuid.NAMESPACE_URL,'skin16/cube'))]=cube
            scene[str(uuid.uuid5(uuid.NAMESPACE_URL,'skin16/ground'))]=entity('Shadow receiver',prefix+'/Ground.hmesh',[0,-.15,0],[10,.25,10])
            save(folder/(material+'-'+name+'.he'),scene)
    save(folder/'native-verification.json',dict(status='PASS_NATIVE_ONLY',clips=clip_paths,
        expectedConstantTranslationMeters=.5,groups=4,influences=16,
        gpuVerified=False,requiredGpuChecks=['same-material Slot5/9/16 against Reference pixels','depth agreement','shadow agreement','Motion16 motion vectors/history'],
        limitation='Native group checks do not establish GPU rendering or temporal correctness.'))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project',type=Path,required=True)
    parser.add_argument('--importer',type=Path)
    parser.add_argument('--compare',nargs=2,type=Path)
    args=parser.parse_args()
    if args.compare:
        from PIL import Image,ImageChops,ImageStat
        a,b=[Image.open(p).convert('RGB') for p in args.compare]
        if a.size!=b.size:raise ValueError('Capture sizes differ')
        difference=ImageStat.Stat(ImageChops.difference(a,b))
        print(json.dumps(dict(meanAbsoluteChannelError=difference.mean,scope='pixel comparison only; inspect shadow/depth/motion separately')))
        return
    project=args.project.resolve();folder=project/'Assets/LocalStudies/EngineSkin16Fixture'
    folder.mkdir(parents=True,exist_ok=True)
    marker=folder/'.skin16-fixture'
    if not marker.exists() and any(folder.iterdir()):raise ValueError('Refusing to replace an unowned fixture folder')
    marker.write_text('SkinInfluences.py owned fixture\n',encoding='utf-8')
    source_glb(folder/'InfluenceCube.glb')
    if args.importer:imported(project,folder,args.importer)
    print(str(folder))


if __name__=='__main__':main()
