"""Build the subgroup-independent fluid kernel for the one verified game shader.

Preserve the game's descriptor contract and reject an unexpected bundle. This is
a complete accumulation kernel, not a dropped dispatch or disabled effect.
"""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

KEY = '098a126f92bf533a'
EXPECTED = [(5,6,1,1392,1,0,0xffffffff,0), (11,7,1,0,3,1,0xffffffff,0),
            (17,7,1,0,3,0,0xffffffff,0), (23,3,1,0,8,4,0xffffffff,0x80000101)]

def replace(key, bundle):
    if key != KEY:
        return bundle
    if len(bundle) < 32:
        raise ValueError('fluid compatibility: truncated header')
    header = list(struct.unpack_from('<8I', bundle))
    if header[:6] != [0x31534758, 1, 5, 4, 0, 0] or len(bundle) != 160 + header[6]:
        raise ValueError('fluid compatibility: unexpected shader contract')
    bindings = [struct.unpack_from('<8I', bundle, 32+i*32) for i in range(4)]
    if bindings != EXPECTED:
        raise ValueError('fluid compatibility: descriptor mismatch')
    compiler = os.environ.get('GLSLC') or shutil.which('glslc')
    validator = shutil.which('spirv-val')
    if not compiler or not validator:
        raise ValueError('fluid compatibility requires glslc and spirv-val')
    source = Path(__file__).resolve().parent.parent/'vulkan/kernels/fluid_accumulate.comp'
    with tempfile.TemporaryDirectory(prefix='dm-fluid-') as work:
        output = Path(work)/'fluid.spv'
        subprocess.run([compiler, '--target-env=vulkan1.1', '-O', str(source), '-o', str(output)], check=True, capture_output=True)
        subprocess.run([validator, '--target-env', 'vulkan1.1', str(output)], check=True, capture_output=True)
        spirv = output.read_bytes()
    header[6] = len(spirv)
    return struct.pack('<8I', *header)+bundle[32:160]+spirv

if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    args = parser.parse_args()
    if args.bundle.stem != KEY:
        parser.error('expected '+KEY+'.xgs')
    original = args.bundle.read_bytes()
    result = replace(KEY, original)
    tmp = args.bundle.with_suffix('.xgs.tmp')
    tmp.write_bytes(result)
    tmp.replace(args.bundle)
    print('fluid accumulation compatibility kernel packed and validated')
