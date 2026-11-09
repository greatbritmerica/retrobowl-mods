"""Retro Bowl 1.6.49 (iOS, YYC) - scramble-then-pass patch.

Double-tap while a scrambling QB is still behind the line of scrimmage -> calls the game's own
(unused) gml_Script_s_qb_revert_throw, which turns him back into a passer (position 1, ball state 0).
Then touch the QB and drag back to throw as normal.

Usage: patch_scramble.py <in RetroBowl binary> <out binary>
"""
import struct, subprocess, sys
sys.path.insert(0, '/home/claude/tools')
from macho import MachO

SRC, DST = sys.argv[1], sys.argv[2]
m = MachO(SRC)
data = bytearray(m.data)

# ---- addresses (v1.6.49) ----
S_CAN_THROW        = 0x1001097d4
CT_POS_CHECK_TBZ   = 0x1001098dc   # tbz w0,#0,FAIL   (after position == 1 compare)
CT_OK              = 0x1001098e0
CT_FAIL            = 0x10010997c
S_QB_REVERT_THROW  = 0x1004d0270
GESTURE_65         = 0x1006e3268   # gml_Object_obj_controller_Gesture_65 (global double tap): just `ret`
RVALUE_EQ_INT      = 0x1000086e8   # bool (RValue*, int)
RVALUE_BOOL        = 0x1008797f8   # bool (RValue*)
SLOT_QB_RUSHING    = 0x100fa7888   # {int id} record for variable 'qb_rushing'
CAVE               = 0x1003fe62c   # gml_Script_s_debug_up_team_offense (never called) - 2744 bytes
MAGIC              = 0x52424D4F44534352
G_GLOBAL_INST_PTR  = 0x1010c82d8   # CInstance* of the global object (as used by s_can_throw)
SLOT_MASTER_BALL   = 0x100fa6a38
WITH_BEGIN         = 0x100bcffa0   # (iter*, self**, other**, RValue* target) -> count
WITH_NEXT          = 0x100bd0374
WITH_END           = 0x100bd03a4
GLOBAL_INST_PTR    = 0x1010c82d8   # CInstance* g_pGlobal (as used in s_can_throw)
SLOT_MASTER_BALL   = 0x100fa6a38   # slot record for 'master_ball'
WITH_BEGIN         = 0x100bcffa0   # (iter*, self**, other**, RValue* target) -> count
WITH_NEXT          = 0x100bd0374   # (iter*, self**, other**) -> more
WITH_END           = 0x100bd03a4   # (iter*, self**, other**)
WITH_DTOR          = 0x10000dea4   # (iter*)
S5_HANDLER         = 0x1005a4ca4   # s_update_ball: handler for ball state 5 (and 9,10,19,20,21): `mov w8,#0x12c`
S18_TOUCH_BLOCK    = 0x1005a4270   # s_update_ball: 'QB has ball' block -> touch on QB => state 1 (aiming)
SLOT_STATE         = 0x100faf518
FN_MOUSE_CHECK_BTN = 0x100fba2b0   # function-id slot for mouse_check_button
FN_POINT_DISTANCE  = 0x100fb99b0   # function-id slot for point_distance
SLOT_X, SLOT_Y     = 0x100fb07e8, 0x100fb0808
SLOT_MOUSE_X       = 0x100fb0a38
SLOT_MOUSE_Y       = 0x100fb0a48
CONST_MB_LEFT      = 0x100f9ea30   # RValue {1.0}
S_IS_TOUCH_ALLOWED = 0x1005d4ca4
CALL_BUILTIN       = 0x100bce4c8   # (self, other, RValue* res, argc, fn_id, RValue** args) -> res*
GET_INST_VAR       = 0x100bba9f0   # (RValue* inst, slot, INT_MIN, RValue* out, 0, 0)
GET_BUILTIN_VAR    = 0x100bb9fc0   # (CInstance* self, slot, INT_MIN, RValue* out)
RV_FREE            = 0x100004ea0
RV_COPY            = 0x10000684c   # (dst, src)
SET_REF_VAR        = 0x100bbab88   # (RValue* ref, slot, INT_MIN, RValue* value)  e.g. obj.running_play = v
SLOT_RUNNING_PLAY  = 0x100fa8138
OBJ44_REF          = 0x010000000000002c   # RValue payload for the object that owns running_play (kind 15 = ref)
SLOT_LAST_TOUCHED  = 0x100fa66a8   # ball.last_touched_by (noone = -4 means a dead ball when it lands)
RV_FREE_CONTENT    = 0x100004c9c
SLOT_HOLDER        = 0x100fa5d78   # ball.holder
S_THROW_BALL       = 0x10058c728   # entry: stp d9, d8, [sp, #-0x70]!
SLOT_OP_DIFFICULTY = 0x100fa71d8   # obj_master.op_difficulty (4 = Extreme)
OBJ_MASTER_REF     = 0x0100000000000058   # ref -> obj_master (88)
OBJ_PLAYERDF_REF   = 0x0100000000000076   # ref -> obj_playerDF (118)
SLOT_ID            = 0x100fb0858
SLOT_TACKLING      = 0x100faf788
SLOT_TACKLE_TIME   = 0x100faf768
YY_COMPARE         = 0x100b68d3c   # int (RValue* a, RValue* b, int flag), d0 = epsilon -> 0 if equal
EPSILON_VA         = 0x101050828
SACK_PCT           = 45
HIT_RADIUS         = 16            # defender within this distance of the QB at release = 'hit'
SLOT_ANIM          = 0x100fa1a88
SLOT_CURRENT_TIME  = 0x100fb0898
ANIM_TACKLED       = 0x4010000000000000  # 4.0 : falling/tackled animation (Animation End -> dead)            # % of windup hits that become a sack (rest: pass gets off)
S_RECEIVE_BALL     = 0x1004d0974   # entry: stp d9, d8, [sp, #-0x70]!  (self = catcher)
SLOT_OFFENSE       = 0x100fa70f8
WRAP_PCT           = 75            # % of contested catches (defender within reach) that are wrapped up immediately
CAVE2              = 0x10009beb0   # gml_Script_s_debug_init_menu (never called) - 10000 bytes
S_AIM_AI_DEFENSE   = 0x1000ff5f4   # entry: stp d11, d10, [sp, #-0x80]!   (self = defender)
SLOT_JDIR          = 0x100fa6238
SLOT_JFORCE        = 0x100fa6258
SLOT_XRECEIVED     = 0x100fb05f8
SLOT_YRECEIVED     = 0x100fb0748
GOT_ATAN2          = 0x100f0ce88
REACT_WINDOW       = 60    # carrier within this distance of the catch spot = 'just caught it'
REACT_RADIUS       = 160   # defenders within this distance of the carrier lock on immediately
S_CHANGE_POSSESSION= 0x10010aca8   # entry: sub sp, sp, #0x150
SLOT_HUMAN_TEAM    = 0x100fa5de8
SLOT_POSSESSION    = 0x100fa7668
FOLLOW = '--follow' in sys.argv     # experiment: human_team follows possession (every drive played)
RV_LT_INT          = 0x1000082c8   # bool (RValue* a, int b)  a < b
S_SOUND_POST       = 0x100572dbc   # diag: goalpost clang
S_SOUND_OOF        = 0x100572384   # diag: grunt
DIAG = '--diag' in sys.argv  # "RCSDOMBR" marker kept in x28 during our s_can_throw call

def expect(va, hexword):
    o = m.va2off(va)
    got = struct.unpack_from('<I', data, o)[0]
    assert got == hexword, f'{va:#x}: expected {hexword:#010x} got {got:#010x}'

def put(va, word):
    struct.pack_into('<I', data, m.va2off(va), word)

def mc(ins):
    out = subprocess.run(['llvm-mc', '-triple=aarch64', '-show-encoding'], input=ins,
                         capture_output=True, text=True, check=True).stdout
    enc = out.split('encoding: [')[1].split(']')[0]
    b = bytes(int(x, 16) for x in enc.split(','))
    return struct.unpack('<I', b)[0]

def enc_b(pc, tgt, link=False):
    off = tgt - pc; assert off % 4 == 0 and -(1 << 27) <= off < (1 << 27)
    return (0x94000000 if link else 0x14000000) | ((off >> 2) & 0x3ffffff)

def enc_bcond(pc, tgt, cond):
    off = tgt - pc; assert -(1 << 20) <= off < (1 << 20)
    return 0x54000000 | (((off >> 2) & 0x7ffff) << 5) | cond

def enc_cbz_w(pc, tgt, rt):
    off = tgt - pc; assert -(1 << 20) <= off < (1 << 20)
    return 0x34000000 | (((off >> 2) & 0x7ffff) << 5) | rt

def enc_cbz_x(pc, tgt, rt):
    off = tgt - pc; assert -(1 << 20) <= off < (1 << 20)
    return 0xB4000000 | (((off >> 2) & 0x7ffff) << 5) | rt

def enc_tbz_w(pc, tgt, rt, bit, nz=False):
    off = tgt - pc; assert bit < 32 and -(1 << 15) <= off < (1 << 15), 'tbz out of range'
    return (0x37000000 if nz else 0x36000000) | (bit << 19) | (((off >> 2) & 0x3fff) << 5) | rt

def enc_adrp(pc, tgt, rd):
    imm = ((tgt & ~0xfff) - (pc & ~0xfff)) >> 12
    return 0x90000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7ffff) << 5) | rd

# ---- tiny assembler with labels ----
class Asm:
    def __init__(self, base):
        self.base, self.items, self.labels = base, [], {}
    def pc(self): return self.base + 4 * len(self.items)
    def label(self, n): self.labels[n] = self.pc()
    def i(self, s): self.items.append(('mc', s))
    def raw(self, fn): self.items.append(('fn', fn))       # fn(pc, labels) -> word
    def build(self):
        out = []
        for k, (kind, v) in enumerate(self.items):
            pc = self.base + 4 * k
            out.append(mc(v) if kind == 'mc' else v(pc, self.labels))
        return out

def movabs(a, reg, val):
    a.i(f'movz {reg}, #{val & 0xffff}')
    for sh in (16, 32, 48):
        a.i(f'movk {reg}, #{(val >> sh) & 0xffff}, lsl #{sh}')

a = Asm(CAVE)

def call0(target):
    a.i('str xzr, [sp]')
    a.i('str xzr, [sp, #8]')
    a.i('mov w8, #5')
    a.i('str w8, [sp, #0xc]')
    a.i('mov x0, x19')
    a.i('mov x1, x20')
    a.i('mov x2, sp')
    a.i('mov w3, #0')
    a.i('mov x4, #0')
    a.raw(lambda pc, L, t=target: enc_b(pc, t, link=True))


# ===== cave_ct : replaces `tbz w0,#0,FAIL` inside s_can_throw =====
a.label('cave_ct')
movabs(a, 'x9', MAGIC)
a.i('cmp x28, x9')
a.raw(lambda pc, L: enc_bcond(pc, L['ct_magic'], 0))          # b.eq ct_magic
a.raw(lambda pc, L: enc_tbz_w(pc, L['ct_fail'], 0, 0))         # tbz w0,#0,FAIL  (original behaviour)
a.raw(lambda pc, L: enc_b(pc, CT_OK))                          # b OK
a.label('ct_magic')
# with(holder): self instance is at [x29,#-0x48]; read qb_rushing and require == 1
a.i('ldur x0, [x29, #-0x48]')
a.raw(lambda pc, L: enc_adrp(pc, SLOT_QB_RUSHING, 8))          # adrp x8, slot page
a.i(f'ldr w1, [x8, #{SLOT_QB_RUSHING & 0xfff}]')
a.i('ldr x8, [x0]')
a.i('ldr x8, [x8, #0x10]')
a.i('blr x8')
a.i('mov w1, #1')
a.raw(lambda pc, L: enc_b(pc, RVALUE_EQ_INT, link=True))       # bl RValue == int
a.raw(lambda pc, L: enc_tbz_w(pc, L['ct_fail'], 0, 0))         # not rushing -> FAIL
a.raw(lambda pc, L: enc_b(pc, CT_OK))                          # rushing -> continue to LOS check
a.label('ct_fail')
a.raw(lambda pc, L: enc_b(pc, CT_FAIL))                        # far branch to original FAIL path

# ===== cave_s5 : runs at the top of s_update_ball's state-5 handler (self = ball) =====
# if state==5 && scrambling QB behind LOS (magic s_can_throw) && s_is_touch_allowed()
#    && mouse_check_button(mb_left) && point_distance(x,y,mouse_x,mouse_y) < 40:
#        s_qb_revert_throw(); goto S18_TOUCH_BLOCK   (game's own code then sets aiming state with xtouch/ytouch)
# else: run displaced instruction, continue original handler.
# Frame 0xd0: 0 res | 0x10 arg | 0x20 inst | 0x30 a1 | 0x40 a2 | 0x50 a3 | 0x60 a4 | 0x70.. argv[4]
#             0xa0 x19,x20 | 0xb0 x21,x28 | 0xc0 x30
def rv_undef(off):
    a.i(f'str xzr, [sp, #{off}]')
    a.i(f'str xzr, [sp, #{off + 8}]')
    a.i('mov w8, #5')
    a.i(f'str w8, [sp, #{off + 12}]')

def slot_w(reg, slot_va):
    a.raw(lambda pc, L, s=slot_va: enc_adrp(pc, s, 8))
    a.i(f'ldr {reg}, [x8, #{slot_va & 0xfff}]')

def script0(target):        # call a 0-arg GML script with self/other = x19/x20, result at [sp]
    rv_undef(0)
    a.i('mov x0, x19'); a.i('mov x1, x20'); a.i('mov x2, sp'); a.i('mov w3, #0'); a.i('mov x4, #0')
    a.raw(lambda pc, L, t=target: enc_b(pc, t, link=True))

def bl(t): a.raw(lambda pc, L, t=t: enc_b(pc, t, link=True))

a.label('cave_s5')
a.i('ldr x9, [sp, #0x3f8]')            # self (ball)   - s_update_ball locals
a.i('ldr x10, [sp, #0x3f0]')           # other
a.i('sub sp, sp, #0xd0')
a.i('stp x19, x20, [sp, #0xa0]')
a.i('stp x21, x28, [sp, #0xb0]')
a.i('str x30, [sp, #0xc0]')
a.i('mov x19, x9')
a.i('mov x20, x10')
# (a) self.state == 5
a.i('mov x0, x19')
slot_w('w1', SLOT_STATE)
a.i('ldr x8, [x0]'); a.i('ldr x8, [x8, #0x10]'); a.i('blr x8')
a.i('mov w1, #5')
bl(RVALUE_EQ_INT)
a.raw(lambda pc, L: enc_cbz_w(pc, L['s5_orig'], 0))
# (b) scrambling QB behind the line (s_can_throw in magic mode)
rv_undef(0)
movabs(a, 'x28', MAGIC)
a.i('mov x0, x19'); a.i('mov x1, x20'); a.i('mov x2, sp'); a.i('mov w3, #0'); a.i('mov x4, #0')
bl(S_CAN_THROW)
a.i('ldr x28, [sp, #0xb8]')
bl(RVALUE_BOOL)
a.raw(lambda pc, L: enc_cbz_w(pc, L['s5_orig'], 0))
# (c) s_is_touch_allowed()
script0(S_IS_TOUCH_ALLOWED)
bl(RVALUE_BOOL)
a.raw(lambda pc, L: enc_cbz_w(pc, L['s5_orig'], 0))
# (d) mouse_check_button(mb_left)
rv_undef(0x10)
a.i('add x0, sp, #0x10')
a.raw(lambda pc, L: enc_adrp(pc, CONST_MB_LEFT, 1))
a.i(f'add x1, x1, #{CONST_MB_LEFT & 0xfff}')
bl(RV_COPY)
a.i('add x8, sp, #0x10'); a.i('str x8, [sp, #0x70]')
rv_undef(0)
a.i('mov x0, x19'); a.i('mov x1, x20'); a.i('mov x2, sp'); a.i('mov w3, #1')
slot_w('w4', FN_MOUSE_CHECK_BTN)
a.i('add x5, sp, #0x70')
bl(CALL_BUILTIN)
bl(RVALUE_BOOL)
a.i('mov w21, w0')
a.i('add x0, sp, #0x10'); bl(RV_FREE)
a.raw(lambda pc, L: enc_cbz_w(pc, L['s5_orig'], 21))
# (e) point_distance(x, y, mouse_x, mouse_y) < 40
for off in (0x30, 0x40, 0x50, 0x60): rv_undef(off)
for off, slot in ((0x30, SLOT_X), (0x40, SLOT_Y)):
    a.i('str x19, [sp, #0x20]'); a.i('str wzr, [sp, #0x28]'); a.i('mov w8, #6'); a.i('str w8, [sp, #0x2c]')
    a.i('add x0, sp, #0x20')
    slot_w('w1', slot)
    a.i('mov w2, #-0x80000000')
    a.i(f'add x3, sp, #{off}')
    a.i('mov w4, #0'); a.i('mov w5, #0')
    bl(GET_INST_VAR)
    a.i('add x0, sp, #0x20'); bl(RV_FREE)
for off, slot in ((0x50, SLOT_MOUSE_X), (0x60, SLOT_MOUSE_Y)):
    a.i('mov x0, x19')
    slot_w('w1', slot)
    a.i('mov w2, #-0x80000000')
    a.i(f'add x3, sp, #{off}')
    bl(GET_BUILTIN_VAR)
for k, off in enumerate((0x30, 0x40, 0x50, 0x60)):
    a.i(f'add x8, sp, #{off}'); a.i(f'str x8, [sp, #{0x70 + 8 * k}]')
rv_undef(0)
a.i('mov x0, x19'); a.i('mov x1, x20'); a.i('mov x2, sp'); a.i('mov w3, #4')
slot_w('w4', FN_POINT_DISTANCE)
a.i('add x5, sp, #0x70')
bl(CALL_BUILTIN)
a.i('mov w1, #40')
bl(RV_LT_INT)
a.i('mov w21, w0')
for off in (0x30, 0x40, 0x50, 0x60):
    a.i(f'add x0, sp, #{off}'); bl(RV_FREE)
a.raw(lambda pc, L: enc_cbz_w(pc, L['s5_orig'], 21))
# (f) go: turn him back into a passer, then hand over to the game's own touch->aim code
if DIAG: script0(S_SOUND_OOF)
# holder.qb_rushing = 0  (before the revert, so its s_set_anim picks the normal passing-QB animation,
#                         and tackling/sack logic treats him as a passer, not a runner)
a.i('mov x0, x19')
slot_w('w1', SLOT_HOLDER)
a.i('ldr x8, [x0]'); a.i('ldr x8, [x8, #0x10]'); a.i('blr x8')     # x0 = &ball.holder
a.i('str xzr, [sp, #0x40]'); a.i('str xzr, [sp, #0x48]')           # value: real 0.0
slot_w('w1', SLOT_QB_RUSHING)
a.i('mov w2, #-0x80000000')
a.i('add x3, sp, #0x40')
bl(SET_REF_VAR)
script0(S_QB_REVERT_THROW)
# running_play = 0 (s_qb_run set it to 1; leaving it makes the pass a live 'lateral' / loose ball)
movabs(a, 'x8', OBJ44_REF)
a.i('str x8, [sp, #0x30]'); a.i('str wzr, [sp, #0x38]'); a.i('mov w8, #0xf'); a.i('str w8, [sp, #0x3c]')
a.i('str xzr, [sp, #0x40]'); a.i('str xzr, [sp, #0x48]')          # value: real 0.0 (kind 0)
a.i('add x0, sp, #0x30')
slot_w('w1', SLOT_RUNNING_PLAY)
a.i('mov w2, #-0x80000000')
a.i('add x3, sp, #0x40')
bl(SET_REF_VAR)
a.i('add x0, sp, #0x30'); bl(RV_FREE)
# self(ball).last_touched_by = noone (-4)   -- same sequence as obj_ball Create
a.i('mov x0, x19')
slot_w('w1', SLOT_LAST_TOUCHED)
a.i('ldr x8, [x0]'); a.i('ldr x8, [x8, #0x18]'); a.i('blr x8')
a.i('mov x21, x0')
a.i('ldr w8, [x0, #0xc]'); a.i('mov w9, #1'); a.i('lsl w8, w9, w8'); a.i('mov w9, #0x46'); a.i('tst w8, w9')
a.raw(lambda pc, L: enc_bcond(pc, L['ltb_store'], 0))       # b.eq -> nothing to free
a.i('mov x0, x21'); bl(RV_FREE_CONTENT)
a.label('ltb_store')
a.i('str wzr, [x21, #0xc]')
movabs(a, 'x8', 0xC010000000000000)                         # -4.0
a.i('str x8, [x21]')
a.i('ldp x19, x20, [sp, #0xa0]')
a.i('ldp x21, x28, [sp, #0xb0]')
a.i('ldr x30, [sp, #0xc0]')
a.i('add sp, sp, #0xd0')
a.raw(lambda pc, L: enc_b(pc, S18_TOUCH_BLOCK))
a.label('s5_orig')
a.i('ldp x19, x20, [sp, #0xa0]')
a.i('ldp x21, x28, [sp, #0xb0]')
a.i('ldr x30, [sp, #0xc0]')
a.i('add sp, sp, #0xd0')
a.i('mov w8, #0x12c')                  # displaced instruction
a.raw(lambda pc, L: enc_b(pc, S5_HANDLER + 4))

# ===== cave_tb : Extreme-only "hit during the throwing motion" =====
# At s_throw_ball entry (self = ball), only when op_difficulty == 4:
#   with(holder): remember QB ptr, x, y; if he is already in the tackled animation -> block.
#   with(obj_playerDF): if point_distance(x,y,qbx,qby) < HIT_RADIUS -> hit.
#   hit: roll current_time % 100 < SACK_PCT -> s_set_anim(4) on the QB and don't throw
#        (Animation End then marks him down -> game's own sack / fumble handling). Else throw.
# Frame 0x180: 0x10 scratch rv | 0x20 out | 0x30 qbx | 0x40 qby | 0x50 objref | 0x60 self* | 0x68 other*
#  0x70 iter | 0x90 QB* | 0x98 x4 | 0xa0 x19,x20 | 0xb0 x21,x22 | 0xc0 x23 | 0xd0 x0,x1 | 0xe0 x2,x3
#  0xf0 x29,x30 | 0x100 px | 0x110 py | 0x120 argv[4] | 0x140 inst rv | 0x150 arg rv | 0x160 res
F = 0x180
def inst_get(dst_off, slot, ptr_off):           # dst = (*(CInstance*)[sp+ptr_off]).slot   (built-ins like x/y)
    a.i(f'ldr x8, [sp, #{ptr_off}]'); a.i('str x8, [sp, #0x140]'); a.i('str wzr, [sp, #0x148]')
    a.i('mov w8, #6'); a.i('str w8, [sp, #0x14c]')
    a.i('add x0, sp, #0x140'); slot_w('w1', slot); a.i('mov w2, #-0x80000000')
    a.i(f'add x3, sp, #{dst_off}'); a.i('mov w4, #0'); a.i('mov w5, #0')
    bl(GET_INST_VAR)
    a.i('add x0, sp, #0x140'); bl(RV_FREE)

def with_setup(objref_or_none):
    a.i('str x19, [sp, #0x60]'); a.i('str x20, [sp, #0x68]')
    a.i('add x0, sp, #0x70'); a.i('add x1, sp, #0x60'); a.i('add x2, sp, #0x68')

a.label('cave_tb')
a.i(f'sub sp, sp, #{F}')
a.i('stp x29, x30, [sp, #0xf0]')
a.i('add x29, sp, #0xf0')
a.i('stp x19, x20, [sp, #0xa0]')
a.i('stp x21, x22, [sp, #0xb0]')
a.i('str x23, [sp, #0xc0]')
a.i('stp x0, x1, [sp, #0xd0]')
a.i('stp x2, x3, [sp, #0xe0]')
a.i('str x4, [sp, #0x98]')
a.i('mov x19, x0')
a.i('mov x20, x1')
# (1) Extreme?
movabs(a, 'x8', OBJ_MASTER_REF)
a.i('str x8, [sp, #0x10]'); a.i('str wzr, [sp, #0x18]'); a.i('mov w8, #0xf'); a.i('str w8, [sp, #0x1c]')
rv_undef(0x20)
a.i('add x0, sp, #0x10'); slot_w('w1', SLOT_OP_DIFFICULTY)
a.i('mov w2, #-0x80000000'); a.i('add x3, sp, #0x20'); a.i('mov w4, #0'); a.i('mov w5, #0')
bl(GET_INST_VAR)
a.i('add x0, sp, #0x10'); bl(RV_FREE)
a.i('add x0, sp, #0x20'); a.i('mov w1, #4'); bl(RVALUE_EQ_INT)
a.i('mov w21, w0')
a.i('add x0, sp, #0x20'); bl(RV_FREE)
a.raw(lambda pc, L: enc_cbz_w(pc, L['tb_orig'], 21))
# (1b) libRBMod's "no windup sack" flag (scramble throws): table slot 2 holds an int* (0 = no mod loaded)
TAB_NO_WINDUP = 0x101353400 + 8 * 2
a.raw(lambda pc, L: enc_adrp(pc, TAB_NO_WINDUP, 8))
a.i(f'ldr x8, [x8, #{TAB_NO_WINDUP & 0xfff}]')
a.raw(lambda pc, L: enc_cbz_x(pc, L['tb_flag_off'], 8))
a.i('ldr w8, [x8]')
a.raw(lambda pc, L: enc_cbz_w(pc, L['tb_flag_off'], 8))
a.raw(lambda pc, L: enc_b(pc, L['tb_orig']))
a.label('tb_flag_off')
if DIAG: script0(S_SOUND_POST)
# (2) with (holder): QB ptr, x, y, already-tackled?
a.i('str xzr, [sp, #0x90]')
a.i('mov w22, #0')                               # w22 = already in tackled anim
rv_undef(0x30); rv_undef(0x40)
a.i('mov x0, x19'); slot_w('w1', SLOT_HOLDER)
a.i('ldr x8, [x0]'); a.i('ldr x8, [x8, #0x10]'); a.i('blr x8')
a.i('mov x3, x0')
with_setup(None)
bl(WITH_BEGIN)
a.i('cmp w0, #1')
a.raw(lambda pc, L: enc_bcond(pc, L['tb_h_end'], 0xb))
a.i('ldr x8, [sp, #0x60]'); a.i('str x8, [sp, #0x90]')
inst_get(0x30, SLOT_X, 0x60)
inst_get(0x40, SLOT_Y, 0x60)
a.i('ldr x0, [sp, #0x60]'); slot_w('w1', SLOT_ANIM)
a.i('ldr x8, [x0]'); a.i('ldr x8, [x8, #0x10]'); a.i('blr x8')
a.i('mov w1, #4'); bl(RVALUE_EQ_INT)
a.i('mov w22, w0')
a.label('tb_h_end')
a.i('add x0, sp, #0x70'); a.i('add x1, sp, #0x60'); a.i('add x2, sp, #0x68'); bl(WITH_END)
a.i('add x0, sp, #0x70'); bl(WITH_DTOR)
a.i('ldr x8, [sp, #0x90]')
a.raw(lambda pc, L: enc_cbz_x(pc, L['tb_orig_xy'], 8))    # no holder -> original
a.raw(lambda pc, L: enc_tbz_w(pc, L['tb_check'], 22, 0))
a.raw(lambda pc, L: enc_b(pc, L['tb_block']))             # already falling -> keep blocking
# (3) with (obj_playerDF): hit if point_distance < HIT_RADIUS
a.label('tb_check')
a.i('mov w21, #0')
movabs(a, 'x8', OBJ_PLAYERDF_REF)
a.i('str x8, [sp, #0x50]'); a.i('str wzr, [sp, #0x58]'); a.i('mov w8, #0xf'); a.i('str w8, [sp, #0x5c]')
a.i('add x3, sp, #0x50')
with_setup(None)
bl(WITH_BEGIN)
a.i('cmp w0, #1')
a.raw(lambda pc, L: enc_bcond(pc, L['tb_d_end'], 0xb))
a.label('tb_d_loop')
rv_undef(0x100); rv_undef(0x110)
inst_get(0x100, SLOT_X, 0x60)
inst_get(0x110, SLOT_Y, 0x60)
for k, off in enumerate((0x100, 0x110, 0x30, 0x40)):
    a.i(f'add x8, sp, #{off}'); a.i(f'str x8, [sp, #{0x120 + 8 * k}]')
rv_undef(0x160)
a.i('ldr x0, [sp, #0x60]'); a.i('ldr x1, [sp, #0x68]'); a.i('add x2, sp, #0x160'); a.i('mov w3, #4')
slot_w('w4', FN_POINT_DISTANCE)
a.i('add x5, sp, #0x120')
bl(CALL_BUILTIN)
a.i(f'mov w1, #{HIT_RADIUS}')
bl(RV_LT_INT)
a.i('orr w21, w21, w0')
a.i('add x0, sp, #0x100'); bl(RV_FREE)
a.i('add x0, sp, #0x110'); bl(RV_FREE)
a.i('add x0, sp, #0x70'); a.i('add x1, sp, #0x60'); a.i('add x2, sp, #0x68'); bl(WITH_NEXT)
a.raw(lambda pc, L: enc_tbz_w(pc, L['tb_d_loop'], 0, 0, nz=True))
a.label('tb_d_end')
a.i('add x0, sp, #0x70'); a.i('add x1, sp, #0x60'); a.i('add x2, sp, #0x68'); bl(WITH_END)
a.i('add x0, sp, #0x70'); bl(WITH_DTOR)
a.i('add x0, sp, #0x50'); bl(RV_FREE)
a.i('and w21, w21, #1')
a.raw(lambda pc, L: enc_cbz_w(pc, L['tb_orig_xy'], 21))
if DIAG: script0(S_SOUND_OOF)
# (4) roll: current_time % 100
rv_undef(0x20)
a.i('mov x0, x19'); slot_w('w1', SLOT_CURRENT_TIME); a.i('mov w2, #-0x80000000'); a.i('add x3, sp, #0x20')
bl(GET_BUILTIN_VAR)
a.i('ldr d0, [sp, #0x20]')
a.i('fabs d0, d0')
a.i('fcvtzu x8, d0')
a.i('mov x9, #100'); a.i('udiv x10, x8, x9'); a.i('msub x8, x10, x9, x8')
a.i('mov x23, x8')
a.i('add x0, sp, #0x20'); bl(RV_FREE)
a.i(f'cmp x23, #{SACK_PCT}')
a.raw(lambda pc, L: enc_bcond(pc, L['tb_orig_xy'], 0xa))  # b.ge -> pass gets off
# (5) sack: s_set_anim(4) on the QB
a.i('str xzr, [sp, #0x150]'); a.i('str xzr, [sp, #0x158]')
movabs(a, 'x8', ANIM_TACKLED)
a.i('str x8, [sp, #0x150]')
a.i('add x8, sp, #0x150'); a.i('str x8, [sp, #0x120]')
rv_undef(0x160)
a.i('ldr x0, [sp, #0x90]'); a.i('mov x1, x19'); a.i('add x2, sp, #0x160'); a.i('mov w3, #1'); a.i('add x4, sp, #0x120')
bl(0x1004fb230)                                  # gml_Script_s_set_anim
a.i('add x0, sp, #0x160'); bl(RV_FREE)
a.label('tb_block')
a.i('add x0, sp, #0x30'); bl(RV_FREE)
a.i('add x0, sp, #0x40'); bl(RV_FREE)
a.i('ldr x2, [sp, #0xe0]')
a.i('str xzr, [x2]'); a.i('str xzr, [x2, #8]'); a.i('mov w8, #5'); a.i('str w8, [x2, #0xc]')
a.i('mov x0, x2')
a.i('ldp x19, x20, [sp, #0xa0]'); a.i('ldp x21, x22, [sp, #0xb0]'); a.i('ldr x23, [sp, #0xc0]')
a.i('ldp x29, x30, [sp, #0xf0]')
a.i(f'add sp, sp, #{F}')
a.i('ret')
a.label('tb_orig_xy')
a.i('add x0, sp, #0x30'); bl(RV_FREE)
a.i('add x0, sp, #0x40'); bl(RV_FREE)
a.label('tb_orig')
a.i('ldp x0, x1, [sp, #0xd0]'); a.i('ldp x2, x3, [sp, #0xe0]'); a.i('ldr x4, [sp, #0x98]')
a.i('ldp x19, x20, [sp, #0xa0]'); a.i('ldp x21, x22, [sp, #0xb0]'); a.i('ldr x23, [sp, #0xc0]')
a.i('ldp x29, x30, [sp, #0xf0]')
a.i(f'add sp, sp, #{F}')
a.i('stp d9, d8, [sp, #-0x70]!')
a.raw(lambda pc, L: enc_b(pc, S_THROW_BALL + 4))


A1 = a
a = Asm(CAVE2)
# ===== cave_rb : Extreme-only "wrapped up at the catch" (fixes the late-throw RB shield) =====
# At s_receive_ball entry (self = catcher): if Extreme, ball.state == 3 (pass in flight), catcher.offense,
# and a defender is within HIT_RADIUS of the catcher -> roll WRAP_PCT. Run the original catch either way;
# afterwards, if the roll hit, put the catcher into the tackled animation (Animation End -> down at the spot).
F = 0x180
a.label('cave_rb')
a.i(f'sub sp, sp, #{F}')
a.i('stp x29, x30, [sp, #0xf0]')
a.i('add x29, sp, #0xf0')
a.i('stp x19, x20, [sp, #0xa0]')
a.i('stp x21, x22, [sp, #0xb0]')
a.i('str x23, [sp, #0xc0]')
a.i('stp x0, x1, [sp, #0xd0]')
a.i('stp x2, x3, [sp, #0xe0]')
a.i('str x4, [sp, #0x98]')
a.i('mov x19, x0')
a.i('mov x20, x1')
a.i('mov w22, #0')
# Extreme?
movabs(a, 'x8', OBJ_MASTER_REF)
a.i('str x8, [sp, #0x10]'); a.i('str wzr, [sp, #0x18]'); a.i('mov w8, #0xf'); a.i('str w8, [sp, #0x1c]')
rv_undef(0x20)
a.i('add x0, sp, #0x10'); slot_w('w1', SLOT_OP_DIFFICULTY)
a.i('mov w2, #-0x80000000'); a.i('add x3, sp, #0x20'); a.i('mov w4, #0'); a.i('mov w5, #0')
bl(GET_INST_VAR)
a.i('add x0, sp, #0x10'); bl(RV_FREE)
a.i('add x0, sp, #0x20'); a.i('mov w1, #4'); bl(RVALUE_EQ_INT)
a.i('mov w21, w0')
a.i('add x0, sp, #0x20'); bl(RV_FREE)
a.raw(lambda pc, L: enc_cbz_w(pc, L['rb_call'], 21))
# global.master_ball.state == 3 ?
a.raw(lambda pc, L: enc_adrp(pc, GLOBAL_INST_PTR, 8))
a.i(f'ldr x0, [x8, #{GLOBAL_INST_PTR & 0xfff}]')
slot_w('w1', SLOT_MASTER_BALL)
a.i('ldr x8, [x0]'); a.i('ldr x8, [x8, #0x10]'); a.i('blr x8')
rv_undef(0x20)
slot_w('w1', SLOT_STATE)
a.i('mov w2, #-0x80000000'); a.i('add x3, sp, #0x20'); a.i('mov w4, #0'); a.i('mov w5, #0')
bl(GET_INST_VAR)
a.i('add x0, sp, #0x20'); a.i('mov w1, #3'); bl(RVALUE_EQ_INT)
a.i('mov w21, w0')
a.i('add x0, sp, #0x20'); bl(RV_FREE)
a.raw(lambda pc, L: enc_cbz_w(pc, L['rb_call'], 21))
# catcher.offense ?
a.i('mov x0, x19'); slot_w('w1', SLOT_OFFENSE)
a.i('ldr x8, [x0]'); a.i('ldr x8, [x8, #0x10]'); a.i('blr x8')
bl(RVALUE_BOOL)
a.raw(lambda pc, L: enc_cbz_w(pc, L['rb_call'], 0))
# catcher x, y
a.i('str x19, [sp, #0x90]')
rv_undef(0x30); rv_undef(0x40)
inst_get(0x30, SLOT_X, 0x90)
inst_get(0x40, SLOT_Y, 0x90)
# with (obj_playerDF): any within HIT_RADIUS ?
a.i('mov w21, #0')
movabs(a, 'x8', OBJ_PLAYERDF_REF)
a.i('str x8, [sp, #0x50]'); a.i('str wzr, [sp, #0x58]'); a.i('mov w8, #0xf'); a.i('str w8, [sp, #0x5c]')
a.i('add x3, sp, #0x50')
with_setup(None)
bl(WITH_BEGIN)
a.i('cmp w0, #1')
a.raw(lambda pc, L: enc_bcond(pc, L['rb_d_end'], 0xb))
a.label('rb_d_loop')
rv_undef(0x100); rv_undef(0x110)
inst_get(0x100, SLOT_X, 0x60)
inst_get(0x110, SLOT_Y, 0x60)
for k, off in enumerate((0x100, 0x110, 0x30, 0x40)):
    a.i(f'add x8, sp, #{off}'); a.i(f'str x8, [sp, #{0x120 + 8 * k}]')
rv_undef(0x160)
a.i('ldr x0, [sp, #0x60]'); a.i('ldr x1, [sp, #0x68]'); a.i('add x2, sp, #0x160'); a.i('mov w3, #4')
slot_w('w4', FN_POINT_DISTANCE)
a.i('add x5, sp, #0x120')
bl(CALL_BUILTIN)
a.i(f'mov w1, #{HIT_RADIUS}')
bl(RV_LT_INT)
a.i('orr w21, w21, w0')
a.i('add x0, sp, #0x100'); bl(RV_FREE)
a.i('add x0, sp, #0x110'); bl(RV_FREE)
a.i('add x0, sp, #0x70'); a.i('add x1, sp, #0x60'); a.i('add x2, sp, #0x68'); bl(WITH_NEXT)
a.raw(lambda pc, L: enc_tbz_w(pc, L['rb_d_loop'], 0, 0, nz=True))
a.label('rb_d_end')
a.i('add x0, sp, #0x70'); a.i('add x1, sp, #0x60'); a.i('add x2, sp, #0x68'); bl(WITH_END)
a.i('add x0, sp, #0x70'); bl(WITH_DTOR)
a.i('add x0, sp, #0x50'); bl(RV_FREE)
a.i('add x0, sp, #0x30'); bl(RV_FREE)
a.i('add x0, sp, #0x40'); bl(RV_FREE)
a.i('and w21, w21, #1')
a.raw(lambda pc, L: enc_cbz_w(pc, L['rb_call'], 21))
# roll
rv_undef(0x20)
a.i('mov x0, x19'); slot_w('w1', SLOT_CURRENT_TIME); a.i('mov w2, #-0x80000000'); a.i('add x3, sp, #0x20')
bl(GET_BUILTIN_VAR)
a.i('ldr d0, [sp, #0x20]'); a.i('fabs d0, d0'); a.i('fcvtzu x8, d0')
a.i('mov x9, #100'); a.i('udiv x10, x8, x9'); a.i('msub x8, x10, x9, x8')
a.i('mov x23, x8')
a.i('add x0, sp, #0x20'); bl(RV_FREE)
a.i(f'cmp x23, #{WRAP_PCT}')
a.i('cset w22, lo')
if DIAG:
    a.raw(lambda pc, L: enc_cbz_w(pc, L['rb_call'], 22))
    script0(S_SOUND_OOF)
# --- run the original catch
a.label('rb_call')
a.i('ldp x0, x1, [sp, #0xd0]'); a.i('ldp x2, x3, [sp, #0xe0]'); a.i('ldr x4, [sp, #0x98]')
a.raw(lambda pc, L: enc_b(pc, L['rb_tramp'], link=True))
a.i('mov x23, x0')
a.raw(lambda pc, L: enc_cbz_w(pc, L['rb_ret'], 22))
# wrapped up: catcher -> tackled animation
a.i('str xzr, [sp, #0x150]'); a.i('str xzr, [sp, #0x158]')
movabs(a, 'x8', ANIM_TACKLED)
a.i('str x8, [sp, #0x150]')
a.i('add x8, sp, #0x150'); a.i('str x8, [sp, #0x120]')
rv_undef(0x160)
a.i('mov x0, x19'); a.i('mov x1, x20'); a.i('add x2, sp, #0x160'); a.i('mov w3, #1'); a.i('add x4, sp, #0x120')
bl(0x1004fb230)
a.i('add x0, sp, #0x160'); bl(RV_FREE)
a.label('rb_ret')
a.i('mov x0, x23')
a.i('ldp x19, x20, [sp, #0xa0]'); a.i('ldp x21, x22, [sp, #0xb0]'); a.i('ldr x23, [sp, #0xc0]')
a.i('ldp x29, x30, [sp, #0xf0]')
a.i(f'add sp, sp, #{F}')
a.i('ret')
# trampoline = original s_receive_ball (displaced first instruction + jump back)
a.label('rb_tramp')
a.i('stp d9, d8, [sp, #-0x70]!')
a.raw(lambda pc, L: enc_b(pc, S_RECEIVE_BALL + 4))

# ===== cave_ai : Extreme-only instant pursuit right after a catch =====
# Runs the original s_aim_AI_defense, then (self = defender) if:
#   master_ball.state == 5 && obj_controller.running_play == 0 && Extreme && holder.offense
#   && dist(holder, catch spot) < REACT_WINDOW && dist(self, holder) < REACT_RADIUS
# -> jdir = direction(self -> holder), jforce = 1
# Frame 0x1c0: 0x10 rv | 0x20 rv | 0x30 hx | 0x40 hy | 0x50 mx | 0x60 my | 0x70 sx | 0x80 sy | 0x90 self* | 0x98 &holderRV
#  0xa0 x19,x20 | 0xb0 x21,x22 | 0xc0 x23,x24 | 0xd0 x0,x1 | 0xe0 x2,x3 | 0xe8.. | 0xf0 x29,x30 | 0x100 argv | 0x120 res | 0x130 x4 | 0x140 d8
F = 0x1c0
def getv(dst, slot, rvptr_reg_setup):
    rvptr_reg_setup()
    slot_w('w1', slot); a.i('mov w2, #-0x80000000'); a.i(f'add x3, sp, #{dst}'); a.i('mov w4, #0'); a.i('mov w5, #0')
    bl(GET_INST_VAR)

def dist_lt(off_a, off_b, off_c, off_d, limit):
    for k, off in enumerate((off_a, off_b, off_c, off_d)):
        a.i(f'add x8, sp, #{off}'); a.i(f'str x8, [sp, #{0x100 + 8 * k}]')
    rv_undef(0x120)
    a.i('mov x0, x19'); a.i('mov x1, x20'); a.i('add x2, sp, #0x120'); a.i('mov w3, #4')
    slot_w('w4', FN_POINT_DISTANCE); a.i('add x5, sp, #0x100')
    bl(CALL_BUILTIN)
    a.i(f'mov w1, #{limit}'); bl(RV_LT_INT)

def write_real_self(slot, dreg):
    a.i('mov x0, x19'); slot_w('w1', slot)
    a.i('ldr x8, [x0]'); a.i('ldr x8, [x8, #0x18]'); a.i('blr x8')
    a.i('mov x21, x0')
    a.i('ldr w8, [x0, #0xc]'); a.i('mov w9, #1'); a.i('lsl w8, w9, w8'); a.i('mov w9, #0x46'); a.i('tst w8, w9')
    lbl = f'wr_{slot:x}'
    a.raw(lambda pc, L, l=lbl: enc_bcond(pc, L[l], 0))
    a.i('mov x0, x21'); bl(RV_FREE_CONTENT)
    a.label(lbl)
    a.i('str wzr, [x21, #0xc]')
    a.i(f'str {dreg}, [x21]')

a.label('cave_ai')
a.i(f'sub sp, sp, #{F}')
a.i('stp x29, x30, [sp, #0xf0]'); a.i('add x29, sp, #0xf0')
a.i('stp x19, x20, [sp, #0xa0]'); a.i('stp x21, x22, [sp, #0xb0]'); a.i('stp x23, x24, [sp, #0xc0]')
a.i('str d8, [sp, #0x140]')
a.i('stp x0, x1, [sp, #0xd0]'); a.i('stp x2, x3, [sp, #0xe0]'); a.i('str x4, [sp, #0x130]')
a.i('mov x19, x0'); a.i('mov x20, x1')
# original AI first
a.raw(lambda pc, L: enc_b(pc, L['ai_tramp'], link=True))
a.i('mov x23, x0')
# (1) master_ball.state == 5
a.raw(lambda pc, L: enc_adrp(pc, GLOBAL_INST_PTR, 8)); a.i(f'ldr x0, [x8, #{GLOBAL_INST_PTR & 0xfff}]')
slot_w('w1', SLOT_MASTER_BALL); a.i('ldr x8, [x0]'); a.i('ldr x8, [x8, #0x10]'); a.i('blr x8')
a.i('mov x24, x0')                                   # x24 = &global.master_ball
rv_undef(0x10)
getv(0x10, SLOT_STATE, lambda: a.i('mov x0, x24'))
a.i('add x0, sp, #0x10'); a.i('mov w1, #5'); bl(RVALUE_EQ_INT); a.i('mov w21, w0')
a.i('add x0, sp, #0x10'); bl(RV_FREE)
a.raw(lambda pc, L: enc_cbz_w(pc, L['ai_ret'], 21))
# (2) obj_controller.running_play == 0
movabs(a, 'x8', OBJ44_REF)
a.i('str x8, [sp, #0x20]'); a.i('str wzr, [sp, #0x28]'); a.i('mov w8, #0xf'); a.i('str w8, [sp, #0x2c]')
rv_undef(0x10)
getv(0x10, SLOT_RUNNING_PLAY, lambda: a.i('add x0, sp, #0x20'))
a.i('add x0, sp, #0x20'); bl(RV_FREE)
a.i('add x0, sp, #0x10'); bl(RVALUE_BOOL); a.i('mov w21, w0')
a.i('add x0, sp, #0x10'); bl(RV_FREE)
a.raw(lambda pc, L: enc_tbz_w(pc, L['ai_ret'], 21, 0, nz=True))   # running play -> leave AI alone
# (3) Extreme
movabs(a, 'x8', OBJ_MASTER_REF)
a.i('str x8, [sp, #0x20]'); a.i('str wzr, [sp, #0x28]'); a.i('mov w8, #0xf'); a.i('str w8, [sp, #0x2c]')
rv_undef(0x10)
getv(0x10, SLOT_OP_DIFFICULTY, lambda: a.i('add x0, sp, #0x20'))
a.i('add x0, sp, #0x20'); bl(RV_FREE)
a.i('add x0, sp, #0x10'); a.i('mov w1, #4'); bl(RVALUE_EQ_INT); a.i('mov w21, w0')
a.i('add x0, sp, #0x10'); bl(RV_FREE)
a.raw(lambda pc, L: enc_cbz_w(pc, L['ai_ret'], 21))
# (4) holder: &ball.holder via master_ball; holder.offense
rv_undef(0x20)
getv(0x20, SLOT_HOLDER, lambda: a.i('mov x0, x24'))          # [0x20] = holder value
rv_undef(0x10)
getv(0x10, SLOT_OFFENSE, lambda: a.i('add x0, sp, #0x20'))
a.i('add x0, sp, #0x10'); bl(RVALUE_BOOL); a.i('mov w21, w0')
a.i('add x0, sp, #0x10'); bl(RV_FREE)
a.raw(lambda pc, L: enc_cbz_w(pc, L['ai_free_h'], 21))
# holder x,y ; catch spot (ball.xreceived / yreceived)
for off in (0x30, 0x40, 0x50, 0x60, 0x70, 0x80): rv_undef(off)
getv(0x30, SLOT_X, lambda: a.i('add x0, sp, #0x20'))
getv(0x40, SLOT_Y, lambda: a.i('add x0, sp, #0x20'))
getv(0x50, SLOT_XRECEIVED, lambda: a.i('mov x0, x24'))
getv(0x60, SLOT_YRECEIVED, lambda: a.i('mov x0, x24'))
dist_lt(0x30, 0x40, 0x50, 0x60, REACT_WINDOW); a.i('mov w21, w0')
a.raw(lambda pc, L: enc_cbz_w(pc, L['ai_free_all'], 21))
# self x,y
a.i('str x19, [sp, #0x90]')
inst_get(0x70, SLOT_X, 0x90)
inst_get(0x80, SLOT_Y, 0x90)
dist_lt(0x70, 0x80, 0x30, 0x40, REACT_RADIUS); a.i('mov w21, w0')
a.raw(lambda pc, L: enc_cbz_w(pc, L['ai_free_all'], 21))
# direction self -> holder (GML degrees): atan2(sy - hy, hx - sx)
a.i('ldr d0, [sp, #0x80]'); a.i('ldr d1, [sp, #0x40]'); a.i('fsub d0, d0, d1')      # dy = sy - hy
a.i('ldr d1, [sp, #0x30]'); a.i('ldr d2, [sp, #0x70]'); a.i('fsub d1, d1, d2')      # dx = hx - sx
a.raw(lambda pc, L: enc_adrp(pc, GOT_ATAN2, 16)); a.i(f'ldr x16, [x16, #{GOT_ATAN2 & 0xfff}]'); a.i('blr x16')
movabs(a, 'x8', 0x404CA5DC1A63C1F8)                  # 57.29577951308232
a.i('fmov d1, x8'); a.i('fmul d8, d0, d1')
a.i('fcmp d8, #0.0')
a.raw(lambda pc, L: enc_bcond(pc, L['ai_pos'], 0xa))   # b.ge
movabs(a, 'x8', 0x4076800000000000)                  # 360.0
a.i('fmov d1, x8'); a.i('fadd d8, d8, d1')
a.label('ai_pos')
a.i('fmov x22, d8')
write_real_self(SLOT_JDIR, 'x22')
movabs(a, 'x22', 0x3FF0000000000000)                 # 1.0
write_real_self(SLOT_JFORCE, 'x22')
a.label('ai_free_all')
for off in (0x30, 0x40, 0x50, 0x60, 0x70, 0x80):
    a.i(f'add x0, sp, #{off}'); bl(RV_FREE)
a.label('ai_free_h')
a.i('add x0, sp, #0x20'); bl(RV_FREE)
a.label('ai_ret')
a.i('mov x0, x23')
a.i('ldr d8, [sp, #0x140]')
a.i('ldp x19, x20, [sp, #0xa0]'); a.i('ldp x21, x22, [sp, #0xb0]'); a.i('ldp x23, x24, [sp, #0xc0]')
a.i('ldp x29, x30, [sp, #0xf0]')
a.i(f'add sp, sp, #{F}')
a.i('ret')
a.label('ai_tramp')
a.i('stp d11, d10, [sp, #-0x80]!')
a.raw(lambda pc, L: enc_b(pc, S_AIM_AI_DEFENSE + 4))

# ===== cave_cp (experiment) : after s_change_possession, obj_controller.human_team = obj_controller.possession =====
if FOLLOW:
    a.label('cave_cp')
    a.i('sub sp, sp, #0x60')
    a.i('stp x29, x30, [sp, #0x50]'); a.i('add x29, sp, #0x50')
    a.i('stp x19, x20, [sp, #0x40]')
    a.raw(lambda pc, L: enc_b(pc, L['cp_tramp'], link=True))
    a.i('mov x19, x0')
    movabs(a, 'x8', OBJ44_REF)
    a.i('str x8, [sp, #0x10]'); a.i('str wzr, [sp, #0x18]'); a.i('mov w8, #0xf'); a.i('str w8, [sp, #0x1c]')
    rv_undef(0x20)
    a.i('add x0, sp, #0x10'); slot_w('w1', SLOT_POSSESSION)
    a.i('mov w2, #-0x80000000'); a.i('add x3, sp, #0x20'); a.i('mov w4, #0'); a.i('mov w5, #0')
    bl(GET_INST_VAR)
    a.i('add x0, sp, #0x10'); slot_w('w1', SLOT_HUMAN_TEAM)
    a.i('mov w2, #-0x80000000'); a.i('add x3, sp, #0x20')
    bl(SET_REF_VAR)
    a.i('add x0, sp, #0x20'); bl(RV_FREE)
    a.i('add x0, sp, #0x10'); bl(RV_FREE)
    a.i('mov x0, x19')
    a.i('ldp x19, x20, [sp, #0x40]')
    a.i('ldp x29, x30, [sp, #0x50]')
    a.i('add sp, sp, #0x60')
    a.i('ret')
    a.label('cp_tramp')
    a.i('sub sp, sp, #0x150')
    a.raw(lambda pc, L: enc_b(pc, S_CHANGE_POSSESSION + 4))

words = A1.build()
words2 = a.build()
assert 4 * len(words) < 2744, len(words)
assert 4 * len(words2) < 10000, len(words2)

# ---- sanity checks on original bytes ----
expect(CT_POS_CHECK_TBZ, enc_tbz_w(CT_POS_CHECK_TBZ, CT_FAIL, 0, 0))
expect(S5_HANDLER, mc('mov w8, #0x12c'))
expect(S_THROW_BALL, mc('stp d9, d8, [sp, #-0x70]!'))
expect(S_RECEIVE_BALL, mc('stp d9, d8, [sp, #-0x70]!'))
expect(S_AIM_AI_DEFENSE, mc('stp d11, d10, [sp, #-0x80]!'))
expect(S_CHANGE_POSSESSION, mc('sub sp, sp, #0x150'))

# ---- write caves + hooks ----
for k, w in enumerate(words):
    put(CAVE + 4 * k, w)
for k, w in enumerate(words2):
    put(CAVE2 + 4 * k, w)
put(CT_POS_CHECK_TBZ, enc_b(CT_POS_CHECK_TBZ, A1.labels['cave_ct']))
put(S5_HANDLER, enc_b(S5_HANDLER, A1.labels['cave_s5']))
put(S_THROW_BALL, enc_b(S_THROW_BALL, A1.labels['cave_tb']))
put(S_RECEIVE_BALL, enc_b(S_RECEIVE_BALL, a.labels['cave_rb']))
put(S_AIM_AI_DEFENSE, enc_b(S_AIM_AI_DEFENSE, a.labels['cave_ai']))
if FOLLOW: put(S_CHANGE_POSSESSION, enc_b(S_CHANGE_POSSESSION, a.labels['cave_cp']))

open(DST, 'wb').write(data)
print(f'cave1: {len(words)} insns @ {CAVE:#x} (ct={A1.labels["cave_ct"]:#x} s5={A1.labels["cave_s5"]:#x} tb={A1.labels["cave_tb"]:#x}); cave2: {len(words2)} insns @ {CAVE2:#x} (rb={a.labels["cave_rb"]:#x})')
