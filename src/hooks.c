// hooks.c - the mods themselves.
#include "rb.h"
#include "ui.h"
#include "sys.h"
#include "input.h"
#include "cpuqb.h"
#include "defense.h"
#include "dbrain.h"
#include "camera3d.h"

enum {
#define HOOK(name, va, insn) HK_##name,
#define CALLSITE(name, site, target) HK_##name,
#include "hooks.def"
#undef HOOK
#undef CALLSITE
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

static int g_cpu_clearing;      // our own response call is closing the message

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
    rb_dlearn_step(self, other, human >= 0 && poss >= 0 && poss == human);
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
    if (rb_cpuqb_action) {                     // CPU 4th-down call, made the same way the buttons do
        int act = rb_cpuqb_action;
        rb_cpuqb_action = ACT_NONE;
        RValue pr = {0}; pr.kind = RV_UNDEFINED;
        rb_vf.active = 0; rb_vf.down = 0;      // whatever happens next, the screen is the human's again
        g_cpu_clearing = 1;
        if (act == ACT_PUNT) { rb_log("cpuqb: punting"); ((GMLScript)RB(0x100094060ULL))(self, other, &pr, 0, 0); }
        else if (act == ACT_FIELD_GOAL) { rb_log("cpuqb: kicking field goal"); ((GMLScript)RB(0x100087064ULL))(self, other, &pr, 0, 0); }
        else if (act == ACT_PLAY_4TH) { rb_log("cpuqb: going for it"); ((GMLScript)RB(0x10008b700ULL))(self, other, &pr, 0, 0); }
        g_cpu_clearing = 0;
    }
    return r;
}

// 4th-down question on a CPU drive: let the CPU answer it
RValue *hook_create_message(CInstance *self, CInstance *other, RValue *res, int argc, RValue **argv) {
    RValue *r = ORIG(create_message)(self, other, res, argc, argv);
    char b[64];
    const char *txt = argc > 0 ? rb_rv_str(argv[0], b, sizeof b) : "";
    if (!(strstr(txt, "4th") || strstr(txt, "ourth"))) return r;          // only the 4th-down question
    if (!rb_obj_exists(OBJ_CONTROLLER)) return r;                          // not in a match
    double human = rb_get_obj_real(OBJ_MASTER, SLOT_PLAYERTEAMINDEX);
    double poss = rb_get_obj_real(OBJ_CONTROLLER, SLOT_POSSESSION);
    if (human >= 0 && poss >= 0 && poss != human && rb_get_obj_real(OBJ_CONTROLLER, SLOT_DOWN) >= 4) {
        rb_cpuqb_fourth_msg = (strstr(txt, "oal") || strstr(txt, "OAL")) ? 1 : 2;
        rb_log("message on CPU 4th down: \"%s\"", txt);
    }
    return r;
}

RValue *hook_msg_clear(CInstance *self, CInstance *other, RValue *res, int argc, RValue **argv) {
    if (!g_cpu_clearing) rb_cpuqb_fourth_closed();
    return ORIG(msg_clear)(self, other, res, argc, argv);
}

// =============================================================== experimental 3D camera
void hook_camera_step2(CInstance *self, CInstance *other) {     // obj_camera End Step: the real camera logic
    rb_cam3d_before(self);
    ORIG_EV(camera_step2)(self, other);
    rb_cam3d_after(self);
}

// the stadium image (field + stands, one sprite) drawn at 0,0: on the ground minus the stands in 3D
typedef void (*DrawSprFn)(CInstance *self, long sprite, int colour, float sub, float x, float y,
                          float xs, float ys, float rot, float alpha);
void call_stadium_draw(CInstance *self, long sprite, int colour, float sub, float x, float y,
                       float xs, float ys, float rot, float alpha) {
    if (rb_cam3d_stadium(self, sprite, colour, sub, x, y, xs, ys, alpha)) return;
    ((DrawSprFn)RB(0x1008d91ccULL))(self, sprite, colour, sub, x, y, xs, ys, rot, alpha);
}

// =============================================================== player control on defense
RValue *hook_get_destination(CInstance *self, CInstance *other, RValue *res, int argc, RValue **argv) {
    RValue *r = ORIG(get_destination)(self, other, res, argc, argv);
    rb_def_after_ai(self);
    rb_dlearn_after_ai(self);
    return r;
}

void hook_player_draw(CInstance *self, CInstance *other) {
    if (rb_cam3d_active() && !rb_cam3d_in_gui_pass) return;      // 3D camera: drawn standing, later
    ORIG_EV(player_draw)(self, other);
    rb_def_draw(self);
}

void hook_ball_draw(CInstance *self, CInstance *other) {
    if (rb_cam3d_active() && !rb_cam3d_in_gui_pass) return;
    ORIG_EV(ball_draw)(self, other);
}

void hook_fieldtxt_draw(CInstance *self, CInstance *other) {
    if (rb_cam3d_active() && !rb_cam3d_in_gui_pass) return;
    ORIG_EV(fieldtxt_draw)(self, other);
}

static void gui_draw_text(CInstance *t) { hook_fieldtxt_draw(t, t); }
static void gui_draw_player(CInstance *p) { hook_player_draw(p, p); }
static void gui_draw_ball(CInstance *b) { hook_ball_draw(b, b); }

void hook_controller_gui(CInstance *self, CInstance *other) {
    rb_cam3d_gui_pass(self, gui_draw_player, gui_draw_ball, gui_draw_text);      // standing players + sky, under the HUD
    ORIG_EV(controller_gui)(self, other);
}

// =============================================================== install
void rb_install_hooks(void **tab) {
#define HOOK(name, va, insn) tab[TAB_HOOK_BASE + HK_##name] = (void *)hook_##name;
#define CALLSITE(name, site, target) tab[TAB_HOOK_BASE + HK_##name] = (void *)call_##name;
#include "hooks.def"
#undef HOOK
#undef CALLSITE
    tab[TAB_GATE_ATTACK] = (void *)rb_gate_attack;
}
