#!/usr/bin/env python3
"""Author a closed camera path and verify sphere clearance against real LOD0 triangles."""
import argparse
import copy
import math
from pathlib import Path
import uuid
import numpy as np
from compress_benchmark_textures import read,write,sha,asset,PROJECT


def local_matrix(t):
    x,y,z=t['Rotation']
    cx,sx,cy,sy,cz,sz=math.cos(x),math.sin(x),math.cos(y),math.sin(y),math.cos(z),math.sin(z)
    r=np.array([[cz*cy,cz*sy*sx-sz*cx,cz*sy*cx+sz*sx],[sz*cy,sz*sy*sx+cz*cx,sz*sy*cx-cz*sx],[-sy,cy*sx,cy*cx]])
    m=np.eye(4);m[:3,:3]=r@np.diag(t['Scale']);m[:3,3]=t['Translation'];return m


def accessor(mesh,buffers,index):
    a=mesh['accessorVec'][index];v=mesh['bufferViewVec'][a['bufferViewIndex']]
    components={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4}[a['type']]
    dtype={5126:'<f4',5125:'<u4',5123:'<u2',5121:'u1'}[a['componentType']]
    item=np.dtype(dtype).itemsize;stride=v['byteStride'] or item*components
    return np.ndarray((a['count'],components),dtype=dtype,buffer=buffers[v['bufferIndex']],
                      offset=v['byteOffset']+a['byteOffset'],strides=(stride,item))


def distance_squared(point,triangles):
    a,b,c=triangles[:,0],triangles[:,1],triangles[:,2]
    ab,ac=b-a,c-a
    normal=np.cross(ab,ac);nn=np.sum(normal*normal,axis=1)
    ap=point-a;projection=ap-normal*(np.sum(ap*normal,axis=1)/np.maximum(nn,1e-30))[:,None]
    d00=np.sum(ab*ab,axis=1);d01=np.sum(ab*ac,axis=1);d11=np.sum(ac*ac,axis=1)
    d20=np.sum(projection*ab,axis=1);d21=np.sum(projection*ac,axis=1)
    denom=d00*d11-d01*d01
    u=(d11*d20-d01*d21)/np.maximum(denom,1e-30);v=(d00*d21-d01*d20)/np.maximum(denom,1e-30)
    inside=(u>=0)&(v>=0)&(u+v<=1)&(nn>1e-20)
    best=np.where(inside,np.sum(ap*normal,axis=1)**2/np.maximum(nn,1e-30),np.inf)
    for p,q in [(a,b),(b,c),(c,a)]:
        edge=q-p
        alpha=np.clip(np.sum((point-p)*edge,axis=1)/np.maximum(np.sum(edge*edge,axis=1),1e-30),0,1)
        delta=point-(p+edge*alpha[:,None]);best=np.minimum(best,np.sum(delta*delta,axis=1))
    return best


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scene',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--far',type=float,default=200.0)
    args=parser.parse_args()
    source=read(args.scene);scene=copy.deepcopy(source);memo={}
    for node in source.values():
        if any('ScriptComponent' in key or 'Effekseer' in key for key in node):
            raise ValueError('Static deterministic baseline cannot silently retain scripts or particles')
    def world(key):
        if key not in memo:
            t=source[key]['TransformComponent'];m=local_matrix(t);parent=t.get('ParentUuid')
            memo[key]=world(parent)@m if parent else m
        return memo[key]
    cameras=[k for k,n in source.items() if n.get('CameraComponent',{}).get('Primary')]
    if len(cameras)!=1:raise ValueError('Source requires exactly one primary camera')
    original_camera=cameras[0];matrix=world(original_camera);scales=np.linalg.norm(matrix[:3,:3],axis=0)
    rotation=matrix[:3,:3]/scales
    if np.max(np.abs(rotation.T@rotation-np.eye(3)))>1e-4:raise ValueError('Camera parent shear requires explicit source repair')
    origin=matrix[:3,3];right=rotation[:,0];forward=-rotation[:,2]
    euler=[math.atan2(rotation[2,1],rotation[2,2]),math.asin(np.clip(-rotation[2,0],-1,1)),math.atan2(rotation[1,0],rotation[0,0])]
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    trajectory={'version':1,'sourceScene':asset(args.scene.resolve()),'sourceSceneSha256':sha(args.scene),
                'originalCameraId':original_camera,'originalCamera':source[original_camera],
                'originalWorldBasisScale':scales.tolist(),'originalWorldMatrix':matrix.tolist(),
                'origin':origin.tolist(),'right':right.tolist(),'forward':forward.tolist(),'eulerRadians':euler,
                'lateralAmplitude':0.75,'forwardAmplitude':0.3,'yawAmplitude':0.15,'pitchAmplitude':0.025,
                'periodFrames':600,'fixedDeltaSeconds':1/60,'randomSeed':'not_applicable',
                'cameraPolicy':'Dedicated rigid root camera at authored world pose; original scene nodes retained; near=0.1, far='+str(args.far)}
    phases=np.arange(240)*2*math.pi/240
    points=origin+np.sin(phases)[:,None]*right*0.75+(1-np.cos(phases))[:,None]*forward*0.3
    sample_gap_bound=2*math.pi/240*(0.75+0.3)
    clearance=0.25+sample_gap_bound/2
    path_min=points.min(axis=0)-clearance;path_max=points.max(axis=0)+clearance
    world_min=np.full(3,np.inf);world_max=-world_min
    candidates=[];mesh_nodes=0;triangles_total=0
    for key,node in source.items():
        if 'MeshComponent' not in node:continue
        mesh_nodes+=1;mesh=read(PROJECT/node['MeshComponent']['meshResPath']);buffers=[(PROJECT/b['uri']).read_bytes() for b in mesh['bufferVec']];m=world(key)
        for primitive in mesh['primitives']:
            p=accessor(mesh,buffers,primitive['attributes']['POSITION']).astype(np.float64)
            p=p@m[:3,:3].T+m[:3,3]
            world_min=np.minimum(world_min,p.min(axis=0));world_max=np.maximum(world_max,p.max(axis=0))
            indices=accessor(mesh,buffers,primitive['indices']).reshape(-1,3)
            triangles_total+=len(indices)
            if np.any(p.max(axis=0)<path_min) or np.any(p.min(axis=0)>path_max):continue
            tri=p[indices]
            mask=np.all(tri.max(axis=1)>=path_min,axis=1)&np.all(tri.min(axis=1)<=path_max,axis=1)
            if np.any(mask):candidates.append(tri[mask])
    candidate_triangles=np.concatenate(candidates) if candidates else np.empty((0,3,3))
    min_squared=min((np.min(distance_squared(p,candidate_triangles)) for p in points),default=np.inf) if len(candidate_triangles) else np.inf
    # Rejected triangles lie outside the expanded full-path AABB, so all have
    # at least the test radius clearance; the actual lower bound is conservative.
    minimum_distance=min(math.sqrt(min_squared),clearance)
    lower_bound=minimum_distance-sample_gap_bound/2
    if lower_bound<0.25-1e-6:raise ValueError('Authored path intersects geometry clearance; minimum lower bound='+str(lower_bound))
    trajectory['geometryClearance']={'method':'Exact point-to-LOD0-triangle distance at 240 samples, minus half conservative arc-length spacing; expanded path AABB rejects only distant triangles',
                                    'worldBounds':[world_min.tolist(),world_max.tolist()],'meshNodes':mesh_nodes,'triangles':triangles_total,
                                    'candidateTriangles':len(candidate_triangles),'sphereRadius':0.25,'minimumClearanceLowerBound':lower_bound,'status':'PASS'}
    write(output/'trajectory.json',trajectory)
    for key in cameras:scene[key]['CameraComponent']['Primary']=False
    camera=copy.deepcopy(source[original_camera]);camera['TransformComponent']={'ParentUuid':'','Translation':origin.tolist(),'Rotation':euler,'Scale':[1,1,1]}
    camera['TagComponent']={'Tag':'BenchmarkCamera:'+asset(output/'trajectory.json')}
    camera['PythonScriptComponent']={'Path':'Scripts.RenderingBenchmarkCamera.py'}
    camera['CameraComponent']['Primary']=True
    camera['CameraComponent']['Camera']['mPerspectiveNear']=0.1
    camera['CameraComponent']['Camera']['mPerspectiveFar']=args.far
    scene[str(uuid.uuid5(uuid.NAMESPACE_URL,asset(output)))]=camera
    write(output/'Benchmark.he',scene)
    write(output/'benchmark_input.json',{'scene':asset(output/'Benchmark.he'),'sceneSha256':sha(output/'Benchmark.he'),
                                       'trajectory':asset(output/'trajectory.json'),'trajectorySha256':sha(output/'trajectory.json'),
                                       'script':'Scripts/RenderingBenchmarkCamera.py','scriptSha256':sha(PROJECT/'Scripts/RenderingBenchmarkCamera.py'),
                                       'fixedDeltaSeconds':1/60,'periodFrames':600,'randomSeed':'not_applicable',
                                       'randomComponents':'none; original static scene has no scripts, particles, procedural animation or RNG use',
                                       'geometryPolicy':'All original mesh nodes, material and geometry references retained; one rigid benchmark camera added'})
    print({'scene':asset(output/'Benchmark.he'),'geometryNodes':mesh_nodes,'triangles':triangles_total,'clearanceLowerBound':lower_bound,'sourceCameraScale':scales.tolist()})


if __name__=='__main__':main()
