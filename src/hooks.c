// hooks.c - the mods themselves.
#include "rb.h"
#include "ui.h"
#include "sys.h"
#include "input.h"
#include "cpuqb.h"
#include "defense.h"

enum {
#define HOOK(name, va, insn) HK_##name,
#include "hooks.def"
#undef HOOK
    HK_COUNT
};
#define ORIG(name) ((GMLScript)RB_TRAMP(HK_##name))
#define ORIG_EV(name) ((GMLEvent)RB_TRAMP(HK_##name))

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
    unsigned long long caller = RB_CALLER();
    double before = rb_get_obj_real(OBJ_CONTROLLER, SLOT_POSSESSION);
    RValue *r = ORIG(change_possession)(self, other, res, argc, argv);
    double poss = rb_get_obj_real(OBJ_CONTROLLER, SLOT_POSSESSION);
    double human = rb_get_obj_real(OBJ_MASTER, SLOT_PLAYERTEAMINDEX);
    rb_log("possession %g -> %g (human team %g) caller %llx stage %g", before, poss, human, caller,
           rb_get_obj_real(OBJ_CONTROLLER, SLOT_COMM_STAGE));
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

// =============================================================== diagnostics
// game's own debug log (slog) -> rbmod.log
RValue *hook_slog(CInstance *self, CInstance *other, RValue *res, int argc, RValue **argv) {
    rb_input_install();
    char line[400]; int n = 0;
    for (int i = 0; i < argc && i < 6 && n < (int)sizeof line - 80; i++) {
        char b[48];
        n += snprintf(line + n, sizeof line - n, "%s%s", i ? " " : "", rb_rv_str(argv[i], b, sizeof b));
    }
    line[n < (int)sizeof line ? n : (int)sizeof line - 1] = 0;
    rb_log("slog: %s", line);
    return ORIG(slog)(self, other, res, argc, argv);
}

RValue *hook_action_result(CInstance *self, CInstance *other, RValue *res, int argc, RValue **argv) {
    char b[48];
    rb_log("action_result(%s) possession %g caller %llx", argc > 0 ? rb_rv_str(argv[0], b, sizeof b) : "-",
           rb_get_obj_real(OBJ_CONTROLLER, SLOT_POSSESSION), RB_CALLER());
    RValue *r = ORIG(action_result)(self, other, res, argc, argv);
    rb_log("action_result done: possession %g", rb_get_obj_real(OBJ_CONTROLLER, SLOT_POSSESSION));
    return r;
}

// per-step ball update: CPU quarterback on CPU drives, ball-state log
static double g_last_state = -1;
RValue *hook_update_ball(CInstance *self, CInstance *other, RValue *res, int argc, RValue **argv) {
    rb_frame++;
    double human = rb_get_obj_real(OBJ_MASTER, SLOT_PLAYERTEAMINDEX);
    double poss = rb_get_obj_real(OBJ_CONTROLLER, SLOT_POSSESSION);
    int cpu_offense = human >= 0 && poss >= 0 && poss != human;
    rb_cpuqb_step(self, other, cpu_offense);
    double los, fwd, qbx, qby;
    rb_cpuqb_info(&los, &fwd, &qbx, &qby);
    rb_def_step(self, other, cpu_offense, los, fwd, qbx, qby);
    double st = rb_get_inst_real(self, SLOT_STATE);
    if (st != g_last_state) {
        rb_log("ball f%u state %g -> %g  pos %.0f,%.0f vel %.2f dist %.1f %s",
               rb_frame, g_last_state, st, rb_get_builtin_real(self, SLOT_X), rb_get_builtin_real(self, SLOT_Y),
               rb_get_inst_real(self, SLOT_VELOCITY), rb_get_inst_real(self, SLOT_DRAGDIST),
               cpu_offense ? "[CPU ball]" : "");
        g_last_state = st;
    }
    // kick recorder: log kick meter changes (human or CPU) and the inputs around them
    static double last_kp = -1, last_dir = -1; static unsigned last_kick_log;
    double kp = rb_get_inst_real(self, SLOT_KICKPOW), dr = rb_get_inst_real(self, SLOT_DIREC);
    if ((kp != last_kp || dr != last_dir) && kp > -9000 && rb_frame - last_kick_log >= 3) {
        rb_log("kick f%u state %g kickpow %.3f direc %.2f flip %g", rb_frame, st, kp, dr, rb_get_inst_real(self, SLOT_KICKFLIP));
        last_kp = kp; last_dir = dr; last_kick_log = rb_frame;
        rb_input_log = 1;
    } else if (rb_frame - last_kick_log > 120) rb_input_log = 0;

    RValue *r = ORIG(update_ball)(self, other, res, argc, argv);
    if (rb_cpuqb_punt_pending) {                                 // CPU chose to punt: same as tapping "yes"
        rb_cpuqb_punt_pending = 0;
        RValue pr = {0}; pr.kind = RV_UNDEFINED;
        rb_log("cpuqb: punting");
        ((GMLScript)RB(0x100094060ULL))(self, other, &pr, 0, 0);   // gml_Script_btn_response_punt_yes
    }
    return r;
}

// =============================================================== player control on defense
RValue *hook_get_destination(CInstance *self, CInstance *other, RValue *res, int argc, RValue **argv) {
    RValue *r = ORIG(get_destination)(self, other, res, argc, argv);
    rb_def_after_ai(self);
    return r;
}

void hook_player_draw(CInstance *self, CInstance *other) {
    ORIG_EV(player_draw)(self, other);
    rb_def_draw(self);
}

// =============================================================== install
void rb_install_hooks(void **tab) {
#define HOOK(name, va, insn) tab[TAB_HOOK_BASE + HK_##name] = (void *)hook_##name;
#include "hooks.def"
#undef HOOK
    tab[TAB_GATE_ATTACK] = (void *)rb_gate_attack;
}
