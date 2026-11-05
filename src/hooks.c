// hooks.c - the mods themselves.
#include "rb.h"
#include "ui.h"

enum {
#define HOOK(name, va, insn) HK_##name,
#include "hooks.def"
#undef HOOK
    HK_COUNT
};
#define ORIG(name) ((GMLScript)RB_TRAMP(HK_##name))

#define VA_S_SIMMING_ACTION 0x100565cf0ULL

static int simming_action(CInstance *self, CInstance *other) {
    RValue r = {0};
    r.kind = RV_UNDEFINED;
    ((GMLScript)RB(VA_S_SIMMING_ACTION))(self, other, &r, 0, 0);
    return rb_rv_real(&r) > 0.5;
}

// =============================================================== play or simulate the CPU's drive
// When the CPU gets the ball we ask: Play Defense / Simulate. While the question is open the
// commentary flow (s_update_commentary) is held at its next step, so nothing advances without an
// answer. At the ATTACK stage the answer picks on-field play or the normal simulation.
enum { CHOICE_NONE = 0, CHOICE_PLAY = 1, CHOICE_SIM = 2 };
static volatile int g_pending;          // popup open, commentary held
static volatile int g_choice;           // answer for the current CPU possession
static int g_hub_depth;

static void on_drive_choice(int c) {     // main thread
    g_choice = (c == CHOICE_PLAY) ? CHOICE_PLAY : CHOICE_SIM;   // failure to show -> simulate
    g_pending = 0;
    rb_log("drive choice: %s", g_choice == CHOICE_PLAY ? "PLAY" : "SIM");
}

RValue *hook_change_possession(CInstance *self, CInstance *other, RValue *res, int argc, RValue **argv) {
    RValue *r = ORIG(change_possession)(self, other, res, argc, argv);
    double poss = rb_get_obj_real(OBJ_CONTROLLER, SLOT_POSSESSION);
    double human = rb_get_obj_real(OBJ_MASTER, SLOT_PLAYERTEAMINDEX);
    rb_log("possession -> %g (human team %g)", poss, human);
    g_choice = CHOICE_NONE;
    g_pending = 0;
    if (human >= 0 && poss != human && !simming_action(self, other)) {
        g_pending = 1;
        rb_ui_choice("Opponent's Ball", "Play this defensive drive or simulate it?",
                     "Play Defense", "Simulate", on_drive_choice);
    }
    return r;
}

RValue *hook_update_commentary(CInstance *self, CInstance *other, RValue *res, int argc, RValue **argv) {
    if (g_pending && g_hub_depth == 0) {          // hold: comm_proceed stays set, retried next frame
        res->i64 = 0;
        res->kind = RV_UNDEFINED;
        return res;
    }
    g_hub_depth++;
    RValue *r = ORIG(update_commentary)(self, other, res, argc, argv);
    g_hub_depth--;
    return r;
}

// Called from the custom stub at 0x1005ad2d0 (ATTACK stage).
// human_has_ball: the game's own test (possession == obj_master.playerteamindex).
enum { GATE_FIELD = 0, GATE_CONTINUE = 1, GATE_SIM = 2 };

int rb_gate_attack(CInstance *self, int human_has_ball) {
    int g = (human_has_ball || g_choice == CHOICE_PLAY) ? GATE_FIELD : GATE_CONTINUE;
    rb_log("ATTACK stage: human_has_ball %d, choice %d -> %s", human_has_ball, g_choice,
           g == GATE_FIELD ? "field" : "game default");
    return g;
}

// =============================================================== install
void rb_install_hooks(void **tab) {
#define HOOK(name, va, insn) tab[TAB_HOOK_BASE + HK_##name] = (void *)hook_##name;
#include "hooks.def"
#undef HOOK
    tab[TAB_GATE_ATTACK] = (void *)rb_gate_attack;
}
