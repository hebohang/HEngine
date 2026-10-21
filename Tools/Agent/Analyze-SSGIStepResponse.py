"""Analyze sparse rectangular-fixture step captures in an explicit directory.

Example: python Tools/Agent/Analyze-SSGIStepResponse.py --capture-directory RUN
  --reference-directory CONSTANT --compare PREVIOUS/analysis-dynamics.json
Requires Analyze-SSGITransport.py, events.json and matching PFM/world files.
The reference directory must contain the validated rectangular-fixture guides
and analysis-fixture-constant.json. This is not a generic motion-image scorer.
"""
import argparse
import importlib.util
import hashlib
import json
from pathlib import Path
import re
import numpy as np

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--capture-directory',type=Path,required=True)
parser.add_argument('--reference-directory',type=Path,required=True)
parser.add_argument('--compare',type=Path)
parser.add_argument('--output',type=Path)
args=parser.parse_args()
directory=args.capture_directory.resolve()
constant=args.reference_directory.resolve()
root=next(p for p in Path(__file__).resolve().parents if (p/'Tools/Agent/Analyze-SSGITransport.py').exists())
spec=importlib.util.spec_from_file_location('transport',root/'Tools/Agent/Analyze-SSGITransport.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
reference=json.loads((constant/'analysis-fixture-constant.json').read_text())
events=json.loads((directory/'events.json').read_text())

def provenance(capture_dir):
    start=capture_dir/'start.log'
    if not start.exists():
        return {'status':'start log unavailable'}
    match=re.search(r'^session: (.+)$',start.read_text(errors='replace'),re.MULTILINE)
    if not match:
        return {'status':'session path unavailable'}
    log=Path(match.group(1).strip()).parent/'game.stdout.log'
    if not log.exists():
        return {'status':'session log unavailable','session_log':str(log)}
    scene_match=re.search(r'Ready scene=(\S+) viewport=',log.read_text(errors='replace'))
    if not scene_match:
        return {'status':'Ready scene unavailable','session_log':str(log)}
    scene_relative=scene_match.group(1);scene_path=root/'Project'/scene_relative
    result={'scene':scene_relative,'session_log':str(log),
            'hash_scope':'Current on-disk hashes only; startup did not capture historical scene/material hashes.'}
    if scene_path.exists():
        scene=json.loads(scene_path.read_text())
        materials=sorted({path for entity in scene.values() for path in entity.get('MeshComponent',{}).get('MaterialOverrides',[])})
        result['material_paths']=materials
        result['current_scene_sha256']=hashlib.sha256(scene_path.read_bytes()).hexdigest()
        result['current_material_sha256']={p:hashlib.sha256((root/'Project'/p).read_bytes()).hexdigest() for p in materials}
    return result
base_world=reference['world']
depth=m.load_image(constant/'baseline-editor-fixture-depth.pfm')[0][...,0]
normal=m.load_image(constant/'baseline-editor-fixture-normal.pfm')[0]
view=np.array(base_world['viewColumnMajor']).reshape(4,4,order='F')
projection=np.array(base_world['projectionColumnMajor']).reshape(4,4,order='F')
height,width=depth.shape;yy,xx=np.indices(depth.shape)
viewpos=np.stack([((xx+.5)/width*2-1)*depth/projection[0,0],(1-(yy+.5)/height*2)*depth/projection[1,1],
                  -depth,np.ones_like(depth)],axis=2)
position=(viewpos@np.linalg.inv(view).T)[...,:3]
camera=np.array(base_world['cameraPosition']);rays=position-camera
t=-camera[1]/np.where(abs(rays[...,1])>1e-8,rays[...,1],1)
floor_point=camera+rays*t[...,None]
floor=((normal[...,1]>.99)&(abs(position[...,1])<.02)&(position[...,0]>.1)&
       (position[...,0]<3.95)&(abs(position[...,2])<1.95))
rois={}
for distance in (1.,2.):
    mask=floor&(abs(floor_point[...,0]-distance)<.1)&(abs(floor_point[...,2])<.3)
    name=f'd{int(distance)}';points=floor_point[mask];rows,cols=np.where(mask)
    expected=np.array(reference['rois'][name]['expected_captured_source_actual_albedo_rgb'])
    rois[name]={'points':np.column_stack((points,np.ones(len(points)))), 'rows':rows,'cols':cols,
               'expected':expected,'static':[],'reactive_noise':[]}
    assert len(points)==reference['rois'][name]['raw']['pixels']

def sample_world_points(image,world,roi):
    current_view=np.array(world['viewColumnMajor']).reshape(4,4,order='F')
    current_projection=np.array(world['projectionColumnMajor']).reshape(4,4,order='F')
    static_view=bool(np.max(abs(current_view-view))<1e-5 and np.max(abs(current_projection-projection))<1e-5)
    if static_view:
        return image[roi['rows'],roi['cols']],True
    clip=roi['points']@(current_projection@current_view).T
    ndc=clip[:,:3]/clip[:,3:]
    px=(ndc[:,0]*.5+.5)*width-.5;py=(.5-ndc[:,1]*.5)*height-.5
    inside=(clip[:,3]>0)&(px>=0)&(px<width-1)&(py>=0)&(py<height-1)
    if not inside.all():
        raise ValueError('World ROI leaves viewport; do not compare different support')
    bx=np.floor(px).astype(int);by=np.floor(py).astype(int);fx=(px-bx)[:,None];fy=(py-by)[:,None]
    values=(image[by,bx]*(1-fx)*(1-fy)+image[by,bx+1]*fx*(1-fy)+
            image[by+1,bx]*(1-fx)*fy+image[by+1,bx+1]*fx*fy)
    return values,False

settings=json.loads((directory/'settings.json').read_text()) if (directory/'settings.json').exists() else None
effective=(settings or {}).get('effective',{})
debug_view=(settings or {}).get('debug',{}).get('Rendering.Debug.GI.View')
aa_mode=effective.get('Rendering.AntiAliasing.Mode')
final_taa=debug_view in (0,'Composite') and aa_mode=='TAA'
report={'evidence':'CPU analysis of runtime-manual sparse step captures','acceptance':'NOT FULL DYNAMIC ACCEPTANCE',
        'capture_directory':str(directory),
        'provenance':provenance(directory),
        'settings_snapshot':settings,
        'pipeline':{'debug_view':debug_view,'effective_aa':aa_mode,
                    'final_taa_enabled_from_settings':final_taa if settings is not None else None},
        'delay_contract':'actual_age = world.frame - mutation.mutationRenderFrame; settledRenderFrame is readback completion only',
        'roi_contract':'Reuse constant-reference floor depth/normal mask and exact y=0 camera-ray intersection; d1/d2 x +/-0.1, |z|<0.3',
        'camera_sampling':'Same-view captures use identical pixels; moved camera captures project identical world points and bilinearly sample.',
        'visibility_contract':'All chosen receivers and cameras have x>0 and camera y>0; source board x=[-0.1,0] cannot obscure these camera-floor segments. No current depth buffer was captured.',
        'limitations':['Static and reactive-noise captures are separated observations, not consecutive frames or a flicker spectrum.',
                       'On/off steps are independent repeated transitions at requested delays, not one contiguous decay curve.',
                       'Camera moves/cut have one early capture each, so they cannot establish motion trail length or settling time.',
                       'Reference uses original-camera captured-source radiance. Moved cameras can change screen visibility and source specular bias.',
                       'Object move shifts light-transport geometry; original-board expected energy is not a new-board physical reference.',
                       'World snapshots use the captured camera projection; subpixel raster jitter is not separately reconstructed, so world ROI sampling is approximate when TAA is enabled.',
                       'Composite with effective TAA is represented; these sparse captures do not establish continuous-video TAA trail quality.' if final_taa else
                       'Final Composite TAA is not established by the settings snapshot.',
                       'No continuous video or complete disocclusion/occluder stress test is represented.'],
        'reference':{'file':str(constant/'analysis-fixture-constant.json'),
                     'kind':'Independent area quadrature of captured source; not universal physical truth'},
        'events':[],'static':{},'reactive_noise':{},'response':{},'all_nonfinite_zero':True}
for event in events:
    name=event['name'];path=directory/(name+'.pfm')
    image=m.load_image(path)[0];world=json.loads(Path(str(path)+'.world.json').read_text())
    actual_age=world['frame']-event['mutation']['mutationRenderFrame']
    assert actual_age>=0
    row={'name':name,'requested_delay':event['delayFrames'],'actual_delay_frames':actual_age,
         'mutation_frame':event['mutation']['mutationRenderFrame'],'capture_world_frame':world['frame'],
         'readback_settled_frame':event['capture']['settledRenderFrame'],
         'world':world,
         'sun_lux':world['directionalLights'][0]['intensity'],'environment':world['environmentIntensity'],
         'exposure':world['exposureMultiplier'],'regions':{},'nonfinite':int((~np.isfinite(image)).sum()),
         'negative':int((image<0).sum()),'fp16_max':int((image==65504).sum())}
    report['all_nonfinite_zero'] &= row['nonfinite']==0
    for key,roi in rois.items():
        values,static_view=sample_world_points(image,world,roi);luma=values@m.LUMA
        expectation=float(roi['expected']@m.LUMA)
        row['regions'][key]={'pixels':len(values),'same_view':static_view,
                             'luminance':m.summary(luma),'rgb_mean':values.mean(axis=0).tolist(),
                             'mean_over_reference':float(luma.mean()/expectation),
                             'expected_rgb':roi['expected'].tolist(),
                             'expected_luminance':expectation}
        if name.startswith('static-'):
            assert static_view
            roi['static'].append(values)
        elif name.startswith('reactive-noise-'):
            assert static_view
            roi['reactive_noise'].append(values)
    report['events'].append(row)

def summarize_observations(observations,expectation):
    samples=np.stack(observations);lum=samples@m.LUMA;means=lum.mean(axis=1)
    temporal_std=lum.std(axis=0)
    return {'samples':len(samples),'pixels':samples.shape[1],
                           'roi_luminance_means':means.tolist(),'mean':float(means.mean()),
                           'mean_over_reference':float(means.mean()/expectation),
                           'roi_mean_population_std':float(means.std()),
                           'roi_mean_relative_std':float(means.std()/means.mean()) if means.mean()!=0 else None,
                           'per_pixel_temporal_std':m.summary(temporal_std),
                           'per_pixel_temporal_std_over_reference_mean':float(temporal_std.mean()/expectation)}

noise_events=[r for r in report['events'] if r['name'].startswith('reactive-noise-')]
if noise_events:
    report['reactive_noise_protocol']={
        'scope':'Near-static short-history observations; not a continuous-video motion/noise measurement.',
        'producer_contract':'Source board z increments by 1e-5 per capture, with requested delay 12; the original-board reference approximates this negligible geometric change. World snapshots do not include per-object transforms to independently verify the increment.',
        'requested_delays':[r['requested_delay'] for r in noise_events],
        'actual_delays':[r['actual_delay_frames'] for r in noise_events],
        'all_requested_delays_are_12':all(r['requested_delay']==12 for r in noise_events)}
for key,roi in rois.items():
    expectation=float(roi['expected']@m.LUMA)
    for group in ('static','reactive_noise'):
        if roi[group]:
            report[group][key]=summarize_observations(roi[group],expectation)
    if key not in report['static']:
        continue
    static_mean=report['static'][key]['mean']
    if key in report['reactive_noise']:
        noise=report['reactive_noise'][key];baseline=report['static'][key]
        noise['comparison_to_static']={
            'mean_ratio':noise['mean']/static_mean if static_mean else None,
            'roi_mean_std_ratio':noise['roi_mean_population_std']/baseline['roi_mean_population_std'] if baseline['roi_mean_population_std'] else None,
            'mean_per_pixel_std_ratio':noise['per_pixel_temporal_std']['mean']/baseline['per_pixel_temporal_std']['mean'] if baseline['per_pixel_temporal_std']['mean'] else None}
    # Independent changed-geometry configuration factor: shifted board keeps
    # orientation, x=0 and height 2, but z changes from [-2,2] to [4,8].
    points=roi['points'];d=points[:,0];z=points[:,2];a=np.sqrt(d*d+4)
    def primitive(t):
        return (np.arctan(t/d)-d/a*np.arctan(t/a))/(2*np.pi)
    original_factor=primitive(2-z)-primitive(-2-z)
    shifted_factor=primitive(8-z)-primitive(4-z)
    report['static'][key]['shifted_board_full_geometry_constant_source_factor_ratio']=float(shifted_factor.mean()/original_factor.mean())
    for row in report['events']:
        row['regions'][key]['mean_over_static_mean']=row['regions'][key]['luminance']['mean']/static_mean if static_mean else None
    for group in ('off-','on-','board-out-'):
        group_rows=[r for r in report['events'] if r['name'].startswith(group)]
        curves=[{'name':r['name'],'actual_delay':r['actual_delay_frames'],
                 'mean_over_static':r['regions'][key]['mean_over_static_mean'],
                 'mean_over_reference':r['regions'][key]['mean_over_reference'],
                 'luminance_mean':r['regions'][key]['luminance']['mean'],
                 'nonzero':r['regions'][key]['luminance']['nonzero_finite']} for r in group_rows]
        report['response'].setdefault(group,{})[key]=curves
if args.compare:
    previous=json.loads(args.compare.read_text())
    previous_dir=Path(previous.get('capture_directory',args.compare.resolve().parent))
    previous_settings=json.loads((previous_dir/'settings.json').read_text())
    current_settings=report['settings_snapshot']
    checks={key:previous_settings.get(key)==current_settings.get(key) for key in ('effective','requested','debug')}
    comparison={'baseline':str(args.compare),'settings_match':checks,'static':{},'events':{},'world_matches':{}}
    comparison['previous_provenance']=provenance(previous_dir)
    comparison['same_scene_path']=comparison['previous_provenance'].get('scene')==report['provenance'].get('scene')
    comparison['same_current_material_paths']=comparison['previous_provenance'].get('material_paths')==report['provenance'].get('material_paths')
    comparison['sampling_limitation']='Capture frame sequences differ; eight static observations cannot establish significance of small mean/std changes.'
    old_by_name={e['name']:e for e in previous['events']}
    for key in rois:
        if key not in report['static'] or key not in previous['static']:
            continue
        old=previous['static'][key];new=report['static'][key]
        comparison['static'][key]={'mean_ratio':new['mean']/old['mean'],
                                  'roi_mean_std_ratio':new['roi_mean_population_std']/old['roi_mean_population_std'],
                                  'mean_per_pixel_std_ratio':new['per_pixel_temporal_std']['mean']/old['per_pixel_temporal_std']['mean']}
    for row in report['events']:
        name=row['name']
        if name not in old_by_name:
            continue
        old=old_by_name[name]
        old_world=old.get('world') or json.loads((previous_dir/(name+'.pfm.world.json')).read_text())
        comparison['world_matches'][name]={key:old_world[key]==row['world'][key] for key in old_world if key!='frame'}
        comparison['events'][name]={'before_actual_delay':old['actual_delay_frames'],'after_actual_delay':row['actual_delay_frames'],
                                   'regions':{key:{'before_ratio_to_static':old['regions'][key].get('mean_over_static_mean'),
                                                   'after_ratio_to_static':row['regions'][key].get('mean_over_static_mean'),
                                                   'before_ratio_to_reference':old['regions'][key]['mean_over_reference'],
                                                   'after_ratio_to_reference':row['regions'][key]['mean_over_reference']}
                                              for key in rois}}
    report['comparison']=comparison
output=args.output or directory/'analysis-dynamics.json'
output.parent.mkdir(parents=True,exist_ok=True)
output.write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
print(json.dumps({'output':str(output),'events':len(report['events']),'static':report['static'],
                  'reactive_noise':report['reactive_noise'],'pipeline':report['pipeline'],
                  'response':report['response'],
                  'camera_steps':[{ 'name':r['name'],'age':r['actual_delay_frames'],
                                    'd1_reference_ratio':r['regions']['d1']['mean_over_reference'],
                                    'd2_reference_ratio':r['regions']['d2']['mean_over_reference']}
                                   for r in report['events'] if r['name'].startswith(('translate-','rotate-','camera-','board-restore'))]},indent=2))
