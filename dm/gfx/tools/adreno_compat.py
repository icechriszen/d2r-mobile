"""Normalize verified compute and fragment modules that crashes the Adreno 750 compiler.

仅对已核验的输入运行标准优化，保留描述符与常量数据；未知输入拒绝处理。
Run standard optimization only on the verified input, preserving descriptors and
constant data. Unknown inputs fail closed; Apple shader generation is unaffected.
"""
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

KEY = '6a7a308d61928982'
INPUT_SHA256 = '8813d82ed2bf2b95d995ef03796cb743589e6f99bcd67d52701981280c3e8867'
CONTRACTS = {
    KEY: ([0x31534758, 1, 5, 6, 0, 256], INPUT_SHA256),
    '00f1902d880682c7': ([0x31534758, 1, 4, 31, 0, 256],
        'f0f17c693fead53aa7eaf19cbc3cfe42bcb14ffae740c8f6c62cf86d228efb24'),
}

def replace(key, bundle):
    if key not in CONTRACTS:
        return bundle
    if len(bundle) < 32:
        raise ValueError('Adreno compatibility: truncated header')
    header = list(struct.unpack_from('<8I', bundle))
    contract, digest = CONTRACTS[key]
    if header[:6] != contract:
        raise ValueError('Adreno compatibility: unexpected shader contract')
    offset = 32 + header[3]*32 + header[4]*4 + header[5]
    spirv = bundle[offset:]
    if len(spirv) != header[6] or hashlib.sha256(spirv).hexdigest() != digest:
        raise ValueError('Adreno compatibility: unverified shader input')
    optimizer = os.environ.get('SPIRV_OPT') or shutil.which('spirv-opt')
    validator = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')
    if not optimizer or not validator:
        raise ValueError('Adreno compatibility requires spirv-opt and spirv-val')
    with tempfile.TemporaryDirectory(prefix='dm-adreno-') as work:
        source, output = Path(work)/'source.spv', Path(work)/'normalized.spv'
        source.write_bytes(spirv)
        subprocess.run([optimizer, '-O', str(source), '-o', str(output)], check=True, capture_output=True)
        subprocess.run([validator, '--target-env', 'vulkan1.1', str(output)], check=True, capture_output=True)
        spirv = output.read_bytes()
    header[6] = len(spirv)
    return struct.pack('<8I', *header) + bundle[32:offset] + spirv
