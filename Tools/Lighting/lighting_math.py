"""Linear lighting math and texture packing shared by baking and tests."""
import math
from pathlib import Path
import struct

import numpy as np


# HE coordinates; each face has forward, right, up. Image rows are bottom-up.
FACES = np.array([
    [[1,0,0],[0,0,1],[0,1,0]], [[-1,0,0],[0,0,-1],[0,1,0]],
    [[0,1,0],[-1,0,0],[0,0,-1]], [[0,-1,0],[-1,0,0],[0,0,1]],
    [[0,0,1],[-1,0,0],[0,1,0]], [[0,0,-1],[1,0,0],[0,1,0]]], dtype=np.float64)


def blender_uv_rows(sampling):
    """Conjugate HE's top-left UV affine transform into bottom-left UV space."""
    if sampling.get('useAffine', False):
        a,b,c = sampling.get('affineRow0', [1,0,0])
        d,e,f = sampling.get('affineRow1', [0,1,0])
    else:
        angle = sampling.get('rotation', 0)
        sx,sy = sampling.get('scale', [1,1])
        c,f = sampling.get('offset', [0,0])
        a,b,d,e = math.cos(angle)*sx, -math.sin(angle)*sy, math.sin(angle)*sx, math.cos(angle)*sy
    rows = np.array([[a,-b,b+c], [-d,e,1-e-f]], dtype=float)
    if not np.isfinite(rows).all(): raise ValueError('Non-finite texture sampling')
    return rows


def sh_basis(directions):
    x, y, z = np.moveaxis(np.asarray(directions,dtype=np.float64), -1, 0)
    return np.stack([np.full_like(x, .28209479177387814), .48860251190292*y,
        .48860251190292*z, .48860251190292*x, 1.0925484305920792*x*y,
        1.0925484305920792*y*z, .31539156525252005*(3*z*z-1),
        1.0925484305920792*x*z, .5462742152960396*(x*x-y*y)], axis=-1)


def cube_irradiance_sh(faces):
    faces = np.asarray(faces, dtype=np.float64)
    size = faces.shape[1]
    if faces.shape != (6, size, size, 3) or not np.isfinite(faces).all():
        raise ValueError("Expected six finite square linear RGB faces")
    axis = (np.arange(size)+.5)/size*2-1
    u, v = np.meshgrid(axis, axis)
    weight = 4/(size*size)*(1+u*u+v*v)**-1.5
    weight *= (4*math.pi)/(6*weight.sum())
    result = np.zeros((9,3), dtype=np.float64)
    for index, (forward, right, up) in enumerate(FACES):
        directions = forward + u[...,None]*right + v[...,None]*up
        directions /= np.linalg.norm(directions,axis=-1,keepdims=True)
        result += np.einsum('hwk,hwc,hw->kc',sh_basis(directions),faces[index],weight)
    result *= np.array([math.pi]+[2*math.pi/3]*3+[math.pi/4]*5)[:,None]
    return result


def cube_to_panorama(faces, width):
    height = width//2
    phi = ((np.arange(width)+.5)/width-.5)*2*math.pi
    latitude = (.5-(np.arange(height)+.5)/height)*math.pi
    p, t = np.meshgrid(phi,latitude)
    d = np.stack([np.cos(t)*np.cos(p),np.sin(t),np.cos(t)*np.sin(p)],axis=-1)
    dots = np.einsum('hwc,fc->hwf',d,FACES[:,0])
    selected = dots.argmax(axis=-1)
    output = np.zeros((height,width,3),np.float32)
    size = faces.shape[1]
    for index,(forward,right,up) in enumerate(FACES):
        mask = selected == index
        direction = d[mask]
        denominator = direction @ forward
        # Bilinear within a face. Samples across the cube edge are supplied by
        # the adjacent face through face selection; border clamping is bounded.
        uv = np.stack([direction@right,direction@up],axis=-1)/denominator[:,None]
        coordinates = np.clip((uv*.5+.5)*size-.5,0,size-1)
        lo = np.floor(coordinates).astype(int); hi=np.minimum(lo+1,size-1)
        f=coordinates-lo
        a=faces[index,lo[:,1],lo[:,0]]*(1-f[:,0,None])+faces[index,lo[:,1],hi[:,0]]*f[:,0,None]
        b=faces[index,hi[:,1],lo[:,0]]*(1-f[:,0,None])+faces[index,hi[:,1],hi[:,0]]*f[:,0,None]
        output[mask]=a*(1-f[:,1,None])+b*f[:,1,None]
    return output


def write_hdr(path, rgb):
    rgb=np.maximum(np.asarray(rgb,dtype=np.float64),0)
    if not np.isfinite(rgb).all(): raise ValueError("Non-finite HDR output")
    height,width,_=rgb.shape
    mantissa, exponent=np.frexp(rgb.max(axis=-1))
    scale=np.where(rgb.max(axis=-1)>1e-32,np.ldexp(np.ones_like(mantissa),8-exponent),0)
    rgbe=np.zeros((height,width,4),np.uint8)
    rgbe[...,:3]=np.clip(rgb*scale[...,None],0,255).astype(np.uint8)
    rgbe[...,3]=np.where(scale>0,np.clip(exponent+128,0,255),0).astype(np.uint8)
    # Standard scanline RLE, literal packets; no exposed/display transform.
    with open(path,'wb') as stream:
        stream.write(f'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {height} +X {width}\n'.encode())
        for row in rgbe:
            stream.write(bytes([2,2,width>>8,width&255]))
            for channel in range(4):
                for offset in range(0,width,128):
                    data=row[offset:offset+128,channel].tobytes()
                    stream.write(bytes([len(data)])); stream.write(data)


def write_dds(path, rgb):
    """DX10 RGBA16F, full mip chain; input rows top-down, values E/pi."""
    rgb=np.maximum(np.asarray(rgb,dtype=np.float32),0)
    if not np.isfinite(rgb).all() or rgb.max(initial=0)>65504:
        raise ValueError("Lightmap exceeds finite half-float range")
    h,w,_=rgb.shape
    if h!=w or h<4 or w&(w-1): raise ValueError("Power-of-two square lightmap required")
    levels=int(math.log2(w))+1
    header=[124,0x2100F,h,w,w*8,0,levels]+[0]*11
    header += [32,4,struct.unpack('<I',b'DX10')[0],0,0,0,0,0]
    header += [0x401008,0,0,0,0]
    with open(path,'wb') as stream:
        stream.write(b'DDS '+struct.pack('<31I',*header))
        stream.write(struct.pack('<5I',10,3,0,1,0))
        for _ in range(levels):
            rgba=np.concatenate([rgb,np.ones((*rgb.shape[:2],1),np.float32)],axis=-1)
            stream.write(rgba.astype('<f2').tobytes())
            if rgb.shape[0]>1: rgb=rgb.reshape(rgb.shape[0]//2,2,rgb.shape[1]//2,2,3).mean(axis=(1,3))
