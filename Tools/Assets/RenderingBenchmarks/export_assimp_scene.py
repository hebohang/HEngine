#!/usr/bin/env python3
"""Export a source scene with the official Assimp C API, retaining its nodes.

This uses a local, separately recorded official DLL. It does not execute scene
scripts, bake transforms, merge meshes, or modify the engine's Assimp dependency.
"""
import argparse
import ctypes as c
import json
from pathlib import Path
import time

from import_gltf_scene import ROOT, digest, write_json


class AiString(c.Structure):
    _fields_ = [('length', c.c_uint32), ('data', c.c_char*1024)]
    def text(self):
        return bytes(self.data)[:self.length].decode('utf-8', errors='replace')


class Matrix(c.Structure):
    _fields_ = [('values', c.c_float*16)]


class Node(c.Structure):
    pass


Node._fields_ = [('name',AiString),('transform',Matrix),('parent',c.POINTER(Node)),
    ('numChildren',c.c_uint),('children',c.POINTER(c.POINTER(Node))),
    ('numMeshes',c.c_uint),('meshes',c.POINTER(c.c_uint)),('metadata',c.c_void_p)]


class MeshHeader(c.Structure):
    _fields_ = [('primitiveTypes',c.c_uint),('numVertices',c.c_uint),('numFaces',c.c_uint)]


class Scene(c.Structure):
    _fields_ = [('flags',c.c_uint),('root',c.POINTER(Node)),('numMeshes',c.c_uint),
        ('meshes',c.POINTER(c.POINTER(MeshHeader))),('numMaterials',c.c_uint),
        ('materials',c.POINTER(c.c_void_p)),('numAnimations',c.c_uint),('animations',c.c_void_p),
        ('numTextures',c.c_uint),('textures',c.c_void_p),('numLights',c.c_uint),('lights',c.c_void_p),
        ('numCameras',c.c_uint),('cameras',c.c_void_p),('metadata',c.c_void_p)]


class MaterialProperty(c.Structure):
    _fields_ = [('key',AiString),('semantic',c.c_uint),('index',c.c_uint),
        ('dataLength',c.c_uint),('type',c.c_int),('data',c.c_void_p)]


class Material(c.Structure):
    _fields_ = [('properties',c.POINTER(c.POINTER(MaterialProperty))),('numProperties',c.c_uint),('allocated',c.c_uint)]


def load_library(path):
    lib = c.CDLL(str(path.resolve()))
    lib.aiImportFile.argtypes = [c.c_char_p,c.c_uint]
    lib.aiImportFile.restype = c.POINTER(Scene)
    lib.aiReleaseImport.argtypes = [c.POINTER(Scene)]
    lib.aiExportScene.argtypes = [c.POINTER(Scene),c.c_char_p,c.c_char_p,c.c_uint]
    lib.aiExportScene.restype = c.c_int
    lib.aiGetErrorString.restype = c.c_char_p
    lib.aiGetMaterialString.argtypes = [c.c_void_p,c.c_char_p,c.c_uint,c.c_uint,c.POINTER(AiString)]
    lib.aiGetMaterialString.restype = c.c_int
    lib.aiGetMaterialTextureCount.argtypes = [c.c_void_p,c.c_int]
    lib.aiGetMaterialTextureCount.restype = c.c_uint
    lib.aiGetMaterialTexture.argtypes = [c.c_void_p,c.c_int,c.c_uint,c.POINTER(AiString),
        c.c_void_p,c.c_void_p,c.c_void_p,c.c_void_p,c.c_void_p,c.c_void_p]
    lib.aiGetMaterialTexture.restype = c.c_int
    return lib


def scene_manifest(lib, scene):
    value = scene.contents
    if not value.root or value.numMeshes > 1000000 or value.numMaterials > 100000:
        raise ValueError('Invalid scene header or incompatible Assimp ABI')
    nodes = []
    def visit(pointer,parent):
        node = pointer.contents
        if node.numChildren > 100000 or node.numMeshes > value.numMeshes:
            raise ValueError('Invalid scene node')
        index = len(nodes)
        nodes.append({'name':node.name.text(),'parent':parent,'localMatrixRowMajor':list(node.transform.values),
                      'meshes':[node.meshes[i] for i in range(node.numMeshes)]})
        for i in range(node.numChildren):
            visit(node.children[i],index)
    visit(value.root,-1)
    materials = []
    for i in range(value.numMaterials):
        pointer = value.materials[i]
        name = AiString()
        lib.aiGetMaterialString(pointer,b'?mat.name',0,0,c.byref(name))
        textures = []
        for semantic in range(1,29):
            for index in range(lib.aiGetMaterialTextureCount(pointer,semantic)):
                path = AiString()
                result = lib.aiGetMaterialTexture(pointer,semantic,index,c.byref(path),None,None,None,None,None,None)
                if result != 0:
                    raise ValueError('Failed to query source material texture')
                textures.append({'semantic':semantic,'index':index,'path':path.text()})
        properties = []
        material = c.cast(pointer,c.POINTER(Material)).contents
        for j in range(material.numProperties):
            prop = material.properties[j].contents
            if prop.dataLength > 65536:
                raise ValueError('Unexpected oversized material property')
            result = {'key':prop.key.text(),'semantic':prop.semantic,'index':prop.index,'type':prop.type}
            if prop.type == 1:
                result['value'] = list(c.cast(prop.data,c.POINTER(c.c_float))[:prop.dataLength//4])
            elif prop.type == 4:
                result['value'] = list(c.cast(prop.data,c.POINTER(c.c_int))[:prop.dataLength//4])
            elif prop.type == 3:
                # Material string storage is length + text, not a full aiString.
                length = c.cast(prop.data,c.POINTER(c.c_uint32)).contents.value
                result['value'] = c.string_at(prop.data+4,min(length,prop.dataLength-4)).decode('utf-8',errors='replace')
            else:
                result['byteLength'] = prop.dataLength
            properties.append(result)
        materials.append({'index':i,'name':name.text(),'textures':textures,'properties':properties})
    return {'nodes':nodes,'materials':materials,'meshCount':value.numMeshes,
        'uniqueVertices':sum(value.meshes[i].contents.numVertices for i in range(value.numMeshes)),
        'uniqueTriangles':sum(value.meshes[i].contents.numFaces for i in range(value.numMeshes)),
        'instanceTriangles':sum(value.meshes[i].contents.numFaces for n in nodes for i in n['meshes']),
        'meshReferences':sum(len(n['meshes']) for n in nodes),'animationCount':value.numAnimations,
        'embeddedTextureCount':value.numTextures,'lightCount':value.numLights,'cameraCount':value.numCameras}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--library',type=Path,default=ROOT/'Examples/RenderingTests/SourceAssets/RenderingBenchmarks/Tools/Assimp605/assimp-vc143-mt.dll')
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True,exist_ok=True)
    lib = load_library(args.library)
    start = time.monotonic()
    # Triangulate only. No PreTransformVertices, OptimizeGraph, or mesh merge.
    scene = lib.aiImportFile(str(args.source.resolve()).encode('utf-8'),0x8)
    if not scene:
        raise ValueError(lib.aiGetErrorString().decode('utf-8',errors='replace'))
    try:
        manifest = scene_manifest(lib,scene)
        manifest.update({'source':str(args.source.resolve()),'sourceSha256':digest(args.source),
            'librarySha256':digest(args.library),'assimpVersion':'%d.%d.%d'%(lib.aiGetVersionMajor(),lib.aiGetVersionMinor(),lib.aiGetVersionPatch()),
            'postProcessFlags':['aiProcess_Triangulate'],'preserveNodeTransforms':True})
        result = lib.aiExportScene(scene,b'gltf2',str(args.output.resolve()).encode('utf-8'),0)
        if result != 0:
            raise ValueError(lib.aiGetErrorString().decode('utf-8',errors='replace'))
        manifest['seconds'] = time.monotonic()-start
        manifest['outputGltfSha256'] = digest(args.output)
        write_json(args.output.with_suffix('.source.json'),manifest)
        print(json.dumps({k:v for k,v in manifest.items() if k not in {'nodes','materials'}},indent=2))
    finally:
        lib.aiReleaseImport(scene)


if __name__ == '__main__':
    main()
