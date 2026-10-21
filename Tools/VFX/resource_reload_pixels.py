"""Pinned same-path texture reload/cross-package isolation image probe; no builds."""
import argparse
import io
import json
import os
from pathlib import Path
import shutil
import uuid
import zipfile

import numpy as np
from PIL import Image
from preview import ROOT, Session, Storage, digest, write_json


def colored_package(source, color):
    with zipfile.ZipFile(source) as package: entries={n:package.read(n) for n in package.namelist()}
    names=[n for n in entries if n.lower().endswith('.png')];assert len(names)==1
    with Image.open(io.BytesIO(entries[names[0]])) as image:
        rgba=image.convert('RGBA');alpha=rgba.getchannel('A')
        rgba.paste(tuple(color)+(255,),[0,0,rgba.width,rgba.height]);rgba.putalpha(alpha)
        encoded=io.BytesIO();rgba.save(encoded,format='PNG');entries[names[0]]=encoded.getvalue()
    result=io.BytesIO()
    with zipfile.ZipFile(result,'w',zipfile.ZIP_DEFLATED) as package:
        for name,data in sorted(entries.items()):package.writestr(name,data)
    return result.getvalue(),names[0]


def metrics(pixels,background,roi):
    x0,y0,x1,y1=roi;a=pixels[y0:y1,x0:x1];b=background[y0:y1,x0:x1]
    mask=np.max(np.abs(a-b),axis=2)>20
    samples=a[mask];counts=[]
    for channel in range(3):
        counts.append(int(((samples[:,channel]>40)&
            (samples[:,channel]-np.max(samples[:,[i for i in range(3) if i!=channel]],axis=1)>15)).sum()))
    return dict(signalPixels=int(mask.sum()),dominantRGB=counts,
                dominantFractions=[v/max(1,int(mask.sum())) for v in counts])


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--backend',choices=['DX12','Vulkan'],required=True)
    p.add_argument('--existing-build-sha256',required=True);p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();exe=ROOT/'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe';pin=digest(exe)
    if pin.lower()!=args.existing_build_sha256.lower():p.error('Candidate hash mismatch')
    source=ROOT/'Project/Assets/Effects/AuthoringPathValidation/CpuSpriteControl.efkpkg';source_hash=digest(source)
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    scratch=ROOT/'Project/Assets/Effects/ResourceReloadPixels'/uuid.uuid4().hex;scratch.mkdir(parents=True)
    a=scratch/'a.efkpkg';b=scratch/'b.efkpkg';red,texture=colored_package(source,(255,0,0))
    green,_=colored_package(source,(0,255,0));blue,_=colored_package(source,(0,0,255));a.write_bytes(red);b.write_bytes(blue)
    report=dict(commandStatus='RUNNING',visualStatus='REQUIRES_REVIEW',checks=[],captures=[],
        evidenceType='runtime-manual',backend=args.backend,executableSha256=pin,sourceSha256=source_hash,
        sharedInternalPath=texture,thresholds=dict(delta=20,channelMin=40,dominanceMargin=15,
        minPixels=50,dominantFraction=.7,oldRoiMae=2),scope='Actual image color changes, not particle-state hashes')
    session=Session(out,args.backend,pin);handles=set()
    call=lambda method,**params:session.call(method,params)
    def check(name,ok,**values):
        report['checks'].append(dict(name=name,status='PASS' if ok else 'FAIL',**values));write_json(out/'report.json',report)
    def capture(name):
        receipt=call('capture.frame',filename=name+'.png');path=out/(name+'.png');shutil.copyfile(receipt['path'],path)
        write_json(out/(name+'-stats.json'),call('vfx.stats'))
        report['captures'].append(dict(name=name,path=str(path),sha256=digest(path)))
        with Image.open(path) as im:return np.asarray(im.convert('RGB'),dtype=np.int16)
    def play(path,x):
        response=call('vfx.play',path=path.relative_to(ROOT/'Project').as_posix(),seed=1806,position=[x,0,0])
        h=response['handle'];handles.add(h)
        if response.get('playing') is not True:raise RuntimeError('Effect failed to start')
        call('runtime.step',frames=8);call('vfx.control',handle=h,action='pause');return h
    saved=os.environ.get('HE_VFX_GPU_PARTICLES');os.environ['HE_VFX_GPU_PARTICLES']='0'
    with Storage(ROOT).workflow_scope(str(ROOT/'Project'),'vfx-resource-reload-pixels',[out]) as run:
        try:
            try:
                session.start();call('runtime.pause');call('runtime.step',frames=30)
                bg=capture('empty');old=play(a,-1.4);first=capture('a-red-old')
                signal=np.max(np.abs(first-bg),axis=2)>20;y,x=np.nonzero(signal)
                if len(x)<50:raise RuntimeError('No useful old-instance image signal')
                # Infer which screen half contains x=-1.4; keep views separated.
                h,w=signal.shape;left=(0,0,w//2,h);right=(w//2,0,w,h)
                old_roi=left if float(x.mean())<w/2 else right;new_roi=right if old_roi==left else left
                report['rois']=dict(old=old_roi,new=new_roi)
                m=metrics(first,bg,old_roi);check('initial_red',m['signalPixels']>=50 and m['dominantFractions'][0]>=.7,metrics=m)
                a.write_bytes(green);result=call('vfx.reload',path=a.relative_to(ROOT/'Project').as_posix())
                check('same_key_reload_accepted',result.get('accepted') is True,receipt=result)
                fresh=play(a,1.4);second=capture('a-old-red-new-green')
                for label,pix in [('reload',second)]:
                    old_m=metrics(pix,bg,old_roi);new_m=metrics(pix,bg,new_roi)
                    check(label+'.old_red_new_green',old_m['signalPixels']>=50 and old_m['dominantFractions'][0]>=.7
                          and new_m['signalPixels']>=50 and new_m['dominantFractions'][1]>=.7,old=old_m,new=new_m)
                call('vfx.control',handle=fresh,action='stop');handles.discard(fresh);call('runtime.step',frames=30)
                other=play(b,1.4);third=capture('a-old-red-b-blue')
                old_m=metrics(third,bg,old_roi);new_m=metrics(third,bg,new_roi)
                check('two_packages_same_path_isolated',old_m['signalPixels']>=50 and old_m['dominantFractions'][0]>=.7
                      and new_m['signalPixels']>=50 and new_m['dominantFractions'][2]>=.7,old=old_m,other=new_m)
                x0,y0,x1,y1=old_roi
                for label,pix in [('reload',second),('other_package',third)]:
                    mae=float(np.abs(first[y0:y1,x0:x1]-pix[y0:y1,x0:x1]).mean())
                    check(label+'.old_image_retained',mae<=2,roiMae=mae,state=call('vfx.control',handle=old,action='status'))
            finally:
                try:
                    for handle in handles:call('vfx.control',handle=handle,action='stop')
                    call('runtime.step',frames=30);report['finalStats']=call('vfx.stats')
                finally:
                    try:session.stop()
                    except Exception as error:report['cleanupError']=str(error);raise
            report['commandStatus']='FAIL' if any(c['status']=='FAIL' for c in report['checks']) else 'PASS'
            run['status']='success' if report['commandStatus']=='PASS' else 'failed'
        except Exception as error:
            report['commandStatus']='FAIL';report['error']=str(error);run['status']='unknown' if report.get('cleanupError') else 'failed'
        finally:
            if saved is None:os.environ.pop('HE_VFX_GPU_PARTICLES',None)
            else:os.environ['HE_VFX_GPU_PARTICLES']=saved
            if not report.get('cleanupError'):
                a.unlink(missing_ok=True);b.unlink(missing_ok=True);scratch.rmdir()
            report['inputsUnchanged']=digest(source)==source_hash and digest(exe)==pin
            if not report['inputsUnchanged']:report['commandStatus']='FAIL';run['status']='failed'
            write_json(out/'report.json',report)
    print(json.dumps(dict(commandStatus=report['commandStatus'],visualStatus=report['visualStatus'],report=str(out/'report.json'))))
    return 0 if report['commandStatus']=='PASS' else 1


if __name__=='__main__':raise SystemExit(main())
