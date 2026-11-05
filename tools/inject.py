#!/usr/bin/env python3
"""Wire libRBMod.dylib into a Retro Bowl 1.6.49 binary.

  inject.py <in RetroBowl binary> <out binary>

- adds LC_LOAD_WEAK_DYLIB @executable_path/Frameworks/libRBMod.dylib
- writes one stub per HOOK() line in src/hooks.def (plus the custom stubs below) into an unused
  function, and points each hook site at its stub.

Entry-hook stub k (at RB_STUB_VA + 24*k):
    adrp x16, TAB ; ldr x16, [x16, #slot] ; cbz x16, tramp ; br x16
    tramp: <displaced first instruction> ; b entry+4
If the dylib is missing, the table stays zero and every stub falls straight through to the original.
The game's instance/register state is untouched: the C hook receives the original arguments.
Needs llvm-mc on PATH.
"""
import re, struct, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RB_H = (ROOT / 'src/rb.h').read_text()

def const(name):
    m = re.search(rf'#define\s+{name}\s+(0x[0-9a-fA-F]+|\d+)', RB_H)
    return int(m.group(1), 0)

def enum(name):
    return int(re.search(rf'\b{name}\s*=\s*(\d+)', RB_H).group(1))

TAB_VA, STUB_VA, STUB_SIZE = const('RB_TAB_VA'), const('RB_STUB_VA'), const('RB_STUB_SIZE')
HOOK_BASE, GATE_ATTACK = enum('TAB_HOOK_BASE'), enum('TAB_GATE_ATTACK')
STUB_AREA = 3672                       # size of gml_Script_s_debug_init_team_ranking
CUSTOM_VA = STUB_VA + 0x800            # custom (mid-function) stubs live here
DYLIB = b'@executable_path/Frameworks/libRBMod.dylib'

# ---------------------------------------------------------------- Mach-O
src, dst = sys.argv[1], sys.argv[2]
data = bytearray(Path(src).read_bytes())
assert struct.unpack_from('<I', data, 0)[0] == 0xfeedfacf
ncmds, sizeofcmds = struct.unpack_from('<II', data, 16)
segs, off = [], 32
first_sect = None
for _ in range(ncmds):
    cmd, sz = struct.unpack_from('<II', data, off)
    if cmd == 0x19:
        name = data[off + 8:off + 24].rstrip(b'\0').decode()
        vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', data, off + 24)
        segs.append((name, vmaddr, vmsize, fileoff, filesize))
        nsect = struct.unpack_from('<I', data, off + 64)[0]
        for s in range(nsect):
            o = struct.unpack_from('<I', data, off + 72 + s * 80 + 48)[0]
            if o and (first_sect is None or o < first_sect): first_sect = o
    if cmd in (0x0c, 0x80000018) and DYLIB in data[off:off + sz]:
        sys.exit('already injected')
    off += sz

def va2off(va):
    for name, vmaddr, vmsize, fileoff, filesize in segs:
        if vmaddr <= va < vmaddr + filesize: return fileoff + va - vmaddr
    raise ValueError(hex(va))

def get(va): return struct.unpack_from('<I', data, va2off(va))[0]
def put(va, w): struct.pack_into('<I', data, va2off(va), w)

# table must sit in __DATA's zero-fill tail
d = [s for s in segs if s[0] == '__DATA'][0]
assert d[1] + d[4] <= TAB_VA and TAB_VA + 0x400 <= d[1] + d[2], 'table outside __DATA zerofill'

# ---------------------------------------------------------------- encoders
def mc(ins):
    out = subprocess.run(['llvm-mc', '-triple=aarch64', '-show-encoding'], input=ins,
                         capture_output=True, text=True, check=True).stdout
    enc = out.split('encoding: [')[1].split(']')[0]
    return struct.unpack('<I', bytes(int(x, 16) for x in enc.split(',')))[0]

def b(pc, tgt, link=False):
    o = tgt - pc; assert o % 4 == 0 and -(1 << 27) <= o < (1 << 27)
    return (0x94000000 if link else 0x14000000) | ((o >> 2) & 0x3ffffff)

def bcond(pc, tgt, cond):
    o = tgt - pc; assert -(1 << 20) <= o < (1 << 20)
    return 0x54000000 | (((o >> 2) & 0x7ffff) << 5) | cond

def cbz(pc, tgt, rt, x=False):
    o = tgt - pc; assert -(1 << 20) <= o < (1 << 20)
    return (0xB4000000 if x else 0x34000000) | (((o >> 2) & 0x7ffff) << 5) | rt

def adrp(pc, tgt, rd):
    imm = ((tgt & ~0xfff) - (pc & ~0xfff)) >> 12
    return 0x90000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7ffff) << 5) | rd

def ldr_x(rt, rn, imm):
    assert imm % 8 == 0 and imm < 32768
    return 0xF9400000 | ((imm // 8) << 10) | (rn << 5) | rt

BR_X16, BLR_X16 = 0xD61F0200, 0xD63F0200

def load_slot(pc, index):          # 2 words: x16 = TAB[index]
    a = TAB_VA + 8 * index
    return [adrp(pc, a, 16), ldr_x(16, 16, a & 0xfff)]

PC_REL = re.compile(r'^(adr|adrp|b|bl|b\.\w+|cbz|cbnz|tbz|tbnz|ldr\s+\w+,\s*0x|ldrsw\s+\w+,\s*0x|prfm\s+\w+,\s*0x)\b')

def write_words(va, words):
    for i, w in enumerate(words): put(va + 4 * i, w)

# ---------------------------------------------------------------- entry hooks
hooks = []
for line in (ROOT / 'src/hooks.def').read_text().splitlines():
    m = re.match(r'\s*HOOK\(\s*(\w+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*"([^"]+)"\s*\)', line)
    if m: hooks.append((m.group(1), int(m.group(2), 16), m.group(3)))

assert STUB_VA + STUB_SIZE * len(hooks) <= CUSTOM_VA, 'too many hooks'
for k, (name, entry, insn) in enumerate(hooks):
    assert not PC_REL.match(insn), f'{name}: first instruction is PC-relative'
    orig = get(entry)
    assert orig == mc(insn), f'{name} @ {entry:#x}: expected "{insn}", found {orig:#010x} (already patched?)'
    s = STUB_VA + STUB_SIZE * k
    w = load_slot(s, HOOK_BASE + k)
    w += [cbz(s + 8, s + 16, 16, x=True), BR_X16, orig, b(s + 20, entry + 4)]
    assert len(w) * 4 == STUB_SIZE
    write_words(s, w)
    put(entry, b(entry, s))
    print(f'hook {name:24s} {entry:#x} -> stub {s:#x}')

# ---------------------------------------------------------------- custom stub: ATTACK-stage gate
# s_update_commentary, after yy_compare(possession, obj_master.playerteamindex): w0 == 0 -> equal
#   0x1005ad2d0  cbz w0, FIELD        (replaced by  b gate)
# C:  int rb_gate_attack(CInstance *controller, int human_has_ball)  -> 0 field / 1 continue / 2 sim
GATE_SITE, FIELD, CONT, SIM = 0x1005ad2d0, 0x1005ad31c, 0x1005ad2d4, 0x1005b0d30
assert get(GATE_SITE) == cbz(GATE_SITE, FIELD, 0), 'gate site changed'
assert get(FIELD) == mc('mov w8, #0x121') and get(SIM) == mc('mov w8, #0x13a')

g = CUSTOM_VA
L = {}
code = []                          # (kind, payload)
def emit(x): code.append(x)
emit(('slot', GATE_ATTACK))        # 2 words
emit(('cbzx16', 'orig'))
emit(mc('cmp w0, #0'))
emit(mc('cset w1, eq'))            # human_has_ball
emit(mc('ldr x0, [sp, #0x568]'))   # self = obj_controller (hub's frame)
emit(BLR_X16)
emit(('cbzw0', 'field'))
emit(mc('cmp w0, #2'))
emit(('beq', 'sim'))
emit(('b', CONT))
emit(('label', 'orig'))
emit(('cbzw0', 'field'))
emit(('b', CONT))
emit(('label', 'field'))
emit(('b', FIELD))
emit(('label', 'sim'))
emit(('b', SIM))

pc = g
for c in code:                     # pass 1: labels
    if isinstance(c, tuple) and c[0] == 'label': L[c[1]] = pc; continue
    pc += 8 if isinstance(c, tuple) and c[0] == 'slot' else 4
words, pc = [], g
for c in code:                     # pass 2: encode
    if isinstance(c, int): words.append(c); pc += 4; continue
    k, p = c
    if k == 'label': continue
    if k == 'slot': words += load_slot(pc, p); pc += 8; continue
    if k == 'cbzx16': words.append(cbz(pc, L[p], 16, x=True))
    elif k == 'cbzw0': words.append(cbz(pc, L[p], 0))
    elif k == 'beq': words.append(bcond(pc, L[p], 0))
    elif k == 'b': words.append(b(pc, p))
    pc += 4
assert pc <= STUB_VA + STUB_AREA
write_words(g, words)
put(GATE_SITE, b(GATE_SITE, g))
print(f'gate  attack                   {GATE_SITE:#x} -> stub {g:#x}')

# ---------------------------------------------------------------- LC_LOAD_WEAK_DYLIB
name = DYLIB + b'\0'
cmdsize = (24 + len(name) + 7) & ~7
lc = struct.pack('<IIIIII', 0x80000018, cmdsize, 24, 2, 0x10000, 0x10000) + name
lc += b'\0' * (cmdsize - len(lc))
end = 32 + sizeofcmds
assert end + cmdsize <= first_sect and not any(data[end:end + cmdsize]), 'no header room'
data[end:end + cmdsize] = lc
struct.pack_into('<II', data, 16, ncmds + 1, sizeofcmds + cmdsize)
print(f'load  {DYLIB.decode()}')

Path(dst).write_bytes(data)
