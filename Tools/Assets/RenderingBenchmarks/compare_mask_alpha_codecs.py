#!/usr/bin/env python3
"""CPU-only BC3/BC7 comparison of the same repaired 32x32 Mask inputs."""
import argparse
import io
from pathlib import Path
import subprocess
import numpy as np
from PIL import Image
from compress_benchmark_textures import read,write,sha,PROJECT
from repair_mask_mips import bilinear_coverage


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--variant',type=Path,required=True)
    args=parser.parse_args(); output=args.variant.resolve()
    report=read(output/'compression.json'); results=[]
    for repair in report['maskRepair']['textures']:
        level=next(m for m in repair['mips'] if m['width']==32)
        chosen=next(a for a in level['attempts'] if a['marginByte']==level['chosenMarginByte'] and a['areaOffsetByte']==level['chosenAreaOffsetByte'])
        command=chosen['command'][:]; png=Path(command[-1]); folder=png.parent/'BC3Comparison'; folder.mkdir(exist_ok=True)
        command[command.index('-f')+1]='BC3_UNORM_SRGB'
        bc_index=command.index('-bc'); del command[bc_index:bc_index+2]
        command[command.index('-o')+1]=str(folder)
        run=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=60)
        (folder/'encoder.log').write_text(run.stdout,encoding='utf-8')
        if run.returncode:raise RuntimeError('BC3 comparison encoder failed')
        original=np.asarray(Image.open(png).convert('RGBA'),dtype=np.float32)/255
        formats={}
        for name,path in [('BC7',png.with_suffix('.dds')),('BC3',folder/'level.dds')]:
            if name=='BC3':
                # Pillow lacks DX10 BC3 sRGB, but accepts the identical BC3
                # payload through its legacy DXT5 decoder. This in-memory
                # header view changes no file or encoded/color-channel bytes.
                data=path.read_bytes(); header=bytearray(data[:128]); header[84:88]=b'DXT5'
                image=Image.open(io.BytesIO(bytes(header)+data[148:]))
            else: image=Image.open(path)
            actual=np.asarray(image.convert('RGBA'),dtype=np.float32)/255
            accepted=original[...,3]>=repair['cutoff']
            mse=float(np.mean((original[...,:3][accepted]-actual[...,:3][accepted])**2))
            formats[name]={'pointCoverage':float(np.mean(actual[...,3]>=repair['cutoff'])),
                           'bilinear8x8Coverage':bilinear_coverage(actual[...,3],repair['cutoff'],8),
                           'acceptedRgbMeanSquaredError':mse,'sha256':sha(path)}
        results.append({'texture':repair['texture'],'mip':6,'size':[32,32],
                        'targetPointCoverage':level['targetCoverage'],'targetBilinearCoverage':level['sourceMip0Bilinear4x4Coverage'],
                        'inputSha256':sha(png),'bc3Command':command,'formats':formats})
    write(output/'mask_codec_comparison.json',{'evidenceType':'unit-test','status':'PASS_MEASURED_COMPARISON',
          'command':'python Tools/Assets/RenderingBenchmarks/compare_mask_alpha_codecs.py --variant '+str(args.variant),
          'selection':'Keep corrected BC7; compare decoded coverage and RGB before considering a whole-texture BC3 format change.',
          'results':results})
    for result in results: print(result['texture'], result['formats'])


if __name__=='__main__':main()
