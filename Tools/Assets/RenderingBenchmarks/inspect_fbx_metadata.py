#!/usr/bin/env python3
"""Read only scalar FBX camera/light/unit metadata; skip geometry and animation arrays."""
import argparse
import json
from pathlib import Path
import struct

from import_gltf_scene import write_json, digest


def metadata(path):
    with path.open('rb') as stream:
        header = stream.read(27)
        if not header.startswith(b'Kaydara FBX Binary'):
            raise ValueError('Only binary FBX metadata is supported')
        version = struct.unpack_from('<I',header,23)[0]
        header_format = '<QQQB' if version >= 7500 else '<IIIB'
        header_size = struct.calcsize(header_format)
        def property_value():
            kind = stream.read(1)
            formats = {b'Y':'h',b'C':'?',b'I':'i',b'F':'f',b'D':'d',b'L':'q'}
            if kind in formats:
                fmt = '<'+formats[kind]
                return struct.unpack(fmt,stream.read(struct.calcsize(fmt)))[0]
            if kind in [b'S',b'R']:
                count = struct.unpack('<I',stream.read(4))[0]
                data = stream.read(count)
                return data.decode('utf-8',errors='replace') if kind == b'S' else {'byteCount':count}
            if kind in [b'f',b'd',b'l',b'i',b'b',b'c']:
                count,encoding,length = struct.unpack('<III',stream.read(12))
                stream.seek(length,1)
                return {'arrayCount':count,'encoding':encoding,'byteCount':length}
            raise ValueError('Unsupported FBX property type '+repr(kind))
        def node():
            raw = stream.read(header_size)
            if len(raw) != header_size:
                return None
            end,count,property_length,name_length = struct.unpack(header_format,raw)
            if not end:
                return None
            name = stream.read(name_length).decode('utf-8')
            if name in ['Geometry','AnimationCurve','AnimationCurveNode','AnimationStack','AnimationLayer','Deformer']:
                stream.seek(end)
                return None
            properties = [property_value() for _ in range(count)]
            children = []
            while stream.tell() < end-header_size:
                child = node()
                if child:
                    children.append(child)
            stream.seek(end)
            return {'name':name,'properties':properties,'children':children}
        roots = []
        while True:
            value = node()
            if not value:
                break
            roots.append(value)
    result = {'version':version,'sourceSha256':digest(path),'globalSettings':{},'attributes':[]}
    def property_map(item):
        values = {}
        for group in item['children']:
            if group['name'] in ['Properties70','Properties60']:
                for prop in group['children']:
                    if prop['name'] == 'P':
                        values[prop['properties'][0]] = prop['properties'][4:]
        return values
    for root in roots:
        if root['name'] == 'GlobalSettings':
            result['globalSettings'] = property_map(root)
        if root['name'] == 'Objects':
            for item in root['children']:
                if item['name'] == 'NodeAttribute':
                    result['attributes'].append({'identity':item['properties'],'properties':property_map(item)})
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args()
    result = metadata(args.source)
    write_json(args.output,result)
    print(json.dumps({'version':result['version'],'globalSettings':result['globalSettings'],
                     'attributes':result['attributes']},indent=2))
