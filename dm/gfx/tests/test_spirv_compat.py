import importlib.util
from pathlib import Path
import struct
import unittest

def module(name):
    path = Path(__file__).resolve().parents[1]/'tools'/(name+'.py')
    spec = importlib.util.spec_from_file_location(name,path)
    obj = importlib.util.module_from_spec(spec); spec.loader.exec_module(obj); return obj

compat = module('spirv_compat')
fluid = module('fluid_compat')
adreno = module('adreno_compat')

def fixture(member=0):
    # A minimal instruction stream sufficient to check struct access analysis.
    instructions = [(22,[1,32]), (23,[2,1,4]), (21,[3,32,0]), (43,[3,4,member]),
                    (30,[5,2,1,1,1]), (32,[6,3,5]), (32,[7,3,1]),
                    (72,[5,3,11,4]), (6,[5,3,0]), (59,[6,8,3]), (65,[7,9,8,4])]
    w=[0x07230203,0x10000,0,10,0]
    for op,args in instructions:w.extend([((len(args)+1)<<16)|op,*args])
    return struct.pack('<%dI'%len(w),*w)

class CompatibilityTests(unittest.TestCase):
    def test_unused_trailing_member_removed_and_idempotent(self):
        original=fixture();fixed=compat.remove_unused_cull_distance(original)
        self.assertLess(len(fixed),len(original))
        self.assertEqual(compat.remove_unused_cull_distance(fixed),fixed)
    def test_real_cull_use_is_not_silently_removed(self):
        with self.assertRaisesRegex(ValueError,'is used'):compat.remove_unused_cull_distance(fixture(3))
    def test_unknown_shader_unchanged(self):
        self.assertEqual(fluid.replace('unknown',b'not a known shader'),b'not a known shader')
    def test_adreno_unknown_input_fails_closed(self):
        self.assertEqual(adreno.replace('unknown', b'unrelated'), b'unrelated')
        for key in adreno.CONTRACTS:
            with self.assertRaises(ValueError):adreno.replace(key, b'bad')
        header=struct.pack('<8I',0x31534758,1,5,6,0,256,20,0)
        with self.assertRaisesRegex(ValueError,'unverified'):adreno.replace(adreno.KEY,header+bytes(192+256+20))
    def test_known_shader_contract_must_match(self):
        with self.assertRaises(ValueError):fluid.replace(fluid.KEY,b'bad')
        header=struct.pack('<8I',0x31534758,1,5,4,0,0,0,0)
        with self.assertRaisesRegex(ValueError,'descriptor mismatch'):fluid.replace(fluid.KEY,header+bytes(128))

if __name__=='__main__':unittest.main()
