// hooks.c - the mods themselves.
#include "rb.h"
#include "ui.h"
#include "sys.h"
#include "input.h"
#include "cpuqb.h"
#include "defcall.h"
#include "offtools.h"
#include "defense.h"
#include "dbrain.h"
#include "tune.h"
#include "rules.h"
#include "script.h"
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
static void fourth_check(const char *txt, const char *via);
static int g_match_live;        // a ball has stepped since the match objects were last (re)built
static CInstance *g_play_ball;  // the ball of the play in progress (every play gets a new one)
static int g_score[2] = { -1, -1 };
// the score from your side of it (which team is listed first changes with who you play as)
void rb_score(int *us, int *them) {
    int h = (int)rb_get_obj_real(OBJ_MASTER, SLOT_PLAYERTEAMINDEX);
    if (h != 0 && h != 1) { *us = *them = -1; return; }
    *us = g_score[h]; *them = g_score[1 - h];
}
// Interception return: the man with the ball, and your eleven who have to get him down.
static struct { CInstance *who, *ball; CInstance *of[12]; int nof; double px, py, vx, vy; } g_ret;
RValue *hook_slog(CInstance *self, CInstance *other, RValue *res, int argc, RValue **argv) {
    rb_input_install();
    char line[400]; int n = 0;
    for (int i = 0; i < argc && i < 6 && n < (int)sizeof line - 80; i++) {
        char b[48];
        n += snprintf(line + n, sizeof line - n, "%s%s", i ? " " : "", rb_rv_str(argv[i], b, sizeof b));
    }
    line[n < (int)sizeof line ? n : (int)sizeof line - 1] = 0;
    rb_log("slog: %s", line);
    if (!strncmp(line, "s_create_message[", 17)) fourth_check(line, "slog");
    const char *sc = strstr(line, "score[");                         // the game's own score line: [team 0 | team 1]
    if (sc) { int a, b; if (sscanf(sc, "score[%d | %d]", &a, &b) == 2) { g_score[0] = a; g_score[1] = b; } }
    if (!strncmp(line, "s_destroy_match_objects", 23)) {             // ball and players are about to be freed
        g_match_live = 0;
        g_play_ball = 0;
        g_ret.who = 0; g_ret.nof = 0;
        rb_off_apply_penalty();
        rb_off_forget(); rb_dbrain_forget();
    }
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

static int g_cpu_ball;          // the CPU has the ball on the field (playable defense)
int rb_no_windup_sack;          // read by the binary's windup-sack check: a QB who scrambled and pulled up isn't sacked mid-throw
static int g_cpu_clearing;      // our own response call is closing the message

// per-step ball update: CPU quarterback on CPU drives, ball-state log
static double g_last_state = -1;
// carry out the CPU's 4th-down call, the same way the message buttons do
static void cpu_fourth_action(CInstance *self, CInstance *other) {
    if (!rb_cpuqb_action) return;
    int act = rb_cpuqb_action;
    rb_cpuqb_action = ACT_NONE;
    RValue pr = {0}; pr.kind = RV_UNDEFINED;
    rb_vf.active = 0; rb_vf.down = 0;          // whatever happens next, the screen is the human's again
    g_cpu_clearing = 1;
    if (act == ACT_PUNT) { rb_log("cpuqb: punting"); ((GMLScript)RB(0x100094060ULL))(self, other, &pr, 0, 0); }
    else if (act == ACT_FIELD_GOAL) {
        // The game's kick is a mini-game that waits for the human's input (it would hand the CPU's kick to
        // the player). So the CPU's kick is settled by distance and reported through the same result call
        // the game uses after a real kick.
        double d = rb_cpuqb_fg_dist;
        double p = d <= 30 ? 0.97 : d <= 40 ? 0.97 - (d - 30) * 0.010 : d <= 50 ? 0.87 - (d - 40) * 0.020 : 0.67 - (d - 50) * 0.035;
        if (p < 0.15) p = 0.15;
        int good = arc4random_uniform(10000) / 10000.0 < p;
        rb_log("cpuqb: kicking a %.0f-yd field goal (%.0f%% chance) -> %s", d, p * 100, good ? "GOOD" : "NO GOOD");
        RValue code = {0}; code.kind = RV_REAL; code.real = good ? 6 : 7;     // ACTION_RESULT_FIELDGOAL_GOOD / _BAD
        RValue *av[1] = { &code };
        ((GMLScript)RB(0x1000f7200ULL))(self, other, &pr, 1, av);            // s_action_result
        RValue pr2 = {0}; pr2.kind = RV_UNDEFINED;
        ((GMLScript)RB(0x100475d8cULL))(self, other, &pr2, 0, 0);            // s_msg_clear (as the buttons do)
    }
    else if (act == ACT_PLAY_4TH) { rb_log("cpuqb: going for it"); ((GMLScript)RB(0x10008b700ULL))(self, other, &pr, 0, 0); }
    g_cpu_clearing = 0;
}

#define PURSUIT_BURST T(T_PURSUIT_BURST) //         // your players' burst while chasing an interception down
static double g_def_mult = 1.0;     // the burst boost the CPU defense got this play
static int holder_cb(CInstance *inst, void *ctx) { *(CInstance **)ctx = inst; return 1; }
static int fastest_cb(CInstance *inst, void *ctx) {
    double a = rb_get_inst_real(inst, SLOT_MY_ACCEL), *best = ctx;
    if (a > *best && a < 100) *best = a;
    return 0;
}
// your players on an interception: remember them, and they turn and chase flat out
static int pursuer_cb(CInstance *inst, void *ctx) {
    (void)ctx;
    if (g_ret.nof >= 12) return 0;
    g_ret.of[g_ret.nof++] = inst;
    double a = rb_get_inst_real(inst, SLOT_MY_ACCEL);
    if (a > 0 && a < 100) rb_set_inst_real(inst, SLOT_MY_ACCEL, a * PURSUIT_BURST);
    return 0;
}

// An interception: the defender who caught it keeps his Extreme burst boost as a ball carrier, and none
// of your players has one - returns were running ~2.1-3.0 px/frame against ~1.3-1.7 for your own
// carriers, so nobody could run him down. With the ball in his hands he is an ordinary player again.
static void int_return_fix(CInstance *ball, CInstance *other) {
    static CInstance *done;
    if (ball == done) return;
    RValue *h = rb_inst_var(ball, SLOT_HOLDER);
    CInstance *who = 0;
    if (h && h->kind != RV_UNDEFINED) { RValue c = *h; rb_with(&c, ball, other, holder_cb, &who); }
    if (!who) return;                                       // holder not set yet: try again next frame
    done = ball;
    double a = rb_get_inst_real(who, SLOT_MY_ACCEL);
    if (a > 0 && a < 100) {
        // Taking the boost off was not enough: defenders are simply quicker than your offense (about
        // 0.17 against 0.12), and two returns still went 70 and 80 yards at ~2.3-2.5 px/frame with nobody
        // able to close. Speed tracks this number, so the man with the ball is held just under your
        // fastest offensive player: he can still gain ground, but somebody can run him down.
        double fastest = 0;
        RValue of = rb_obj_ref(OBJ_PLAYER_OF);
        rb_with(&of, ball, other, fastest_cb, &fastest);
        double na = a / g_def_mult;
        if (fastest > 0 && na > fastest * T(T_RETURN_SPEED)) na = fastest * T(T_RETURN_SPEED);
        rb_set_inst_real(who, SLOT_MY_ACCEL, na);
        rb_log("int return: returner slowed (acceleration %.3f -> %.3f; your fastest player %.3f)", a, na, fastest);
    }
    // Slowing him was half of it. The other half: your offense barely chased - one return jogged 88
    // yards over 21 seconds with nobody getting to him. Now all of them turn and run him down.
    g_ret.who = who; g_ret.ball = ball; g_ret.nof = 0; g_ret.vx = g_ret.vy = 0;
    g_ret.px = rb_get_builtin_real(who, SLOT_X); g_ret.py = rb_get_builtin_real(who, SLOT_Y);
    RValue of2 = rb_obj_ref(OBJ_PLAYER_OF);
    rb_with(&of2, ball, other, pursuer_cb, 0);
    rb_log("int return: %d of your players in pursuit (x%.2f burst)", g_ret.nof, PURSUIT_BURST);
}

// ---- CPU athleticism: on Extreme the CPU's players get a burst boost every play (both sides of the ball)
#define CPU_ACCEL_BOOST T(T_CPU_OFF_BOOST)
typedef struct { int n; double before, after, mult; } BoostCtx;
static int boost_cb(CInstance *inst, void *vc) {
    BoostCtx *c = vc;
    static struct { CInstance *inst; double val; } seen[64]; static int ns;
    double a = rb_get_inst_real(inst, SLOT_MY_ACCEL);
    for (int i = 0; i < ns; i++)                            // players kept between plays: boost only once
        if (seen[i].inst == inst && seen[i].val == a) return 0;
    if (a > 0 && a < 100) {
        double b = a * c->mult;
        rb_set_inst_real(inst, SLOT_MY_ACCEL, b);
        b = rb_get_inst_real(inst, SLOT_MY_ACCEL);
        int k = 0;
        for (; k < ns; k++) if (seen[k].inst == inst) break;
        if (k == ns) { if (ns < 64) ns++; else k = (int)(rb_frame % 64); }
        seen[k].inst = inst; seen[k].val = b;
        c->before += a; c->after += b; c->n++;
    }
    return 0;
}
static void cpu_boost(CInstance *ball, CInstance *other, int cpu_offense) {
    static CInstance *done_ball;
    if (ball == done_ball) return;
    double st = rb_get_inst_real(ball, SLOT_STATE);
    if (st != 0) return;                                    // boost at the line, once per play
    done_ball = ball;
    if (rb_get_obj_real(OBJ_MASTER, SLOT_OP_DIFFICULTY) != 4) return;
    BoostCtx c = { 0, 0, 0, cpu_offense ? CPU_ACCEL_BOOST : 1.0 + T(T_CPU_DEF_BOOST) * rb_dbrain_dial() };
    RValue r = rb_obj_ref(cpu_offense ? OBJ_PLAYER_OF : OBJ_PLAYER_DF);
    rb_with(&r, ball, other, boost_cb, &c);
    if (!cpu_offense) g_def_mult = c.mult;
    static int logs;
    if (c.n && logs++ < 12) rb_log("boost: CPU %s x%.2f acceleration (%d players, avg %.3f -> %.3f)",
                                  cpu_offense ? "offense" : "defense", c.mult, c.n, c.before / c.n, c.after / c.n);
}

RValue *hook_update_ball(CInstance *self, CInstance *other, RValue *res, int argc, RValue **argv) {
    rb_frame++;
    g_match_live = 1;
    if (self != g_play_ball) {                                       // a new play: pick up edited settings and rules
        g_play_ball = self;
        rb_tune_load();
        rb_rules_load();
        rb_script_load();
        rb_tune_play_reset();
        rb_rules_new_play();
    }
    double human = rb_get_obj_real(OBJ_MASTER, SLOT_PLAYERTEAMINDEX);
    double poss = rb_get_obj_real(OBJ_CONTROLLER, SLOT_POSSESSION);
    int cpu_offense = human >= 0 && poss >= 0 && poss != human;
    g_cpu_ball = cpu_offense;
    if (human >= 0 && poss >= 0) cpu_boost(self, other, cpu_offense);
    rb_cpuqb_step(self, other, cpu_offense);
    rb_defcall_step(self, other, cpu_offense);
    rb_off_step(self, other, human >= 0 && poss >= 0 && !cpu_offense);
    if (cpu_offense) rb_off_cpu_drive();
    rb_no_windup_sack = cpu_offense ? rb_cpuqb_scrambled() : rb_off_scrambled();
    double los, fwd, qbx, qby;
    rb_cpuqb_info(&los, &fwd, &qbx, &qby);
    rb_def_step(self, other, cpu_offense, los, fwd, qbx, qby);
    rb_dlearn_step(self, other, human >= 0 && poss >= 0 && poss == human);
    double st = rb_get_inst_real(self, SLOT_STATE);
    if (st == 9 && human >= 0 && !cpu_offense) {
        int_return_fix(self, other);
        if (g_ret.who && g_ret.ball == self) {                      // where the returner is heading (smoothed)
            double rx = rb_get_builtin_real(g_ret.who, SLOT_X), ry = rb_get_builtin_real(g_ret.who, SLOT_Y);
            g_ret.vx += (rx - g_ret.px - g_ret.vx) * 0.2; g_ret.vy += (ry - g_ret.py - g_ret.vy) * 0.2;
            g_ret.px = rx; g_ret.py = ry;
        }
    } else if (g_ret.who && g_ret.ball == self) g_ret.who = 0;     // he's down (or scored)
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
    cpu_fourth_action(self, other);
    return r;
}

// 4th-down question on a CPU drive: let the CPU answer it
// The CPU's 4th-down question ("Play 4th down or punt?" / "...or go for field goal?"): let the CPU answer.
// Seen through slog (the script logs its text first thing) and through the script's own arguments.
static void fourth_check(const char *txt, const char *via) {
    int is4 = strstr(txt, "4th") || strstr(txt, "ourth");
    if (!is4 || !(strstr(txt, "unt") || strstr(txt, "oal"))) return;
    if (rb_cpuqb_fourth_msg) return;                                       // already caught this one
    if (!rb_obj_exists(OBJ_CONTROLLER)) return;
    double human = rb_get_obj_real(OBJ_MASTER, SLOT_PLAYERTEAMINDEX);
    double poss = rb_get_obj_real(OBJ_CONTROLLER, SLOT_POSSESSION);
    rb_log("4th-down question (via %s): human team %g, possession %g", via, human, poss);
    if (human >= 0 && poss >= 0 && poss != human) {                       // (down may still read 3 here)
        rb_cpuqb_fourth_msg = strstr(txt, "oal") ? 1 : 2;
        rb_log("message on CPU 4th down: \"%.60s\"", txt);
    }
}

RValue *hook_create_message(CInstance *self, CInstance *other, RValue *res, int argc, RValue **argv) {
    char b[64], txt[200];                                                  // read the text BEFORE the script runs
    txt[0] = 0;
    for (int i = 0; i < argc && i < 10 && argv; i++) {
        if (!argv[i] || (argv[i]->kind & 0xffffff) != RV_STRING) continue;
        const char *t = rb_rv_str(argv[i], b, sizeof b);
        if (!t || !t[0]) continue;
        int L = (int)strlen(txt);
        snprintf(txt + L, sizeof txt - L, "%s%s", L ? " | " : "", t);
    }
    RValue *r = ORIG(create_message)(self, other, res, argc, argv);
    fourth_check(txt, "args");
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

extern unsigned rb_spr_calls;
// route previews (s_draw_my_route): on CPU drives they'd show the player the CPU's routes
// s_check_ball_collision, run as each player: can he catch / intercept / tip the ball this frame
RValue *hook_ball_collision(CInstance *self, CInstance *other, RValue *res, int argc, RValue **argv) {
    if (g_match_live && (rb_off_blocker_ignores_ball(self) || rb_dbrain_ball_contact(self, other))) {
        res->kind = RV_UNDEFINED; res->i64 = 0;
        return res;
    }
    return ORIG(ball_collision)(self, other, res, argc, argv);
}

RValue *hook_draw_route(CInstance *self, CInstance *other, RValue *res, int argc, RValue **argv) {
    if (g_cpu_ball) { res->kind = RV_UNDEFINED; res->i64 = 0; return res; }
    return ORIG(draw_route)(self, other, res, argc, argv);
}

// every sprite draw (draw_sprite_ext's engine routine): in 3D the crowd strips are stood up as bleachers
long hook_spr_draw(CInstance *self, long sprite, long colour, float sub, float x, float y,
                   float xs, float ys, float rot, float alpha) {
    RbDrawSprFn orig = (RbDrawSprFn)RB_TRAMP(HK_spr_draw);
    rb_spr_calls++;
    if (!rb_cam3d_in_gui_pass && rb_cam3d_on && rb_cam3d_active())
        return rb_cam3d_spr_draw(orig, self, sprite, colour, sub, x, y, xs, ys, rot, alpha);
    return orig(self, sprite, colour, sub, x, y, xs, ys, rot, alpha);
}

// =============================================================== player control on defense
// One of your players during an interception return: take the angle and go get him.
static void int_pursuit(CInstance *p) {
    if (!g_ret.who || !g_match_live || p == g_ret.who) return;
    int mine = 0;
    for (int i = 0; i < g_ret.nof; i++) if (g_ret.of[i] == p) { mine = 1; break; }
    if (!mine) return;
    double x = rb_get_builtin_real(p, SLOT_X), y = rb_get_builtin_real(p, SLOT_Y);
    double d = hypot(g_ret.px - x, g_ret.py - y);
    double lead = d / 2.4;                                          // frames until he could be reached
    if (lead > 45) lead = 45;
    double tx = g_ret.px + g_ret.vx * lead, ty = g_ret.py + g_ret.vy * lead;
    double a = atan2(-(ty - y), tx - x) * 57.29577951308232;
    rb_set_inst_real(p, SLOT_JDIR, a < 0 ? a + 360 : a);
    rb_set_inst_real(p, SLOT_JFORCE, 1.0);
}

RValue *hook_get_destination(CInstance *self, CInstance *other, RValue *res, int argc, RValue **argv) {
    RValue *r = ORIG(get_destination)(self, other, res, argc, argv);
    int_pursuit(self);
    rb_defcall_after_ai(self);           // the call shapes the AI defenders...
    rb_def_after_ai(self);               // ...the player's own man stays his
    rb_dlearn_after_ai(self);
    rb_off_after_ai(self);               // play action / pump fake (player's offense)
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

extern CInstance *rb_game_controller;
void hook_controller_gui(CInstance *self, CInstance *other) {
    rb_game_controller = self;
    rb_cpuqb_fourth_tick();                    // runs even while the game waits on the 4th-down message
    cpu_fourth_action(self, other);
    rb_cam3d_gui_pass(self, gui_draw_player, gui_draw_ball, gui_draw_text);      // standing players + sky, under the HUD
    ORIG_EV(controller_gui)(self, other);
    if (g_match_live) rb_off_gui(self);                                  // on top of the HUD
}

// =============================================================== install
void rb_install_hooks(void **tab) {
#define HOOK(name, va, insn) tab[TAB_HOOK_BASE + HK_##name] = (void *)hook_##name;
#define CALLSITE(name, site, target) tab[TAB_HOOK_BASE + HK_##name] = (void *)call_##name;
#include "hooks.def"
#undef HOOK
#undef CALLSITE
    tab[TAB_GATE_ATTACK] = (void *)rb_gate_attack;
    tab[TAB_NO_WINDUP] = (void *)&rb_no_windup_sack;
}
