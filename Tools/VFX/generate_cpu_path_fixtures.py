"""Official-compile minimal CPU sprite/soft/distortion/model path fixtures; no GPU run."""
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET

import authoring as a

ROOT = a.ROOT
INTAKE = ROOT/'Project/SourceAssets/EffekseerInbox/effekseer01-official'
OUT = ROOT/'Project/SourceAssets/VFXAuthoring/AuthoringPathValidation'
ASSETS = ROOT/'Project/Assets/Effects/AuthoringPathValidation'


def main():
    manifest=json.loads((INTAKE/'asset.json').read_text())
    if manifest['license_identifier']!='CC0-1.0' or manifest['license_decision']!='ACCEPT':
        raise RuntimeError('Reviewed model/texture intake license changed')
    license_data=(INTAKE/'LICENSE.txt').read_bytes()
    source=INTAKE/'source/Effekseer01'
    editor=ROOT/'Engine/Tools/Effekseer/bin/Effekseer.exe'
    core=editor.with_name('EffekseerCore.dll')
    reports=[]
    for name in ('CpuSpriteControl','CpuSoftParticle','CpuDistortion','CpuModel'):
        deps={'Texture/Particle04_clear_hard.png':(source/'Texture/Particle04_clear_hard.png').read_bytes()}
        texture='Texture/Particle04_clear_hard.png'
        if name=='CpuModel':
            texture='Texture/Arrow.png'
            deps={texture:(source/texture).read_bytes(), 'Model/Arrow.efkmodel':(source/'Model/Arrow.efkmodel').read_bytes()}
        # Arrow's actual local XY bounds are [-2,2], not a unit sprite.
        # Keep its 1.2 m square above the ground and inside the unchanged camera.
        scale=(.3,.3,.3) if name=='CpuModel' else (2,2,2)
        location=(0,.65,0) if name=='CpuModel' else (0.0,0.0,0.0)
        node=a.toon.make_sprite(name,texture,(255,255,255,255),scale,scale,180,
                                max_generation=1,fade_in=1,fade_out=1,alpha_blend=1,location=location)
        # Current 1.80.6 shared renderer color, not the legacy Sprite-only field.
        for c in 'RGBA': a.set_xml(node,'DrawingValues/ColorAll/Fixed/'+c,255)
        a.set_xml(node,'DrawingValues/ColorAll/Type',0)
        a.set_xml(node,'RendererCommonValues/ZTest','True')
        a.set_xml(node,'RendererCommonValues/ZWrite','False')
        expected={'DrawingValues/Type':2,'RendererCommonValues/Material':0}
        if name=='CpuSoftParticle':
            expected.update({'AdvancedRendererCommonValuesValues/SoftParticleParams/Enabled':'True',
                'AdvancedRendererCommonValuesValues/SoftParticleParams/Distance':.375,
                'AdvancedRendererCommonValuesValues/SoftParticleParams/DistanceNear':.0625,
                'AdvancedRendererCommonValuesValues/SoftParticleParams/DistanceNearOffset':.03125})
        if name=='CpuDistortion':
            expected.update({'RendererCommonValues/Material':6,'RendererCommonValues/DistortionIntensity':1.25})
        if name=='CpuModel':
            node.remove(node.find('DrawingValues'))
            expected.update({'DrawingValues/Type':5,'DrawingValues/Model/Model':'Model/Arrow.efkmodel',
                             'DrawingValues/Model/Lighting':'False','DrawingValues/Model/Culling':2})
            for axis in 'XYZ':
                for end in ('Start','End'):
                    expected['ScalingValues/Easing/'+end+'/'+axis+'/Center']=.3
                expected['LocationValues/PVA/Location/'+axis+'/Center']=.65 if axis=='Y' else 0
        for path,value in expected.items(): a.set_xml(node,path,value)
        project=ET.Element('EffekseerProject');root=ET.SubElement(project,'Root')
        a.set_xml(root,'Name','Root'); ET.SubElement(root,'Children').append(node)
        for path,value in [('ToolVersion','1.80.6'),('Version',3),('StartFrame',0),('EndFrame',180),
                           ('IsLoop','False'),('Global/RandomSeed',1806)]: a.set_xml(project,path,value)
        payload=b'<?xml version="1.0" encoding="utf-8"?>\n'+ET.tostring(project,encoding='utf-8')
        with tempfile.TemporaryDirectory(prefix='he-cpu-path-') as temporary:
            stage=Path(temporary); project_path=stage/(name+'.efkproj'); binary_path=stage/(name+'.efkefc')
            project_path.write_bytes(payload)
            for path,data in deps.items():
                target=stage/path; target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
            a.toon.compile_effect(editor,project_path,binary_path)
            decoded=stage/'roundtrip.xml'
            subprocess.run(['pwsh','-NoProfile','-File',str(a.RECIPES/'InspectCompiled.ps1'),
                '-InputFile',str(binary_path),'-OutputFile',str(decoded),'-CoreAssembly',str(core)],check=True)
            actual=ET.parse(decoded).getroot().find('Root/Children/Node')
            for path,value in expected.items():
                # Defaults are omitted by the official EDIT serializer.
                default='2' if path=='DrawingValues/Type' else 'False' if value=='False' else '0'
                got=actual.findtext(path,default)
                if type(value) in (int,float): assert abs(float(got)-value)<1e-6,(path,got,value)
                else: assert got==value,(path,got,value)
            assert actual.findtext('GpuParticles/Enabled','False')=='False'
            data=binary_path.read_bytes(); chunks=dict(a.stress.parse_chunks(data)); runtime=chunks[b'BIN_']
            evidence={'compiled_EDIT':'PASS','compiled_BIN_present':bool(runtime)}
            if name=='CpuSoftParticle':
                encoded=struct.pack('<fff',.375,.0625,.03125)
                assert runtime.count(encoded)==1
                evidence['runtime_soft_parameter_offset']=runtime.index(encoded)
            # INFO is emitted by the official compiler and contains dependency names.
            info=chunks[b'INFO']
            for path in deps:
                assert path.encode('utf-16-le') in info,(name,'missing compiled dependency',path)
            report={'fixture':name,'simulation':'cpu','nodes':1,'lifetimeFrames':180,'parameters':expected,
                    'dependencies':{path:a.sha(blob) for path,blob in deps.items()},
                    'license':'CC0-1.0','author':'HEngine diagnostic generator; Effekseer model/texture',
                    'source_manifest':str((INTAKE/'asset.json').relative_to(ROOT)),
                    'source_url':manifest['source_url'],'license_evidence_sha256':a.sha(license_data),
                    'source_sha256':a.sha(payload),'binary_sha256':a.sha(data),
                    'compiler_sha256':{p.name:a.sha(p.read_bytes()) for p in (editor,core)},
                    'validation':dict(evidence,official_compile='PASS',DX12='NOT_RUN',Vulkan='NOT_RUN'),
                    'requiredGraphInputs':['SceneDepth snapshot'] if name=='CpuSoftParticle' else
                        ['SceneColor background snapshot'] if name=='CpuDistortion' else []}
            entries={name+'.efkefc':data,'PROVENANCE.json':a.encoded(report),'LICENSE.txt':license_data,**deps}
            folder=OUT/name;folder.mkdir(parents=True,exist_ok=True)
            for path,blob in dict(entries,**{name+'.efkproj':payload,'roundtrip.xml':decoded.read_bytes()}).items():
                target=folder/path;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(blob)
            ASSETS.mkdir(parents=True,exist_ok=True)
            package=a.toon.deterministic_zip(entries); (ASSETS/(name+'.efkpkg')).write_bytes(package)
            report['package_sha256']=a.sha(package); reports.append(report)
    (OUT/'report.json').write_bytes(a.encoded(reports))
    print(json.dumps([dict(fixture=r['fixture'],sha256=r['package_sha256'],validation=r['validation']) for r in reports]))


if __name__=='__main__': main()
