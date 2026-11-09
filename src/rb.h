// rb.h - Retro Bowl 1.6.49 (iOS, GameMaker YYC) runtime bindings for the mod library.
// All addresses are unslid VAs from the 1.6.49 binary (image base 0x100000000).
#pragma once
#include <stdint.h>

// ---------------------------------------------------------------- layout shared with tools/inject.py
#define RB_IMAGE_BASE   0x100000000ULL
#define RB_TAB_VA       0x101353400ULL   // zero-filled tail of __DATA: pointer table the stubs jump through
#define RB_STUB_VA      0x1000a1a04ULL   // gml_Script_s_debug_init_team_ranking (unused) holds the stubs
#define RB_STUB_SIZE    24               // per entry-hook stub: adrp/ldr/cbz/br + displaced insn + b back
#define RB_TAB_MAGIC    0x5242544142ULL  // "BATBR" - written last by the constructor

enum {
    TAB_MAGIC       = 0,
    TAB_GATE_ATTACK = 1,   // custom stub: commentary ATTACK stage, play-on-field vs simulate decision
    TAB_NO_WINDUP   = 2,   // int*: nonzero = skip the Extreme windup-sack check (scramble throws; binary cave_tb)
    TAB_HOOK_BASE   = 8,   // entry hooks from hooks.def start here
};

// ---------------------------------------------------------------- GameMaker values
typedef struct RValue {
    union { double real; int64_t i64; void *ptr; };
    uint32_t flags;
    uint32_t kind;
} RValue;

enum { RV_REAL = 0, RV_STRING = 1, RV_UNDEFINED = 5, RV_PTR = 6, RV_BOOL = 13, RV_REF = 15 };

typedef struct CInstance CInstance;
typedef RValue *(*GMLScript)(CInstance *self, CInstance *other, RValue *result, int argc, RValue **argv);
typedef void (*GMLEvent)(CInstance *self, CInstance *other);

// object indices (game.ios OBJT)
enum { OBJ_BTN_FIELDGOAL = 16, OBJ_BALL = 3, OBJ_CONTROLLER = 44, OBJ_MASTER = 88, OBJ_PLAYER = 117, OBJ_PLAYER_DF = 118, OBJ_PLAYER_OF = 119 };

// variable slot records ({int32 runtime id} at these VAs)
#define SLOT_POSSESSION      0x100fa7668ULL
#define SLOT_PLAYERTEAMINDEX 0x100fa7468ULL
#define SLOT_COMM_STAGE      0x100fa4348ULL
#define SLOT_STATE           0x100faf518ULL
#define SLOT_HOLDER          0x100fa5d78ULL
#define SLOT_MASTER_BALL     0x100fa6a38ULL
#define SLOT_OP_DIFFICULTY   0x100fa71d8ULL
#define SLOT_RUNNING_PLAY    0x100fa8138ULL
#define SLOT_OFFENSE         0x100fa70f8ULL
#define SLOT_MY_ACCEL        0x100fa6ca8ULL   // per-player acceleration (scales movement and jdir/jforce)
#define SLOT_X               0x100fb07e8ULL
#define SLOT_Y               0x100fb0808ULL
#define SLOT_DIRECTION       0x100fb0a98ULL
#define SLOT_VELOCITY        0x100fb02d8ULL
#define SLOT_ZVEL            0x100fb07b8ULL
#define SLOT_DRAGDIST        0x100fa4c28ULL
#define SLOT_DRAGX           0x100fa4c38ULL
#define SLOT_DRAGY           0x100fa4c48ULL
#define SLOT_XTOUCH          0x100fb0638ULL
#define SLOT_YTOUCH          0x100fb0788ULL
#define SLOT_MY_THROW_POWER  0x100fa6d98ULL
#define SLOT_DOWN            0x100fa4b48ULL
#define SLOT_SCRIMMAGE       0x100faece8ULL
#define SLOT_KICKING         0x100fa6308ULL
#define SLOT_YARDS           0x100fb06e8ULL
#define SLOT_X_YARDS         0x100fb0578ULL
#define SLOT_TXT             0x100fafe18ULL
#define SLOT_SPRITE_WIDTH    0x100fb07f8ULL
#define SLOT_SPRITE_HEIGHT   0x100fb0818ULL
#define SLOT_KICKPOW         0x100fa6328ULL
#define SLOT_DIREC           0x100fa4a08ULL
#define SLOT_KICKFLIP        0x100fa62f8ULL
#define SLOT_CPU_KICKPOW     0x100fa4558ULL
#define SLOT_JDIR            0x100fa6238ULL
#define SLOT_JFORCE          0x100fa6258ULL

// ---------------------------------------------------------------- runtime functions
#define VA_GET_INST_VAR   0x100bba9f0ULL  // (RValue* inst, slot, INT_MIN, RValue* out, 0, 0)
#define VA_SET_REF_VAR    0x100bbab88ULL  // (RValue* target, slot, INT_MIN, RValue* value)
#define VA_GET_BUILTIN    0x100bb9fc0ULL  // (CInstance* self, slot, INT_MIN, RValue* out)
#define VA_SET_BUILTIN    0x100bba1bcULL  // (CInstance* self, slot, INT_MIN, RValue* value)
#define VA_CALL_BUILTIN   0x100bce4c8ULL  // (self, other, RValue* res, argc, fn_id, RValue** argv)
#define VA_RV_FREE        0x100004ea0ULL
#define VA_RV_COPY        0x10000684cULL  // (dst, src)
#define VA_RV_TO_DOUBLE   0x1008792c8ULL
#define VA_WITH_BEGIN     0x100bcffa0ULL  // (iter*, self**, other**, RValue* target) -> count
#define VA_WITH_NEXT      0x100bd0374ULL  // (iter*, self**, other**) -> more
#define VA_WITH_END       0x100bd03a4ULL  // (iter*, self**, other**)
#define VA_WITH_DTOR      0x10000dea4ULL  // (iter*)

extern uintptr_t rb_base;   // runtime address of the game's Mach-O header
#define RB(va) ((void *)(rb_base + ((uint64_t)(va) - RB_IMAGE_BASE)))

static inline int rb_slot(uint64_t slot_va) { return *(int32_t *)RB(slot_va); }

// original (un-hooked) entry of hook number k from hooks.def
#define RB_TRAMP(k) RB(RB_STUB_VA + (uint64_t)(k) * RB_STUB_SIZE + 16)

// ---------------------------------------------------------------- helpers (rb_runtime.c)
void   rb_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
RValue rb_obj_ref(int object_index);
int    rb_obj_exists(int object_index);                       // RValue that refers to an object (kind 15)
double rb_get_obj_real(int object_index, uint64_t slot_va); // object_index.var  (first instance)
double rb_get_inst_real(CInstance *inst, uint64_t slot_va); // inst.var
double rb_get_builtin_real(CInstance *inst, uint64_t slot_va);
void   rb_set_builtin_real(CInstance *inst, uint64_t slot_va, double v); // inst.x, inst.direction ...
void   rb_set_obj_real(int object_index, uint64_t slot_va, double v);
void   rb_set_inst_real(CInstance *inst, uint64_t slot_va, double v);
double rb_rv_real(RValue *v);
// with (target) { fn(instance) }  - target: an object ref (rb_obj_ref) or an instance value.
// fn returns nonzero to stop early. Returns the number of instances visited.
int rb_with(RValue *target, CInstance *self, CInstance *other, int (*fn)(CInstance *inst, void *ctx), void *ctx);
RValue *rb_inst_var(CInstance *inst, uint64_t slot_va);    // pointer to inst.var (read), or 0
const char *rb_rv_str(RValue *v, char *buf, int n);   // printable form of any value
#define RB_CALLER() ((unsigned long long)((uintptr_t)__builtin_return_address(0) - rb_base + RB_IMAGE_BASE))
