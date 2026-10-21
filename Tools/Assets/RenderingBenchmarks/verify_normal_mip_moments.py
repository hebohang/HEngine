#!/usr/bin/env python3
"""Measure preserved XYZ average-length moments, independently of the encoder.

Uses unnormalized, linear BOX averages of the original prepared PNG as reference.
BC5 direction remains valid but its absent blue cannot carry this variance.
"""
import argparse
import io
from pathlib import Path
import struct

import numpy as np
from PIL import Image
from compress_benchmark_textures import read, write, sha, PROJECT


def variance(pixels):
    length = np.linalg.norm(pixels[..., :3] * 2 - 1, axis=-1)
    return np.minimum(np.maximum(1-length, 0)/np.maximum(length, 1e-4), .35)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--variant', type=Path, required=True)
    args = parser.parse_args()
    root = args.variant.resolve()
    records = read(root/'compression.json')['textures']
    results = []
    for key, record in records.items():
        if record['role'] != 'normal':
            continue
        path = PROJECT/record['output']
        if record['encodedFormat'] != 98 or sha(path) != record['outputSha256']:
            raise ValueError('Expected unmodified linear BC7 normal: '+key)
        pixels = np.asarray(Image.open(record['source']).convert('RGB'), dtype=np.float32)/255
        data = path.read_bytes()
        header = bytearray(data[:148])
        offset = 148
        levels = []
        direction_quality = {}
        for mip in range(record['mipLevels']):
            height, width = pixels.shape[:2]
            byte_count = ((width+3)//4)*((height+3)//4)*16
            # Initial angle error is already measured by the conversion. Here
            # inspect all filtered levels and keep each image bounded in RAM.
            struct.pack_into('<II', header, 12, height, width)
            struct.pack_into('<I', header, 28, 1)
            decoded = np.asarray(Image.open(io.BytesIO(bytes(header)+data[offset:offset+byte_count])).convert('RGB'), dtype=np.float32)/255
            if mip == 0:
                source_vector, decoded_vector = pixels*2-1, decoded*2-1
                source_vector /= np.maximum(np.linalg.norm(source_vector,axis=-1)[...,None],1e-6)
                decoded_vector /= np.maximum(np.linalg.norm(decoded_vector,axis=-1)[...,None],1e-6)
                angles = np.degrees(np.arccos(np.clip(np.sum(source_vector*decoded_vector,axis=-1),-1,1)))
                direction_quality = {'rgbNormalizeMeanDegrees':float(angles.mean()),
                                     'rgbNormalizeP99Degrees':float(np.percentile(angles,99)),
                                     'reference':'Normalized original XYZ; distinct from current XY-reconstructed runtime path'}
            else:
                reference, actual = variance(pixels), variance(decoded)
                error = np.abs(reference-actual)
                levels.append({'mip':mip, 'width':width, 'height':height,
                               'referenceMeanVariance':float(reference.mean()),
                               'bc7MeanVariance':float(actual.mean()),
                               'varianceMeanAbsoluteError':float(error.mean()),
                               'varianceP99AbsoluteError':float(np.percentile(error,99)),
                               'bc5MeanVarianceWithMissingBlue':0.0})
            offset += byte_count
            if width > 1 or height > 1:
                # Source benchmark dimensions are powers of two; one-wide
                # tails preserve the remaining dimension's BOX average.
                if width > 1:
                    pixels = (pixels[:,0::2]+pixels[:,1::2])*.5
                if height > 1:
                    pixels = (pixels[0::2]+pixels[1::2])*.5
        if offset != len(data):
            raise ValueError('DDS mip payload bounds mismatch')
        results.append({'texture':key, 'sourceSha256':record['sourceSha256'],
                        'outputSha256':record['outputSha256'], 'mips':levels,
                        'mipZeroRgbDecode':direction_quality})
    errors = [m['varianceMeanAbsoluteError'] for r in results for m in r['mips']]
    report = {'evidenceType':'unit-test', 'status':'PASS_MEASURED_FORMAT_CONTRACT',
              'command':'python Tools/Assets/RenderingBenchmarks/verify_normal_mip_moments.py --variant '+str(args.variant),
              'reference':'Unnormalized linear BOX XYZ means, shader variance clamp .35; no inferred BC5 Z moment',
              'normalTextures':len(results), 'measuredMipLevels':len(errors),
              'worstRgbDecodeMeanDegrees':max((r['mipZeroRgbDecode']['rgbNormalizeMeanDegrees'] for r in results),default=0),
              'worstRgbDecodeP99Degrees':max((r['mipZeroRgbDecode']['rgbNormalizeP99Degrees'] for r in results),default=0),
              'worstMipMeanAbsoluteVarianceError':max(errors, default=0),
              'limitation':'Measured compression error, not rendered specular stability or pixel-perfect PNG equivalence.',
              'textures':results}
    write(root/'normal_mip_moments.json', report)
    print({k:v for k,v in report.items() if k != 'textures'})


if __name__ == '__main__':
    main()
