"""CPU-only, hash-checked contact sheets; never grants visual acceptance."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import uuid

from PIL import Image, ImageChops, ImageDraw, ImageOps


def digest(path):
    value=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):value.update(block)
    return value.hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def write_json(path,value):
    Path(path).write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8')


def checked_image(record,root):
    path=Path(record['path']).resolve()
    if not path.is_relative_to(root) or not path.is_file():
        raise ValueError('Image missing or outside source evidence: '+str(path))
    actual=digest(path)
    if actual.lower()!=record['sha256'].lower():raise ValueError('Image hash mismatch: '+str(path))
    with Image.open(path) as source:
        if source.format!='PNG':raise ValueError('Expected original PNG: '+str(path))
        result=source.convert('RGB')
    return result,dict(path=str(path),sha256=actual,size=list(result.size))


def difference(image,reference):
    if image.size!=reference.size:
        return dict(status='UNAVAILABLE',reason='Original dimensions differ; not resampled')
    channels=ImageChops.difference(image,reference).split()
    maximum=ImageChops.lighter(ImageChops.lighter(channels[0],channels[1]),channels[2])
    histogram=maximum.histogram();total=image.width*image.height
    return dict(status='MEASURED',maxChannelDelta=max(i for i,count in enumerate(histogram) if count),
        thresholds={str(t):dict(changedPixels=sum(histogram[t+1:]),fraction=sum(histogram[t+1:])/total,
            bounds=maximum.point(lambda p:255 if p>t else 0).getbbox()) for t in (4,16)},
        scope='Final RGB image difference, including background and postprocessing; not particle count or fragment overdraw')


def save_sheet(path,tiles):
    sheet=Image.new('RGB',(1280,404*((len(tiles)+1)//2)),(24,24,28))
    draw=ImageDraw.Draw(sheet)
    for index,(image,label) in enumerate(tiles):
        x=(index%2)*640;y=(index//2)*404
        preview=ImageOps.contain(image,(640,360),Image.Resampling.LANCZOS)
        sheet.paste(preview,(x+(640-preview.width)//2,y+44+(360-preview.height)//2))
        draw.multiline_text((x+6,y+4),label,fill='white',spacing=3)
    # Bound disk preview before any visual inspection. All originals stay intact.
    for quality in (85,75,65,50,35,20):
        sheet.save(path,'JPEG',quality=quality,optimize=True)
        if path.stat().st_size<=500*1024:return
    raise RuntimeError('Contact sheet exceeds 500 KiB: '+str(path))


def review(source,output=None):
    source=Path(source).resolve();root=source.parent
    manifest=read_json(source)
    output=Path(output).resolve() if output else root/('review-'+uuid.uuid4().hex[:12])
    if not output.is_relative_to(root) or output==root or output.exists():
        raise ValueError('Output must be a new child directory of source evidence')
    output.mkdir(parents=True)
    report=dict(visualStatus='NOT_REVIEWED',source=dict(path=str(source),sha256=digest(source)),
        backend=manifest.get('backend','unknown'),effects=[],
        scope='Every stored capture, empty reference and retired reference; contact sheets aid human review, pixel differences never assign visual PASS')
    try:
        for group_index,group in enumerate(manifest['groups']):
            empty,empty_info=checked_image(group['empty'],root)
            for effect_index,effect in enumerate(group['effects']):
                summary=effect
                if 'report' in summary:
                    effect_path=Path(summary['report']).resolve()
                elif summary.get('captures'):
                    effect_path=Path(summary['captures'][0]['path']).resolve().parent/'report.json'
                else:raise ValueError('Effect has no report or captures: '+summary.get('id','unknown'))
                if not effect_path.is_relative_to(root):raise ValueError('Effect report outside evidence')
                stored=read_json(effect_path)
                # Require exact original captures/retirement, while allowing root
                # report to contain additional driver bookkeeping.
                for key in ('id','captures','retired'):
                    if key in summary and stored.get(key)!=summary[key]:raise ValueError('Root/effect report mismatch: '+str(effect_path))
                effect=stored
                if not effect.get('captures'):raise ValueError('Effect has no captures: '+effect.get('id','unknown'))
                retired,retired_info=checked_image(effect['retired'],root)
                name=re.sub(r'[^A-Za-z0-9_-]','_',effect['id'])[:80]
                destination=output/f'{group_index:02d}-{effect_index:02d}-{name}';destination.mkdir()
                item=dict(id=effect['id'],scene=group['scene'],simulation=group.get('simulation'),
                    backend=report['backend'],visualStatus='NOT_REVIEWED',
                    sourceReport=dict(path=str(effect_path),sha256=digest(effect_path)),images=[],sheets=[])
                report['effects'].append(item)
                records=[('empty',None,empty,empty_info)]
                for capture in effect['captures']:
                    image,identity=checked_image(capture,root)
                    records.append(('+'.join(capture.get('stages',[])) or 'capture',capture.get('frame'),image,identity))
                records.append(('after-stop-retired',None,retired,retired_info))
                for label,frame,image,identity in records:
                    item['images'].append(dict(stage=label,frame=frame,**identity,
                        vsEmpty=difference(image,empty),vsRetired=difference(image,retired)))
                for start in range(0,len(records),4):
                    tiles=[]
                    for label,frame,image,identity in records[start:start+4]:
                        heading=f"{report['backend']} | {effect['id']} | {Path(group['scene']).stem}"
                        tiles.append((image,heading[:100]+'\n'+label+' | frame='+str(frame if frame is not None else 'reference')))
                    sheet=destination/f'page-{start//4+1:02d}.jpg';save_sheet(sheet,tiles)
                    item['sheets'].append(dict(path=str(sheet),sha256=digest(sheet),imageIndices=list(range(start,min(start+4,len(records))))))
                write_json(destination/'review.json',item)
        report['preparationStatus']='COMPLETE'
    except Exception as error:
        report.update(preparationStatus='FAIL',error=type(error).__name__+': '+str(error))
        raise
    finally:
        write_json(output/'review.json',report)
    return output,report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report',type=Path,help='Existing template_matrix root report.json')
    parser.add_argument('--output',type=Path,help='New child directory; defaults to review-<unique id>')
    args=parser.parse_args()
    output,result=review(args.report,args.output)
    print(json.dumps(dict(output=str(output),effects=len(result['effects']),
        sheets=sum(len(e['sheets']) for e in result['effects']),visualStatus='NOT_REVIEWED')))
