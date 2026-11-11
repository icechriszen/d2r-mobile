"""Vulkan-only interface fixes. The locked Apple translation chain is unchanged."""
import struct

def remove_unused_cull_distance(data):
    words = list(struct.unpack('<%dI' % (len(data)//4), data))
    instructions = []
    i = 5
    while i < len(words):
        count = words[i] >> 16
        if not count or i+count > len(words):
            raise ValueError('invalid SPIR-V instruction')
        instructions.append((words[i] & 65535, words[i+1:i+count]))
        i += count
    types = {a[0]: (op,a[1:]) for op,a in instructions if 19 <= op <= 39}
    constants = {a[1]: a[2] for op,a in instructions if op == 43 and len(a) == 3}
    # Decoration BuiltIn=11; BuiltIn CullDistance=4. Only the translator's
    # unused final member can be removed without renumbering real fields.
    targets = {a[0]: a[1] for op,a in instructions if op == 72 and a[2:] == [11,4]}
    if not targets:
        return data
    for tid, member in targets.items():
        if types.get(tid,(0,[]))[0] != 30 or member != len(types[tid][1])-1:
            raise ValueError('CullDistance is not a trailing struct member')
    value_types = {a[1]:a[0] for op,a in instructions if len(a)>1 and a[0] in types and not 19 <= op <= 39}
    for op,a in instructions:
        if op in (65,66): # AccessChain / InBoundsAccessChain
            pointer = types.get(value_types.get(a[2]),(0,[]))
            if pointer[0] != 32:
                raise ValueError('unknown access-chain base type')
            tid = pointer[1][1]
            for index in a[3:]:
                kind, members = types.get(tid,(0,[]))
                if kind == 30:
                    if index not in constants:
                        raise ValueError('dynamic struct member index')
                    at = constants[index]
                    if tid in targets and at == targets[tid]:
                        raise ValueError('CullDistance is used: cannot remove')
                    tid = members[at]
                elif kind in (23,24,28,29): tid = members[0]
                else: raise ValueError('unsupported access-chain type')
        elif op in (67,70):
            # PtrAccessChain is not emitted by this translator; fail closed.
            raise ValueError('pointer arithmetic needs CullDistance usage analysis')
        elif len(a)>1 and a[0] in targets and op not in (5,6,30,71,72):
            raise ValueError('whole per-vertex struct value is used')
    out = words[:5]
    for op,a in instructions:
        if op in (6,72) and a[0] in targets and a[1] == targets[a[0]]: continue
        if op == 30 and a[0] in targets: a = a[:-1]
        out.extend([((len(a)+1)<<16)|op, *a])
    return struct.pack('<%dI' % len(out), *out)
