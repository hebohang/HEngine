"""Offline version comparison. No engine launch. Missing columns are unavailable, never zero."""
import csv,hashlib,json,pathlib,statistics,sys,math

def digest(path):
 h=hashlib.sha256()
 with path.open('rb') as f:
  for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
 return h.hexdigest()
def stats(v):
 if not v:return None
 s=sorted(v)
 def q(p):
  x=(len(s)-1)*p;a=int(x);b=min(a+1,len(s)-1);return s[a]+(s[b]-s[a])*(x-a)
 return dict(n=len(v),mean=statistics.mean(v),median=q(.5),p95=q(.95),p99=q(.99),max=max(v))
mode=sys.argv[1];p=pathlib.Path(sys.argv[2])
if mode=='manifest':
 # Hash source assets and descriptor/config, not generated shader/DDC/temp outputs.
 paths=set()
 for folder in ['Assets','Config','Settings','Scripts']:
  d=p/folder
  if d.exists():paths.update(x for x in d.rglob('*') if x.is_file())
 paths.update(x for x in p.iterdir() if x.is_file())
 data=[dict(path=x.relative_to(p).as_posix(),bytes=x.stat().st_size,sha256=digest(x)) for x in sorted(paths,key=lambda x:x.relative_to(p).as_posix())]
 pathlib.Path(sys.argv[3]).write_text(json.dumps(data,indent=2),encoding='utf-8');print('Hashed input files:',len(data));sys.exit(0)
runs=json.loads((p/'runs.json').read_text(encoding='utf-8-sig'));result=[]
for run in runs:
 rows=list(csv.DictReader((p/(run['name']+'.csv')).open()));rows=[r for r in rows if int(r['frame'])>600]
 assert len(rows)==1800,(run['name'],len(rows))
 assert [int(r['frame']) for r in rows]==list(range(601,2401)), 'Stable frame scope is not exactly 601..2400'
 assert all(int(r['width'])==1920 and int(r['height'])==1080 for r in rows), 'Resolution changed'
 cutoff=max(int(r['gpuWarmupCutoffSample']) for r in rows);gpu={}
 for r in rows:
  k=int(r['gpuSample'])
  if k>cutoff:
   v=(r['gpuFrameSpanMs'],r['gpuPassSumMs'])
   if k in gpu:assert gpu[k]==v,'Conflicting GPU duplicates'
   gpu[k]=v
 assert gpu,'No post-warmup GPU samples'
 columns=set(rows[0]);metrics={}
 for col in sorted(columns-{'frame','gpuSample','gpuWarmupCutoffSample'}):
  if col in ['gpuFrameSpanMs','gpuPassSumMs']:
   index=0 if col=='gpuFrameSpanMs' else 1;values=[float(v[index]) for v in gpu.values()]
  else:
   try:values=[float(r[col]) for r in rows]
   except (TypeError,ValueError):
    metrics[col]=dict(status='not_available',reason='non-numeric CSV column',observedValues=sorted(set(str(r[col]) for r in rows))[:8]);continue
  if not all(math.isfinite(v) for v in values):raise ValueError('Non-finite CSV metric: '+col)
  metrics[col]=stats(values)
 for new in ['cpuGraphRecordMs','cpuFramePreparationMs','cpuMaterialPackingMs','materialPackingCalls','materialPackingCacheHits','customGpuDraws','customGpuObjects']:
  if new not in metrics:metrics[new]=dict(status='not_available',reason='column absent in this binary')
 result.append(dict(name=run['name'],side=run['side'],workload=run['workload'],repeat=run['repeat'],cpuRows=len(rows),gpuUnique=len(gpu),cutoff=cutoff,metrics=metrics))
pairs=[]
for workload in ['LocalLights','Sponza','SurfaceCoverage']:
 for repeat in range(3):
  a=next(x for x in result if x['workload']==workload and x['repeat']==repeat and x['side']=='baseline');b=next(x for x in result if x['workload']==workload and x['repeat']==repeat and x['side']=='final')
  common={k:dict(baseline=a['metrics'][k]['mean'],final=b['metrics'][k]['mean'],delta=b['metrics'][k]['mean']-a['metrics'][k]['mean']) for k in a['metrics'] if k in b['metrics'] and 'mean' in a['metrics'][k] and 'mean' in b['metrics'][k]}
  pair=dict(workload=workload,repeat=repeat,commonMetrics=common,image=dict(status='not_available'))
  try:
   from PIL import Image,ImageChops,ImageStat
   im1=Image.open(str(p/(a['name']+'.png'))).convert('RGB');im2=Image.open(str(p/(b['name']+'.png'))).convert('RGB')
   if im1.size!=im2.size:raise ValueError('image dimensions differ')
   d=ImageChops.difference(im1,im2);h=d.histogram();n=im1.width*im1.height
   pair['image']=dict(maeRgb=ImageStat.Stat(d).mean,maximumChannelDifference=max(x[1] for x in d.getextrema()),channelFractionOver4=sum(sum(h[c*256+5:(c+1)*256]) for c in range(3))/(3*n),status='metrics_only_not_quality_verdict')
  except (ImportError,OSError,ValueError) as e:pair['image']['reason']=str(e)
  pairs.append(pair)
(p/'summary.json').write_text(json.dumps(dict(runs=result,pairs=pairs),indent=2),encoding='utf-8')
lines=['# Baseline vs final','', '|Workload|Repeat|CPU wall delta ms|GPU frame delta ms|','|---|---:|---:|---:|']
for x in pairs:lines.append('|{}|{}|{:.6f}|{:.6f}|'.format(x['workload'],x['repeat'],x['commonMetrics']['cpuWallFrameMs']['delta'],x['commonMetrics']['gpuFrameSpanMs']['delta']))
lines+=['','Negative delta = lower measured time; no automatic quality/speedup verdict. Old missing counters are not available, not zero.','Full run distributions, uploads/draws/resources and image metrics: summary.json.']
(p/'summary.md').write_text('\n'.join(lines),encoding='utf-8')
print('Offline report created.')
