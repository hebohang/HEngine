#!/usr/bin/env python3
"""Official Effekseer 1810 stress export: editable XML is the only authoring input.

Legacy 1800 stress packages are retained separately as negative compatibility
fixtures. No runtime binary patching participates in this generator.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import zipfile
import zlib
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple

SOURCE_EFFECT=Path("Engine/Source/ThirdParty/Effekseer/Examples/Resources/GpuParticles_sprite_mass.efkefc")
SOURCE_TEXTURE=Path("Engine/Source/ThirdParty/Effekseer/Examples/Resources/Texture/Particle01.png")
SOURCE_LICENSE=Path("Project/SourceAssets/Effekseer/STRESS_ASSET_LICENSE.md")
OUTPUT_DIRECTORY=Path("Project/Assets/Effects/Stress")
AUTHOR_DIRECTORY=Path("Project/SourceAssets/VFXAuthoring/Stress")
EXPECTED_EFFECT_SHA256="f4e94742a8c1b4dab03f4c427b07cb0a4a5650139b95f4fd6584e300326f79ce"
EXPECTED_TEXTURE_SHA256="b1346d2bfafa6e783661453680b282edc6bc0e0b85711414b9914aac03a3a881"
EXPECTED_LICENSE_SHA256="92c350f70cee73cb8dbe03a24a24212137e7da027a9e14b6a72cd6c6f674a6be"
COMPILER_HASHES={"Effekseer.exe":"cc2ea1f64d86b203ee9924c72252421996a848d2416ebbf8432abe9e4f7e30c6",
 "EffekseerCore.dll":"d8de02503e9e847b673bcf5201ebc8c977a033ea3689a089ca611d980f90b264"}
FIXED_RANDOM_SEED=1806
PARTICLE_LIFETIME_FRAMES=200.0
OWNER_LIFETIME_FRAMES=1200
TOTAL_EMIT_COUNT=2_000_000
RELEASE_VERSION="1.80.6"
RELEASE_URL="https://github.com/effekseer/Effekseer/releases/tag/1806"
GRADIENT_STOPS=((0.0,(255,235,150,220)),(.35,(255,132,38,205)),(.72,(214,54,20,145)),(1.0,(92,18,8,32)))
class GenerationError(RuntimeError): pass

def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + kind
        + payload
        + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    )

def build_spark_texture_png() -> bytes:
    """Build a small white sprite whose radial falloff lives in alpha.

    The official sample texture stores an opaque black background and relies on
    additive blending to hide it.  A real alpha falloff remains safe when a
    renderer or intermediate target uses a different blend/composite path.
    """

    size = 32
    rows = bytearray()
    center = (size - 1) * 0.5
    radius = center
    for y in range(size):
        rows.append(0)  # PNG filter: None
        for x in range(size):
            distance = math.sqrt((x - center) ** 2 + (y - center) ** 2) / radius
            alpha = max(0.0, 1.0 - distance)
            alpha = alpha * alpha * (3.0 - 2.0 * alpha)
            rows.extend((255, 255, 255, int(round(alpha * 255.0))))

    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib.compress(bytes(rows), level=9))
        + png_chunk(b"IEND", b"")
    )

def read_verified(path: Path, expected_sha256: str) -> bytes:
    if not path.is_file():
        raise GenerationError("required source file is missing: {0}".format(path))
    data = path.read_bytes()
    actual = sha256_bytes(data)
    if actual != expected_sha256:
        raise GenerationError(
            "source hash mismatch for {0}: expected {1}, got {2}".format(
                path, expected_sha256, actual
            )
        )
    return data

def parse_chunks(data: bytes) -> List[Tuple[bytes, bytes]]:
    if len(data) < 8 or data[:4] != b"EFKE":
        raise GenerationError("source is not an EFKE container")
    if struct.unpack_from("<I", data, 4)[0] != 0:
        raise GenerationError("unexpected EFKE container version")

    chunks: List[Tuple[bytes, bytes]] = []
    offset = 8
    while offset < len(data):
        if offset + 8 > len(data):
            raise GenerationError("truncated EFKE chunk header")
        tag = data[offset : offset + 4]
        size = struct.unpack_from("<I", data, offset + 4)[0]
        offset += 8
        end = offset + size
        if end > len(data):
            raise GenerationError("truncated EFKE chunk {0!r}".format(tag))
        chunks.append((tag, data[offset:end]))
        offset = end

    tags = [tag for tag, _ in chunks]
    if tags != [b"INFO", b"EDIT", b"BIN_"]:
        raise GenerationError("unexpected EFKE chunk sequence: {0!r}".format(tags))
    return chunks

def build_container(chunks: Iterable[Tuple[bytes, bytes]]) -> bytes:
    output = bytearray(b"EFKE")
    output.extend(struct.pack("<I", 0))
    for tag, payload in chunks:
        if len(tag) != 4:
            raise GenerationError("invalid EFKE chunk tag")
        output.extend(tag)
        output.extend(struct.pack("<I", len(payload)))
        output.extend(payload)
    return bytes(output)

def interpolate_channel(a: int, b: int, t: float) -> int:
    return max(0, min(255, int(round(a + (b - a) * t))))

def build_gradient() -> bytes:
    stops = (
        (0.00, (255, 235, 150, 220)),
        (0.35, (255, 132, 38, 205)),
        (0.72, (214, 54, 20, 145)),
        (1.00, (92, 18, 8, 32)),
    )
    pixels = bytearray()
    for index in range(32):
        t = index / 31.0
        lower = stops[0]
        upper = stops[-1]
        for candidate_lower, candidate_upper in zip(stops, stops[1:]):
            if candidate_lower[0] <= t <= candidate_upper[0]:
                lower = candidate_lower
                upper = candidate_upper
                break
        span = upper[0] - lower[0]
        local = 0.0 if span <= 0.0 else (t - lower[0]) / span
        pixels.extend(
            interpolate_channel(lower[1][channel], upper[1][channel], local)
            for channel in range(4)
        )
    if len(pixels) != 32 * 4:
        raise GenerationError("gradient construction failed")
    return bytes(pixels)

def deterministic_zip(entries: Dict[str, bytes]) -> bytes:
    from io import BytesIO

    stream = BytesIO()
    with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in sorted(entries):
            if name.startswith("/") or ".." in name.split("/") or "\\" in name:
                raise GenerationError("unsafe generated archive path: {0}".format(name))
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, entries[name])
    return stream.getvalue()


def set_xml(element, path, value):
    for part in path.split('/'):
        child=element.find(part)
        if child is None: child=ET.SubElement(element,part)
        element=child
    element.text=str(value)


def author_project(decoded, nominal):
    project=ET.fromstring(decoded)
    nodes=project.findall('Root/Children/Node')
    if len(nodes)!=1: raise GenerationError('Expected one original shared CPU/GPU sprite node')
    node=nodes[0]
    # Preserve the CPU sprite, transforms, rendering and owner emission settings.
    # Only owner lifetime changes; every GPU field formerly runtime-only is authored.
    for suffix in ('Center','Max','Min'): set_xml(node,'CommonValues/Life/'+suffix,OWNER_LIFETIME_FRAMES)
    expected={'GpuParticles/Enabled':'True','GpuParticles/Basic/EmitCount/Value':TOTAL_EMIT_COUNT,
        'GpuParticles/Basic/EmitPerFrame':nominal//200,'GpuParticles/Velocity/Spread':60,
        'GpuParticles/Scale/Type':2,'GpuParticles/Force/Gravity/Y':-.036,
        'GpuParticles/RenderBasic/BlendType':2,'GpuParticles/RenderShape/ShapeSize':.03,
        'GpuParticles/RenderColor/ColorInheritType':1,'GpuParticles/RenderColor/ColorAll/Type':4,
        'GpuParticles/RenderColor/Emissive':1,'GpuParticles/RenderColor/FadeIn':8,'GpuParticles/RenderColor/FadeOut':35}
    ranges={'Basic/LifeTime':(200,200),'Velocity/InitialSpeed':(.035,.10),'Velocity/Damping':(.01,.03),
            'Scale/Easing/InitialSingleScale':(.75,1.15),'Scale/Easing/TerminalSingleScale':(.15,.35)}
    for path,(minimum,maximum) in ranges.items():
        for suffix,value in (('Min',minimum),('Max',maximum),('Center',(minimum+maximum)/2)):
            expected['GpuParticles/'+path+'/'+suffix]=value
    for path,value in expected.items(): set_xml(node,path,value)
    gradient=node.find('GpuParticles/RenderColor/ColorAll/Gradient')
    gradient.clear()
    colors=ET.SubElement(gradient,'ColorMarkers');alphas=ET.SubElement(gradient,'AlphaMarkers')
    for position,rgba in GRADIENT_STOPS:
        color=ET.SubElement(colors,'Key');alpha=ET.SubElement(alphas,'Key')
        for channel,value in zip(('ColorR','ColorG','ColorB'),rgba): set_xml(color,channel,value/255)
        set_xml(color,'Position',position);set_xml(color,'Intensity',1)
        set_xml(alpha,'Position',position);set_xml(alpha,'Alpha',rgba[3]/255)
    set_xml(project,'ToolVersion',RELEASE_VERSION)
    set_xml(project,'Global/RandomSeed',FIXED_RANDOM_SEED)
    set_xml(project,'EndFrame',OWNER_LIFETIME_FRAMES)
    set_xml(project,'IsLoop','False')
    return b'<?xml version="1.0" encoding="utf-8"?>\n'+ET.tostring(project,encoding='utf-8'),expected


def validate_compiled_stress(payload, nominal):
    """Read actual official EFKE/BIN fields; no handwritten runtime payload output.

    This is a bounded validator for these two authored fixtures, not a general
    Effekseer parser. Unique field anchors and complete GPU-tail consumption are
    mandatory. Reject legacy 1800 even if its nominal counts happen to match.
    """
    if nominal not in (20000,50000): raise GenerationError('Unsupported nominal count')
    chunks=dict(parse_chunks(payload));data=chunks[b'BIN_']
    if data[:4]!=b'SKFE' or struct.unpack_from('<i',data,4)[0]!=1810:
        raise GenerationError('Official final Version18 (1810) required')
    def unique(pattern, end=None):
        found=[];start=0
        while True:
            at=data.find(pattern,start,len(data) if end is None else end)
            if at<0: break
            found.append(at);start=at+1
        if len(found)!=1: raise GenerationError('Expected one structural field anchor, found '+str(found))
        return found[0]
    anchor=struct.pack('<3if2fBB',1,TOTAL_EMIT_COUNT,nominal//200,0,200,200,0,0)
    offset=unique(anchor);position=offset
    def read(fmt):
        nonlocal position
        size=struct.calcsize('<'+fmt)
        if position+size>len(data): raise GenerationError('Truncated GPU parameter block')
        result=struct.unpack_from('<'+fmt,data,position);position+=size;return result
    def expect(fmt, values, label):
        actual=read(fmt)
        if len(actual)!=len(values) or any(not math.isfinite(a) or abs(a-b)>1e-6 for a,b in zip(actual,values)):
            raise GenerationError(label+': '+str(actual)+' != '+str(values))
        return actual
    expect('3if2fBB',(1,TOTAL_EMIT_COUNT,nominal//200,0,200,200,0,0),'GPU basic/emission')
    speed=expect('8f',(0,0,1,60,.10,.035,.03,.01),'velocity/damping')
    expect('12f',(0,)*12,'rotation')
    expect('B',(2,),'scale easing type')
    expect('8f',(1.15,.75,1,1,1,1,1,1),'initial scale')
    expect('8f',(.35,.15,1,1,1,1,1,1),'terminal scale')
    expect('3f',(0,0,1),'linear easing coefficients')
    gravity=expect('3f',(0,-.0006,0),'gravity per frame')
    expect('2f',(0,0),'vortex forces');expect('3f',(0,0,0),'vortex center');expect('3f',(0,1,0),'vortex axis')
    expect('fifi',(0,0,4,1),'turbulence')
    expect('4BIf',(2,0,1,0,0,.03),'GPU sprite render state')
    expect('3B',(1,4,0),'gradient/inherit/color space')
    gradient=bytes(read('128B'));legacy=build_gradient()
    gradient_deltas=[a-b for a,b in zip(gradient,legacy)]
    delta=max(abs(v) for v in gradient_deltas)
    if max(abs(v) for v in gradient_deltas[:-4])>1 or gradient[-4:]!=bytes(4):
        raise GenerationError('Unexpected official gradient quantization/endpoint behavior')
    expect('3f',(1,8,35),'emissive/fade')
    expect('B4I4B4B',(0,0,0xffffffff,0xffffffff,0xffffffff,1,1,0,0,0,0,0,0),'material/texture/sampler')
    expect('i',(0,),'child count')
    if position!=len(data): raise GenerationError('Unexpected trailing nodes/bytes')
    # Exact current CPU common structure: preserve the original additive sprite.
    common=struct.pack('<if8i14i',0,1,0,-1,-1,-1,-1,-1,-1,2,*([1,0]*7))
    common_offset=unique(common,offset)
    seed_offset=unique(struct.pack('<i',FIXED_RANDOM_SEED),common_offset)
    life_offset=unique(struct.pack('<ii',OWNER_LIFETIME_FRAMES,OWNER_LIFETIME_FRAMES),common_offset)
    return dict(binaryVersion=1810,nominalParticles=nominal,emitCount=TOTAL_EMIT_COUNT,emitPerFrame=nominal//200,
        lifetimeFrames=200,ownerLifetimeFrames=1200,seed=1806,gpuNodeOffset=offset,cpuCommonOffset=common_offset,
        cpuSamplerOffset=common_offset+40,cpuSamplerPairs=[[1,0]]*7,seedOffset=seed_offset,ownerLifetimeOffset=life_offset,
        velocityAndDamping=speed,gravity=gravity,gradientSha256=sha256_bytes(gradient),legacyGradientMaxByteDelta=delta,
        gradientSamples=[list(gradient[i:i+4]) for i in range(0,128,4)],
        legacyGradientSamples=[list(legacy[i:i+4]) for i in range(0,128,4)],
        gradientSampleDeltas=[gradient_deltas[i:i+4] for i in range(0,128,4)],
        gradientDifference=dict(interiorMaxByteDelta=max(abs(v) for v in gradient_deltas[:-4]),
            changedChannels=sum(v!=0 for v in gradient_deltas),meanAbsoluteByteDelta=sum(abs(v) for v in gradient_deltas)/128,
            officialEndpoint=[0,0,0,0],legacyEndpoint=list(legacy[-4:]),
            reason='Official Gradient.Sample at t=1 returns zero; accepted migration difference, visual review required'),
        effectSha256=sha256_bytes(payload),binSha256=sha256_bytes(data),scope='Compiled authored parameters, not live GPU particle count')


def inspect(root, source, output):
    subprocess.run(['pwsh','-NoProfile','-File',str(root/'Tools/VFX/recipes/InspectCompiled.ps1'),
        '-InputFile',str(source),'-OutputFile',str(output),'-CoreAssembly',str(root/'Engine/Tools/Effekseer/bin/EffekseerCore.dll')],
        check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)


def compile_official(root, source, output):
    editor=root/'Engine/Tools/Effekseer/bin/Effekseer.exe'
    result=subprocess.run([str(editor),'-cui','-in',str(source),'-o',str(output)],cwd=editor.parent,
        stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
    if result.returncode or not output.is_file(): raise GenerationError('Official compiler failed: '+result.stderr.decode(errors='replace')[:1200])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo-root',type=Path,default=Path(__file__).resolve().parents[2])
    parser.add_argument('--check',action='store_true',help='Recompile and compare all source/package bytes without touching outputs')
    args=parser.parse_args();root=args.repo_root.resolve()
    original=read_verified(root/SOURCE_EFFECT,EXPECTED_EFFECT_SHA256)
    read_verified(root/SOURCE_TEXTURE,EXPECTED_TEXTURE_SHA256)
    license_text=(root/SOURCE_LICENSE).read_bytes().replace(b'\r\n',b'\n')
    if sha256_bytes(license_text)!=EXPECTED_LICENSE_SHA256: raise GenerationError('Reviewed license text changed')
    for name,sha in COMPILER_HASHES.items(): read_verified(root/'Engine/Tools/Effekseer/bin'/name,sha)
    texture=build_spark_texture_png();outputs={};summaries=[]
    with tempfile.TemporaryDirectory(prefix='he-stress-official-') as temporary:
        stage=Path(temporary);source=stage/'upstream.efkefc';source.write_bytes(original)
        decoded=stage/'upstream.xml';inspect(root,source,decoded)
        for nominal in (20000,50000):
            name='GpuSparks'+str(nominal//1000)+'k';folder=stage/name;folder.mkdir()
            (folder/'Texture').mkdir();(folder/'Texture/Particle01.png').write_bytes(texture)
            authored,expected=author_project(decoded.read_bytes(),nominal)
            project=folder/(name+'.efkproj');project.write_bytes(authored)
            compiled=folder/(name+'.efkefc');compile_official(root,project,compiled)
            data=compiled.read_bytes();validation=validate_compiled_stress(data,nominal)
            roundtrip=folder/'roundtrip.xml';inspect(root,compiled,roundtrip)
            node=ET.parse(roundtrip).getroot().find('Root/Children/Node')
            for path,value in expected.items():
                actual=node.findtext(path)
                # Explicit nondefaults must survive official EDIT serialization.
                if actual is None and value in (1,200,'True'): continue
                if isinstance(value,str): valid=actual==value
                else: valid=actual is not None and abs(float(actual)-value)<1e-5
                if not valid: raise GenerationError('EDIT roundtrip mismatch '+path+': '+str(actual))
            if node.findtext('DrawingValues/Type','2')!='2': raise GenerationError('CPU sprite was removed')
            recompiled=folder/'recompiled.efkefc';compile_official(root,roundtrip,recompiled)
            if dict(parse_chunks(recompiled.read_bytes()))[b'BIN_']!=dict(parse_chunks(data))[b'BIN_']:
                raise GenerationError('Saved EDIT cannot reproduce runtime BIN')
            metadata=dict(schema_version=2,asset_id=name.lower(),estimated_peak_particles=nominal,
                uses_gpu_particles=True,has_cpu_fallback=False,minimum_effekseer_version=RELEASE_VERSION,
                source=dict(release_url=RELEASE_URL,effect_path=SOURCE_EFFECT.as_posix(),effect_sha256=EXPECTED_EFFECT_SHA256),
                license=dict(identifier='CC0-1.0',path=SOURCE_LICENSE.as_posix(),sha256=EXPECTED_LICENSE_SHA256),
                compiler_sha256=COMPILER_HASHES,source_sha256=sha256_bytes(authored),texture_sha256=sha256_bytes(texture),
                parameters=dict(fixed_random_seed=1806,emit_count=TOTAL_EMIT_COUNT,emit_per_frame=nominal//200,lifetime_frames=200,
                    owner_lifetime_frames=1200,initial_speed_range=[.035,.1],damping_range=[.01,.03],gravity=[0,-.0006,0],
                    authored_gravity=[0,-.036,0],scale_start=[.75,1.15],scale_end=[.15,.35],shape_size=.03,
                    gradient_stops=GRADIENT_STOPS,fade_in=8,fade_out=35),
                validation=dict(official_compile='PASS',edit_runtime_roundtrip='PASS',runtime=validation,DX12='NOT_RUN',Vulkan='NOT_RUN'),
                editor_data_note='All runtime parameters authored in EDIT; official exporter only. No runtime patching.')
            encoded=(json.dumps(metadata,indent=2,sort_keys=True)+'\n').encode()
            entries={name+'.efkefc':data,'PROVENANCE.json':encoded,'LICENSE-Effekseer-Sample-CC0.md':license_text,'Texture/Particle01.png':texture}
            package=deterministic_zip(entries)
            for relative,blob in {**entries,name+'.efkproj':authored,'roundtrip.xml':roundtrip.read_bytes()}.items():
                outputs[root/AUTHOR_DIRECTORY/name/relative]=blob
            outputs[root/OUTPUT_DIRECTORY/(name+'.efkpkg')]=package
            summaries.append(dict(name=name,package_sha256=sha256_bytes(package),source_sha256=sha256_bytes(authored),validation=validation))
    outputs[root/AUTHOR_DIRECTORY/'manifest.json']=(json.dumps(dict(schemaVersion=2,compiler=COMPILER_HASHES,outputs=summaries),indent=2,sort_keys=True)+'\n').encode()
    changed=0
    for path,blob in outputs.items():
        if path.is_file() and path.read_bytes()==blob: continue
        if args.check: raise GenerationError('Rebuild differs: '+str(path))
        path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(blob);changed+=1
    print(json.dumps(dict(status='PASS',mode='check' if args.check else 'generate',changed=changed,outputs=summaries),indent=2))
    return 0


if __name__=='__main__':
    raise SystemExit(main())
