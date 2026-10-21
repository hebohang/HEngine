import copy,json,struct,tempfile,unittest,zipfile
from pathlib import Path
from lifecycle_acceptance import live_cycle_valid,stress_identity,combine_reports,ROOT,BUDGET,digest,no_resource_growth,resource_observations,POOL_FIELDS
from generate_effekseer_stress_assets import parse_chunks,validate_compiled_stress
class Contracts(unittest.TestCase):
 def resource_samples(self):
  return [dict(stats=dict(bufferPools=dict(available=True,**{k:100 for k in POOL_FIELDS}),
                         renderMemory=dict(available=True,perProcess=True,localUsageBytes=1000,nonLocalAvailable=True,nonLocalUsageBytes=500))) for _ in range(25)]
 def test_resident_pools_nonzero_and_os_usage_stable(self):
  self.assertEqual([c['status'] for c in resource_observations(self.resource_samples())],['PASS']*3)
 def test_each_pool_growth_and_missing_measurement_rejected(self):
  for key in POOL_FIELDS:
   for value in (101,None,True,float('nan')):
    with self.subTest(key=key,value=value):
     samples=self.resource_samples();samples[10]['stats']['bufferPools'][key]=value
     self.assertEqual(resource_observations(samples)[0]['status'],'FAIL')
  samples=self.resource_samples();samples[0]['stats']['bufferPools']['available']=False
  self.assertEqual(resource_observations(samples)[0]['status'],'FAIL')
 def test_os_unavailable_not_zero_and_growth_failure(self):
  samples=self.resource_samples()
  for sample in samples:sample['stats']['renderMemory']={}
  self.assertEqual([c['status'] for c in resource_observations(samples)],['PASS','NOT_VERIFIED','NOT_VERIFIED'])
  for field,index in [('localUsageBytes',1),('nonLocalUsageBytes',2)]:
   samples=self.resource_samples();samples[12]['stats']['renderMemory'][field]+=1
   self.assertEqual(resource_observations(samples)[index]['status'],'FAIL')
  samples=self.resource_samples();samples[12]['stats']['renderMemory']['available']=False
  self.assertEqual([c['status'] for c in resource_observations(samples)][1:],['FAIL','FAIL'])
  samples=self.resource_samples()
  for sample in samples:sample['stats']['renderMemory']['perProcess']=False
  self.assertEqual([c['status'] for c in resource_observations(samples)][1:],['NOT_VERIFIED','NOT_VERIFIED'])
 def test_live_requires_both_fresh_exact_timers_and_work(self):
  before={k:1 for k in ('lastGpuSimulationSubmission','lastGpuRenderSubmission','totalRecordedDispatchCount','totalRecordedDrawCount')}
  good={k:2 for k in before};good.update({k:dict(status=1,current=.1) for k in ('gpuSimulationMs','gpuRenderMs')})
  self.assertTrue(live_cycle_valid(before,good))
  for metric in ('gpuSimulationMs','gpuRenderMs'):
   for value in (None,0,-1,float('nan')):
    bad=copy.deepcopy(good);bad[metric]['current']=value;self.assertFalse(live_cycle_valid(before,bad))
   bad=copy.deepcopy(good);bad[metric]['status']=0;self.assertFalse(live_cycle_valid(before,bad))
  for key in before:
   bad=copy.deepcopy(good);bad[key]=1;self.assertFalse(live_cycle_valid(before,bad))
 def test_no_positive_resource_tolerance(self):
  self.assertTrue(no_resource_growth([100,100],0,0))
  self.assertFalse(no_resource_growth([100,101],1,.1))
  self.assertFalse(no_resource_growth([100,101,99],-1,-.1))
  self.assertFalse(no_resource_growth([100,100],0,.001))
 def test_real_compiled_nominal(self):
  for n in (20000,50000):
   p=ROOT/f'Project/Assets/Effects/Stress/GpuSparks{n//1000}k.efkpkg'
   self.assertEqual(stress_identity(p,n)['nominalParticles'],n)
   with self.assertRaises(ValueError):stress_identity(p,70000-n)
 def test_compiled_version_sampler_and_rate_tampering_rejected(self):
  with tempfile.TemporaryDirectory() as temp:
   for n in (20000,50000):
    original=ROOT/f'Project/Assets/Effects/Stress/GpuSparks{n//1000}k.efkpkg'
    with zipfile.ZipFile(original) as archive:
     entries={name:archive.read(name) for name in archive.namelist()}
    effect=next(name for name in entries if name.endswith('.efkefc'))
    payload=entries[effect];layout=validate_compiled_stress(payload,n)
    for label,offset,value in (('legacy_version',4,1800),('sampler',layout['cpuSamplerOffset'],331),('rate',layout['gpuNodeOffset']+8,999)):
     with self.subTest(nominal=n,mutation=label):
      chunks=[]
      for tag,data in parse_chunks(payload):
       if tag==b'BIN_':
        data=bytearray(data);struct.pack_into('<i',data,offset,value)
       chunks.append(tag+struct.pack('<I',len(data))+data)
      mutated=dict(entries);mutated[effect]=payload[:8]+b''.join(chunks)
      destination=Path(temp)/f'{n}-{label}.efkpkg'
      with zipfile.ZipFile(destination,'w') as archive:
       for name,data in mutated.items():archive.writestr(name,data)
      with self.assertRaisesRegex(ValueError,'Compiled stress validation failed'):
       stress_identity(destination,n)
 def test_provenance_rate_lifetime_product_cannot_hide_mismatch(self):
  with tempfile.TemporaryDirectory() as temp:
   original=ROOT/'Project/Assets/Effects/Stress/GpuSparks20k.efkpkg'
   with zipfile.ZipFile(original) as archive:
    entries={name:archive.read(name) for name in archive.namelist()}
   metadata=json.loads(entries['PROVENANCE.json'])
   metadata['parameters'].update(lifetime_frames=100,emit_per_frame=200)
   entries['PROVENANCE.json']=json.dumps(metadata).encode()
   destination=Path(temp)/'metadata.efkpkg'
   with zipfile.ZipFile(destination,'w') as archive:
    for name,data in entries.items():archive.writestr(name,data)
   with self.assertRaisesRegex(ValueError,'metadata differs from compiled lifetime_frames'):
    stress_identity(destination,20000)
 def test_combine_requires_two_scales_and_actual_cycle_checks(self):
  with tempfile.TemporaryDirectory() as temp:
   paths=[]
   for n in (20000,50000):
    checks=[dict(name=f'cycle_{i}.{s}',status='PASS') for i in range(30) for s in ('live_work','retired_zero')]+[dict(name='cycles_private_memory_trend',status='PASS')]+resource_observations(self.resource_samples())
    data=dict(nominalIdentity=dict(nominalParticles=n),executableSha256='fixed',backend='DX12',budgetSha256=digest(BUDGET),budgetUnchanged=True,commandStatus='PASS',cycleSamples=[{}]*30,checks=checks)
    p=Path(temp)/str(n);p.write_text(json.dumps(data));paths.append(p)
   self.assertEqual(combine_reports(paths)['acceptanceStatus'],'PARTIAL')
   self.assertEqual(combine_reports([paths[0],paths[0]])['commandStatus'],'FAIL')
   data=json.loads(paths[1].read_text());original=copy.deepcopy(data)
   for check in data['checks']:
    if check['name']=='cycles_os_local_memory_trend':check['status']='FAIL'
   paths[1].write_text(json.dumps(data))
   self.assertEqual(combine_reports(paths)['commandStatus'],'FAIL')
   paths[1].write_text(json.dumps(original))
   data=json.loads(paths[1].read_text());data['checks']=[];paths[1].write_text(json.dumps(data))
   self.assertEqual(combine_reports(paths)['commandStatus'],'FAIL')
if __name__=='__main__':unittest.main()
