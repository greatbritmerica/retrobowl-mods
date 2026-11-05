// hooks.c - the mods themselves.
#include "rb.h"

enum {
#define HOOK(name, va, insn) HK_##name,
#include "hooks.def"
#undef HOOK
    HK_COUNT
};
#define ORIG(name) ((GMLScript)RB_TRAMP(HK_##name))

// ---------------------------------------------------------------- possession changes (logging only for now)
RValue *hook_change_possession(CInstance *self, CInstance *other, RValue *res, int argc, RValue **argv) {
    RValue *r = ORIG(change_possession)(self, other, res, argc, argv);
    rb_log("possession -> %g (human team %g)",
           rb_get_obj_real(OBJ_CONTROLLER, SLOT_POSSESSION),
           rb_get_obj_real(OBJ_MASTER, SLOT_PLAYERTEAMINDEX));
    return r;
}

// ---------------------------------------------------------------- ATTACK stage: play the drive or simulate it
// Called from the custom stub at 0x1005ad2d0 in s_update_commentary.
// human_has_ball: the game's own test (possession == obj_master.playerteamindex).
// Return: GATE_FIELD = play on the field, GATE_CONTINUE = game's normal checks (CPU drive -> sim),
//         GATE_SIM = simulate.
enum { GATE_FIELD = 0, GATE_CONTINUE = 1, GATE_SIM = 2 };

int rb_gate_attack(CInstance *self, int human_has_ball) {
    rb_log("ATTACK stage: possession %g, human team %g, human_has_ball %d",
           rb_get_inst_real(self, SLOT_POSSESSION),
           rb_get_obj_real(OBJ_MASTER, SLOT_PLAYERTEAMINDEX), human_has_ball);
    return human_has_ball ? GATE_FIELD : GATE_CONTINUE;   // vanilla behaviour for now
}

// ---------------------------------------------------------------- install
void rb_install_hooks(void **tab) {
#define HOOK(name, va, insn) tab[TAB_HOOK_BASE + HK_##name] = (void *)hook_##name;
#include "hooks.def"
#undef HOOK
    tab[TAB_GATE_ATTACK] = (void *)rb_gate_attack;
}
