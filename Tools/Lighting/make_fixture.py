"""Create an isolated, reproducible bake fixture in any HEngine project."""
import argparse
import json
from pathlib import Path
import struct
import uuid

import numpy as np

from bake import asset
from lighting_math import write_hdr


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--project',required=True,type=Path)
    args=parser.parse_args(); root=args.project.resolve()
    base='Assets/Generated/LightingFixture'
    folder=asset(root,base); folder.mkdir(parents=True,exist_ok=True)
    def write(name,data):
        (folder/name).write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8')
        return base+'/'+name
    # One upward-facing quad, unique padded UV1; instances form floor and walls.
    positions=[[-2,0,-2],[-2,0,2],[2,0,2],[2,0,-2]]
    indices=[0,1,2,0,2,3]
    uv=[[.05,.05],[.05,.95],[.95,.95],[.95,.05]]
    raw=bytearray(); views=[]; accessors=[]
    def buffer(values,components,kind):
        offset=len(raw)
        flattened=np.array(values,dtype='<f4' if kind==5126 else '<u4').reshape(-1)
        raw.extend(flattened.tobytes()); views.append({'bufferIndex':0,'byteOffset':offset,'byteLength':len(raw)-offset})
        accessors.append({'bufferViewIndex':len(views)-1,'byteOffset':0,'count':len(flattened)//components,
            'type':{1:'SCALAR',2:'VEC2',3:'VEC3'}[components],'componentType':kind,'normalized':False})
        return len(accessors)-1
    p=buffer(positions,3,5126); n=buffer([[0,1,0]]*4,3,5126); u=buffer(uv,2,5126); i=buffer(indices,1,5125)
    (folder/'quad.bin').write_bytes(raw)
    def mat(name,color):
        return write(name+'.hmat',{'SchemaVersion':2,'Type':'Custom','ShaderPath':'Shaders/BasePBR.hlsl','PassName':'BasePBR',
            'Domain':'Surface','BlendMode':'Opaque','CullMode':'None','DepthWrite':True,
            'Properties':{'pbrMetallicRoughness':{'baseColorFactor':color,'metallicFactor':0,'roughnessFactor':.8},
                'ExposedProperties':{'g_BaseColorFactor':color,'g_MetallicFactor':0,'g_RoughnessFactor':.8}},
            'Parameters':{key:{'StableId':key,'Type':kind,'Value':value} for key,kind,value in
                [('g_BaseColorFactor','color',color),('g_MetallicFactor','float',0),
                 ('g_RoughnessFactor','float',.8),('g_AoStrength','float',0)]}})
    white=mat('white',[.7,.7,.7,1]); red=mat('red',[.65,.025,.025,1]); green=mat('green',[.025,.5,.025,1])
    mesh=write('quad.hmesh',{'name':'LightingFixtureQuad','bufferVec':[{'uri':base+'/quad.bin','byteLength':len(raw)}],
        'bufferViewVec':views,'accessorVec':accessors,'primitives':[{'mode':4,'indices':i,
        'attributes':{'POSITION':p,'NORMAL':n,'TEXCOORD_0':u,'TEXCOORD_1':u},'materialResPath':white}]})
    sky=base+'/sky.hdr'; write_hdr(folder/'sky.hdr',np.ones((32,64,3))*[.35,.4,.5])
    scene={}
    def entity(name,position=[0,0,0],rotation=[0,0,0],**components):
        identifier=str(uuid.uuid5(uuid.NAMESPACE_URL,'he-lighting-fixture:'+name))
        scene[identifier]={'TagComponent':{'Tag':name},'TransformComponent':{
            'Translation':position,'Rotation':rotation,'Scale':[1,1,1]},**components}
        return identifier
    floor=entity('Floor',MeshComponent={'Path':mesh,'MaterialOverrides':[white]})
    entity('RedWall',[-2,2,0],[0,0,-1.57079632679],MeshComponent={'Path':mesh,'MaterialOverrides':[red]})
    entity('GreenWall',[2,2,0],[0,0,1.57079632679],MeshComponent={'Path':mesh,'MaterialOverrides':[green]})
    entity('BackWall',[0,2,-2],[1.57079632679,0,0],MeshComponent={'Path':mesh,'MaterialOverrides':[white]})
    entity('Sun',[0,5,0],DirectionalLightComponent={'Intensity':2,'Color':[1,.9,.8],'LocalDirection':[.2,-1,-.3]})
    entity('Environment',EnvironmentLightComponent={'HdrTexturePath':sky,'IBLIntensity':1,'ExposureEV100':0})
    entity('Camera',[0,2.5,6],[-.15,0,0],CameraComponent={'Primary':True,'bOverrideExposure':True,
        'ExposureEV100':0,'Camera':{'mProjectionType':0,'mPerspectiveFOV':.95,'mPerspectiveNear':.05,'mPerspectiveFar':100}})
    scene_path=write('Source.he',scene)
    config={'version':1,'scene':scene_path,'output':base+'/Baked','samples':32,'probeResolution':32,
        'bounces':4,'device':'CPU','probes':[{'id':'room','position':[0,1.5,0],
        'boundsMin':[-1.95,.05,-1.95],'boundsMax':[1.95,3.95,1.95],'blendDistance':.25}],
        'lightmaps':[{'entity':floor,'resolution':64,'padding':2,'generateUV1':True}]}
    write('Bake.json',config)
    phi=((np.arange(128)+.5)/128-.5)*2*np.pi
    theta=(.5-(np.arange(64)+.5)/64)*np.pi
    p,t=np.meshgrid(phi,theta)
    directions=np.stack([np.cos(t)*np.cos(p),np.sin(t),np.cos(t)*np.sin(p)],axis=-1)
    write_hdr(folder/'axes.hdr',.5+.5*directions)
    direction_scene={key:value for key,value in scene.items() if 'EnvironmentLightComponent' in value}
    for value in direction_scene.values(): value['EnvironmentLightComponent']['HdrTexturePath']=base+'/axes.hdr'
    write('DirectionSource.he',direction_scene)
    write('DirectionBake.json',{'version':1,'scene':base+'/DirectionSource.he','output':base+'/DirectionBaked',
        'samples':8,'probeResolution':32,'probes':[{'id':'axes','position':[0,1,0],
        'boundsMin':[-2,-2,-2],'boundsMax':[2,2,2]}]})
    print('Fixture config:',base+'/Bake.json')


if __name__=='__main__': main()
