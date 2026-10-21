"""Offline analysis only: no engine/process launch. No automatic speed/quality verdict."""
import csv,json,math,pathlib,statistics,sys
p=pathlib.Path(sys.argv[1]);runs=json.loads((p/'runs.json').read_text(encoding='utf-8-sig'))
def stats(v):
 s=sorted(v)
 if not s: raise ValueError('Missing samples')
 def q(t):
  x=(len(s)-1)*t;a=int(x);b=min(a+1,len(s)-1);return s[a]+(s[b]-s[a])*(x-a)
 return dict(n=len(s),mean=statistics.mean(s),median=q(.5),p95=q(.95),p99=q(.99),max=max(s))
result=[]
for run in runs:
 name=run['name'];rows=[r for r in csv.DictReader((p/(name+'.csv')).open()) if int(r['frame'])>600]
 assert len(rows)==1800
 cutoff=max(int(r['gpuWarmupCutoffSample']) for r in rows);gpu={}
 for r in rows:
  k=int(r['gpuSample'])
  if k>cutoff:
   v=float(r['gpuFrameSpanMs'])
   if k in gpu: assert gpu[k]==v,'Conflicting duplicate GPU sample'
   gpu[k]=v
 pass_samples={}
 for r in csv.DictReader((p/(name+'.csv.passes.csv')).open()):
  k=int(r['gpuSample']);n=r['passName']
  if k in gpu and n.startswith('DepthPyramid'):
   v=float(r['milliseconds']);key=(k,n)
   if key in pass_samples: assert pass_samples[key]==v
   pass_samples[key]=v
 totals=[];expected=1 if run['merge'] else 11
 for k in gpu:
  vals=[v for (sample,n),v in pass_samples.items() if sample==k]
  assert len(vals)==expected,'Missing/extra pyramid pass timestamp'
  totals.append(sum(vals))
 metrics={k:stats([float(r[k]) for r in rows]) for k in ['cpuWallFrameMs','renderSubmitPresentMs','cpuGraphRecordMs']}
 metrics['gpuFrameSpanMs']=stats(list(gpu.values()));metrics['gpuPyramidSumMs']=stats(totals)
 result.append(dict(name=name,repeat=run['repeat'],merge=run['merge'],gpuCutoff=cutoff,metrics=metrics))
pairs=[]
for i in range(3):
 a=next(x for x in result if x['repeat']==i and x['merge']==0);b=next(x for x in result if x['repeat']==i and x['merge']==1)
 pairs.append(dict(repeat=i,deltaMs={k:b['metrics'][k]['mean']-a['metrics'][k]['mean'] for k in a['metrics']}))
summary=dict(runs=result,pairedMeanDeltas=pairs,notes=['CPU all 1800 stable rows; GPU unique samples strictly above observed cutoff','Graph record includes Graph.Execute callback/barriers/instrumentation, excludes surrounding preparation and submission','Merged pass removes ten timestamp pairs; report observer overhead limitation','No multiqueue experiment; no automatic speedup or visual PASS'])
(p/'summary.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
lines=['# Depth pass merge A/B','', '| Run | CPU graph mean | GPU frame mean | GPU pyramid mean | GPU samples |','|---|---:|---:|---:|---:|']
for x in result:
 m=x['metrics'];lines.append('| {} | {:.6f} | {:.6f} | {:.6f} | {} |'.format(x['name'],m['cpuGraphRecordMs']['mean'],m['gpuFrameSpanMs']['mean'],m['gpuPyramidSumMs']['mean'],m['gpuFrameSpanMs']['n']))
lines+=['','All units ms. Distribution details and paired deltas are in summary.json. This is not a visual quality verdict. Ten fewer timestamp pairs may affect measured cost.']
(p/'summary.md').write_text('\n'.join(lines),encoding='utf-8')
print('Offline summary written.')
