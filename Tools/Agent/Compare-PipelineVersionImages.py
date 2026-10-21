"""Offline version-image comparison. Does not launch the engine or declare quality PASS.
Requires Pillow. Run only after GPU performance sampling has stopped.
"""
import argparse, hashlib, json, math, pathlib, statistics
from PIL import Image, ImageChops, ImageStat

def read(path):
 return json.loads(path.read_text(encoding='utf-8-sig')) if path.is_file() else None

def sha(path):
 h=hashlib.sha256()
 with path.open('rb') as f:
  for b in iter(lambda:f.read(1048576),b''):h.update(b)
 return h.hexdigest()

def distribution(values):
 s=sorted(values)
 def q(t):
  n=(len(s)-1)*t;a=int(n);b=min(a+1,len(s)-1);return s[a]+(s[b]-s[a])*(n-a)
 return dict(mean=statistics.mean(s),std=statistics.pstdev(s),median=q(.5),p95=q(.95),p99=q(.99),max=s[-1])

def roi_stats(im, box):
 pixels=list(im.crop(box).getdata());channels=list(zip(*pixels))
 y=[.2126*r+.7152*g+.0722*b for r,g,b in pixels]
 return dict(bounds=list(box),pixelCount=len(pixels),rgb=[distribution(c) for c in channels],displayEncodedLuma=distribution(y),lumaFractionOver160=sum(v>160 for v in y)/len(y),note='Display-encoded RGB 0..255; not scene-linear radiance, no truth/reference error claim.')

def preview(im,path):
 im=im.copy();im.thumbnail((1280,720))
 for quality in [88,82,74,64,52]:
  im.save(str(path),quality=quality,optimize=True)
  if path.stat().st_size<=500*1024:break
 if path.stat().st_size>500*1024:raise ValueError('Preview over 500 KiB')
 return dict(path=str(path.resolve()),width=im.width,height=im.height,bytes=path.stat().st_size)

def diff_images(a,b):
 if a.size!=b.size:raise ValueError('Different image dimensions')
 d=ImageChops.difference(a,b);h=d.histogram();n=a.width*a.height
 maxchannel=ImageChops.lighter(ImageChops.lighter(d.getchannel('R'),d.getchannel('G')),d.getchannel('B'))
 return dict(maeRgb=ImageStat.Stat(d).mean,maeMean=sum(ImageStat.Stat(d).mean)/3,maximumChannelDifference=max(v[1] for v in d.getextrema()),channelFractionOver4=sum(sum(h[k*256+5:(k+1)*256]) for k in range(3))/(3*n),pixelFractionOver4=sum(maxchannel.histogram()[5:])/n,units='absolute display encoded channel values, range 0..255')

def settings_diff(a,b):
 result={}
 for section in ['effective','debug']:
  x=(a or {}).get(section,{}) or {};y=(b or {}).get(section,{}) or {}
  common=set(x)&set(y)
  result[section]=dict(commonCount=len(common),onlyBaseline=sorted(set(x)-set(y)),onlyFinal=sorted(set(y)-set(x)),differences=[dict(key=k,baseline=x[k],final=y[k]) for k in sorted(common) if x[k]!=y[k]])
 return result

def counters(observation):
 if not observation:return dict(status='not_available')
 result={'requestedRenderDelay':observation.get('requestedRenderDelay')}
 for stage in ['before','after']:
  x=observation.get(stage,{}) or {};c=x.get('world',{}).get('runtimeControl',{}) or {}
  result[stage]={k:x.get(k) for k in ['renderFrame','completedRenderFrames','simulationTimeSeconds','sceneGeneration']}
  result[stage]['runtimeControl']={k:c.get(k) for k in ['completedSteps','pendingSteps','stepInProgress','paused']}
 capture=observation.get('capture',{}) or {}
 result['capture']=capture
 # Capture responses contain useful frame scope plus path; retain verbatim archive separately.
 result['latest']=(observation.get('after',{}).get('capture',{}) or {}).get('latest')
 return result

def numeric_delta(a,b):
 if isinstance(a,dict) and isinstance(b,dict):return {k:numeric_delta(a[k],b[k]) for k in a.keys()&b.keys()}
 if isinstance(a,(int,float)) and not isinstance(a,bool) and isinstance(b,(int,float)) and not isinstance(b,bool):return b-a
 return None

def main():
 ap=argparse.ArgumentParser();ap.add_argument('--baseline',required=True,type=pathlib.Path);ap.add_argument('--final',required=True,type=pathlib.Path);ap.add_argument('--output',required=True,type=pathlib.Path);args=ap.parse_args()
 if args.output.exists():raise ValueError('Use a new output directory')
 args.output.mkdir(parents=True);out=args.output;items=[]
 groups=[('surface-motion','pose-'+str(i),False) for i in range(8)]+[('ssgi-{}-{}'.format(r,f),'endpoint',True) for r in [1,8] for f in [128,512]]
 for case,name,is_ssgi in groups:
  label=case+'-'+name;entry=dict(pair=label,case=case,endpoint=name,status='not_available')
  dirs={'baseline':args.baseline/('baseline-'+case),'final':args.final/('final-'+case)}
  paths={s:d/(name+'.png') for s,d in dirs.items()}
  entry['sourceImages']={s:str(p.resolve()) for s,p in paths.items()}
  missing=[s for s,p in paths.items() if not p.is_file()]
  if missing:entry['reason']='Missing image: '+','.join(missing);items.append(entry);continue
  images={s:Image.open(str(p)).convert('RGB') for s,p in paths.items()}
  try:
   entry['pixels']=diff_images(images['baseline'],images['final']);entry['imageSha256']={s:sha(p) for s,p in paths.items()}
   entry['previews']={s:preview(im,out/(label+'-'+s+'.jpg')) for s,im in images.items()}
   if is_ssgi:
    if images['baseline'].size!=(1280,720):raise ValueError('Fixed SSGI ROI requires original 1280x720 image')
    entry['roi']={s:{k:roi_stats(im,box) for k,box in [('wall',(470,240,800,300)),('floor',(350,420,900,460))]} for s,im in images.items()}
   observations={s:read(d/(name+'-observation.json')) for s,d in dirs.items()}
   entry['counters']={s:counters(v) for s,v in observations.items()};entry['numericCounterDeltaFinalMinusBaseline']=numeric_delta(entry['counters']['baseline'],entry['counters']['final'])
   settings={s:read(d/'settings-observed.json') for s,d in dirs.items()}
   entry['settings']=settings_diff(settings['baseline'],settings['final']);entry['settingsAvailable']={s:v is not None for s,v in settings.items()}
   archive=out/(label+'-context.json');archive.write_text(json.dumps(dict(observations=observations,settings=settings),ensure_ascii=False,indent=2),encoding='utf-8');entry['contextArchive']=str(archive.resolve())
   entry['status']='metrics_available_not_quality_verdict'
  except (ValueError,OSError) as e:entry['reason']=str(e)
  finally:
   for im in images.values():im.close()
  items.append(entry)
 notes=['Eight settled camera poses are not an atomic consecutive GPU-frame sequence.','SSGI 128/512 labels denote requested rendered-frame delay after capture request, not equal accepted history age or equal renderFrame.','No truth image: differences and ROI variation describe observed amplitude, not error or regression proof.','Shared settings differences, missing settings and frame counters must be reviewed before attributing image changes to version.','JPEG previews only for sequential visual review; all metrics use original PNG.','No automatic PASS/FAIL for image quality. Missing images/evidence remain not_available.']
 (out/'summary.json').write_text(json.dumps(dict(baselineRuns=read(args.baseline/'runs.json'),finalRuns=read(args.final/'runs.json'),pairs=items,notes=notes),ensure_ascii=False,indent=2),encoding='utf-8')
 lines=['# Pipeline version image comparison','', '| Pair | MAE RGB mean | max channel | pixels >4 | status |','|---|---:|---:|---:|---|']
 for x in items:
  m=x.get('pixels');lines.append('|{}|{}|{}|{}|{}|'.format(x['pair'],'{:.6f}'.format(m['maeMean']) if m else 'N/A',m['maximumChannelDifference'] if m else 'N/A','{:.5%}'.format(m['pixelFractionOver4']) if m else 'N/A',x['status']))
 lines+=['','## SSGI wall ROI','', '|pair|side|mean Y|std Y|p95 Y|Y >160|','|---|---|---:|---:|---:|---:|']
 for x in items:
  for side,rois in x.get('roi',{}).items():
   m=rois['wall'];y=m['displayEncodedLuma'];lines.append('|{}|{}|{:.4f}|{:.4f}|{:.4f}|{:.5%}|'.format(x['pair'],side,y['mean'],y['std'],y['p95'],m['lumaFractionOver160']))
 lines+=['']+notes;(out/'summary.md').write_text('\n'.join(lines),encoding='utf-8')
 print('Offline comparison written; inspect previews sequentially and review settings/counter differences.')
if __name__=='__main__':main()
