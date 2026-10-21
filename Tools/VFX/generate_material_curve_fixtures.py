"""Original portable material and external NURBS fixtures; official 1.80.6 effect compiler."""
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import authoring as a

ROOT = a.ROOT
OUT = ROOT/'Project/SourceAssets/VFXAuthoring/MaterialCurveValidation'
ASSETS = ROOT/'Project/Assets/Effects/MaterialCurveValidation'
LICENSE = b'Original HEngine diagnostic material code, curve control points and effect composition.\nDedicated to the public domain under CC0 1.0: https://creativecommons.org/publicdomain/zero/1.0/\nNo third-party artwork or sample material is incorporated.\n'


def curve_bytes():
    # CurveLoader.cpp: version, count, double4 points, knot count, doubles,
    # degree (SDK calls this order), step, type, dimension. Degree-one line.
    points = [(-1., .3, 0., 1.), (1., .3, 0., 1.)]
    knots = [0., 0., 1., 1.]
    data = struct.pack('<ii', 1, len(points))
    data += b''.join(struct.pack('<4d', *p) for p in points)
    data += struct.pack('<i4d4i', len(knots), *knots, 1, 32, 0, 3)
    assert len(data) == 124
    assert struct.unpack_from('<4d', data, 8) == points[0]
    assert struct.unpack_from('<i4d4i', data, 72) == (4, *knots, 1, 32, 0, 3)
    return data


def main():
    editor = ROOT/'Engine/Tools/Effekseer/bin/Effekseer.exe'
    core = editor.with_name('EffekseerCore.dll')
    reports = []
    with tempfile.TemporaryDirectory(prefix='he-material-curve-') as temporary:
        stage = Path(temporary)
        material = stage/'OriginalCyan.efkmat'
        subprocess.run(['pwsh', '-NoProfile', '-File', str(a.RECIPES/'CreateDiagnosticMaterial.ps1'),
                        '-OutputFile', str(material), '-CoreAssembly', str(core)], check=True)
        for name in ('OriginalMaterial', 'OriginalCurve'):
            is_curve = name == 'OriginalCurve'
            dependency = 'OriginalLine.efkcurve' if is_curve else material.name
            deps = {dependency: curve_bytes() if is_curve else material.read_bytes()}
            for path, data in deps.items(): (stage/path).write_bytes(data)
            node = a.toon.make_sprite(name, '', (255,255,255,255), (.45,.45,.45), (.45,.45,.45),
                                      180, max_generation=1, fade_in=1, fade_out=1, alpha_blend=1)
            expected = {'RendererCommonValues/ZTest':'True', 'RendererCommonValues/ZWrite':'False'}
            if is_curve:
                expected.update({'LocationValues/Type':4, 'LocationValues/NurbsCurve/FilePath':dependency,
                                 'LocationValues/NurbsCurve/Scale':1, 'LocationValues/NurbsCurve/MoveSpeed':.02,
                                 'LocationValues/NurbsCurve/LoopType':1})
            else:
                expected.update({'RendererCommonValues/Material':128, 'RendererCommonValues/MaterialFile/Path':dependency})
            for path, value in expected.items(): a.set_xml(node, path, value)
            for channel in 'RGBA': a.set_xml(node, 'DrawingValues/ColorAll/Fixed/'+channel, 255)
            a.set_xml(node, 'DrawingValues/ColorAll/Type', 0)
            project = ET.Element('EffekseerProject'); root = ET.SubElement(project, 'Root')
            a.set_xml(root, 'Name', 'Root'); ET.SubElement(root, 'Children').append(node)
            for path, value in [('ToolVersion','1.80.6'),('Version',3),('StartFrame',0),('EndFrame',180),
                                ('IsLoop','False'),('Global/RandomSeed',1806)]: a.set_xml(project,path,value)
            payload = b'<?xml version="1.0" encoding="utf-8"?>\n'+ET.tostring(project,encoding='utf-8')
            source = stage/(name+'.efkproj'); binary = stage/(name+'.efkefc'); source.write_bytes(payload)
            a.toon.compile_effect(editor, source, binary)
            decoded = stage/(name+'-roundtrip.xml')
            subprocess.run(['pwsh','-NoProfile','-File',str(a.RECIPES/'InspectCompiled.ps1'),
                            '-InputFile',str(binary),'-OutputFile',str(decoded),'-CoreAssembly',str(core)],check=True)
            actual = ET.parse(decoded).getroot().find('Root/Children/Node')
            for path,value in expected.items():
                defaults = {'RendererCommonValues/ZTest':'True', 'RendererCommonValues/ZWrite':'False',
                            'LocationValues/NurbsCurve/Scale':'1'}
                got = actual.findtext(path, defaults.get(path, '0'))
                if isinstance(value,(int,float)): assert abs(float(got)-value)<1e-6,(path,got,value)
                else: assert got==value,(path,got,value)
            data = binary.read_bytes(); chunks = dict(a.stress.parse_chunks(data))
            assert dependency.encode('utf-16-le') in chunks[b'INFO'], 'Official dependency slot missing'
            assert dependency.encode('utf-16-le') in chunks[b'BIN_'], 'Runtime dependency slot missing'
            report = dict(fixture=name, license='CC0-1.0', author='HEngine diagnostic generator',
                dependencies={p:a.sha(b) for p,b in deps.items()}, parameters=expected,
                sourceSha256=a.sha(payload), binarySha256=a.sha(data),
                compilerSha256={p.name:a.sha(p.read_bytes()) for p in (editor,core)},
                validation=dict(officialCompile='PASS',officialEditReadback='PASS',
                    materialClrReadback='PASS' if not is_curve else 'N/A',
                    curveStructuralReadback='PASS' if is_curve else 'N/A',DX12='NOT_RUN',Vulkan='NOT_RUN'),
                compiledMaterial='NOT_PROVIDED: no platform binary is authored or synthesized')
            entries={name+'.efkefc':data,**deps,'LICENSE.txt':LICENSE,'PROVENANCE.json':a.encoded(report)}
            folder=OUT/name;folder.mkdir(parents=True,exist_ok=True)
            for path, blob in {**entries,name+'.efkproj':payload,'roundtrip.xml':decoded.read_bytes()}.items():
                (folder/path).write_bytes(blob)
            ASSETS.mkdir(parents=True,exist_ok=True)
            package=a.toon.deterministic_zip(entries);(ASSETS/(name+'.efkpkg')).write_bytes(package)
            report['packageSha256']=a.sha(package);reports.append(report)
    (OUT/'report.json').write_bytes(a.encoded(reports))
    print(json.dumps(reports))


if __name__ == '__main__': main()
