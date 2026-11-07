// defcall.c - the player calls the defense on CPU drives.
//
// Before every CPU snap the player sees the formation for a moment, then picks a call:
//   Blitz       - linebackers join the rush: wrecks runs and deep drops, gets burned by quick throws
//   Man         - tight on short routes, weak against the run (backs turned)
//   Short zone  - squats on short routes and the run, gets beaten deep
//   Deep zone   - takes away the deep ball, soft against the run
// The CPU's play (run / short / deep) shows in its formation (cpuqb.c): back right behind the QB =
// run-heavy, back offset or gone = pass, receivers split wide = deep shots more likely.
//
// The call decides how good the AI defenders are on that play:
//   right call (+1): coverage beats the receiver to the spot, the run gets swarmed
//   neutral     (0): coverage trails a step, little run help
//   wrong call (-1): coverage trails badly, run defenders hold their spots, rushers contain
// Rushers only get home on a blitz or a right read; otherwise sacks come from the player himself.
// The player's own defender is never steered.
#include "rb.h"
#include "sys.h"
#include "ui.h"
#include "cpuqb.h"
#include "defense.h"
#include "defcall.h"

enum { CALL_NONE, CALL_BLITZ, CALL_MAN, CALL_SZONE, CALL_DZONE };
enum { PLAY_RUN, PLAY_SHORT, PLAY_DEEP };
static const char *CALL_NAME[] = { "-", "Blitz", "Man", "Short zone", "Deep zone" };
static const char *PLAY_NAME[] = { "RUN", "SHORT PASS", "DEEP PASS" };
//                                   run short deep
static const int MATCH[5][3] = { {  0,  0,  0 },
                                 { +1, -1, +1 },     // blitz
                                 { -1, +1,  0 },     // man
                                 { +1, +1, -1 },     // short zone
                                 { -1,  0, +1 } };   // deep zone

#define LOOK_FRAMES  45          // formation on screen before the call menu
#define RUSH_DEPTH   45.0        // defenders this close to the line at the snap are rushers

static volatile int g_asked, g_answer, g_open;   // menu state (answer written on the main thread)
static int g_call, g_play = -1, g_match;
static char g_last[160];                      // last play's verdict, shown in the next menu
static int g_rights, g_calls;

static struct {
    int live;                                 // CPU play in progress with a call
    double st, prev_st;
    unsigned t, t_snap, t_throw, t_hand, t_catch;
    double los, fwd, qbx, qby;
    struct { CInstance *inst; double x, y; int rusher, blitz; } d[16]; int nd;
    CInstance *ball;
} S;

static void on_call(int c) {                  // main thread
    g_answer = (c >= 1 && c <= 4) ? c : CALL_MAN;
    g_open = 0;
}

// cpuqb asks before the snap: returns 1 once the call is in
int rb_defcall_ready(int down, double yards, unsigned look_frames) {
    if (g_answer) return 1;
    if (look_frames < LOOK_FRAMES) return 0;
    if (!g_asked && !g_open) {
        g_asked = 1; g_open = 1;
        static char title[64], msg[200];
        const char *dn = down <= 1 ? "1st" : down == 2 ? "2nd" : down == 3 ? "3rd" : "4th";
        snprintf(title, sizeof title, "%s & %.0f  -  your call", dn, yards < 1 ? 1 : yards);
        snprintf(msg, sizeof msg, "%s%sBack behind QB = run, beside = pass. Receivers split wide = deep.",
                 g_last, g_last[0] ? "\n" : "");
        static const char *b[4] = { "Blitz", "Man", "Short zone", "Deep zone" };
        rb_ui_side_menu(title, msg, b, 4, on_call);
    }
    return 0;
}

// cpuqb, at the snap: what the CPU is running
void rb_defcall_snap(int is_run, int deep) {
    g_call = g_answer ? g_answer : CALL_MAN;
    g_play = is_run ? PLAY_RUN : deep ? PLAY_DEEP : PLAY_SHORT;
    g_match = MATCH[g_call][g_play];
    g_asked = 0; g_answer = 0;
    g_calls++; if (g_match > 0) g_rights++;
    rb_log("defcall: %s vs %s -> %s (%d/%d right reads)", CALL_NAME[g_call], PLAY_NAME[g_play],
           g_match > 0 ? "RIGHT CALL" : g_match < 0 ? "WRONG CALL" : "neutral", g_rights, g_calls);
    snprintf(g_last, sizeof g_last, "Last: %s - %s %s (%d/%d)", PLAY_NAME[g_play], CALL_NAME[g_call],
             g_match > 0 ? "RIGHT" : g_match < 0 ? "wrong" : "ok", g_rights, g_calls);
    S.live = 1; S.t_snap = S.t; S.t_throw = S.t_hand = S.t_catch = 0; S.nd = 0;
}

int rb_defcall_blitz(void) { return S.live && g_call == CALL_BLITZ; }

void rb_defcall_reset(void) { if (!g_open) { g_asked = 0; g_answer = 0; } S.live = 0; }

typedef struct { int n; } RoleCtx;
static int role_cb(CInstance *inst, void *vc) {
    (void)vc;
    if (S.nd >= 16) return 1;
    double x = rb_get_builtin_real(inst, SLOT_X), y = rb_get_builtin_real(inst, SLOT_Y);
    if (x < -9000) return 0;
    S.d[S.nd].inst = inst; S.d[S.nd].x = x; S.d[S.nd].y = y;
    S.d[S.nd].rusher = fabs(x - S.los) < RUSH_DEPTH; S.d[S.nd].blitz = 0;
    S.nd++;
    return 0;
}

// once per ball step while the CPU has the ball
void rb_defcall_step(CInstance *ball, CInstance *other, int cpu_offense) {
    S.t++;
    if (!cpu_offense) { S.live = 0; return; }
    S.ball = ball;
    S.prev_st = S.st;
    S.st = rb_get_inst_real(ball, SLOT_STATE);
    rb_cpuqb_info(&S.los, &S.fwd, &S.qbx, &S.qby);
    if (!S.live) return;
    if (S.nd == 0 && S.st != 0) {                               // just snapped: who rushes
        RValue r = rb_obj_ref(OBJ_PLAYER_DF);
        rb_with(&r, ball, other, role_cb, 0);
        if (g_call == CALL_BLITZ) {                             // send the two nearest linebackers too
            for (int k = 0; k < 2; k++) {
                int best = -1; double bd = 1e9;
                for (int i = 0; i < S.nd; i++)
                    if (!S.d[i].rusher && !S.d[i].blitz && fabs(S.d[i].x - S.los) < bd) { bd = fabs(S.d[i].x - S.los); best = i; }
                if (best >= 0) S.d[best].blitz = 1;
            }
        }
    }
    if (S.st == 3 && S.prev_st != 3) S.t_throw = S.t;
    if ((S.st == 19 || S.st == 5) && !S.t_hand && !S.t_throw) S.t_hand = S.t;
    if (S.st == 5 && S.prev_st == 3) S.t_catch = S.t;
    if (S.st == 4 || S.st == 6 || S.st == 7 || S.st == 9 || S.st == 11) S.live = 0;   // play over
}

static double dir_to(double fx, double fy, double tx, double ty) {
    double a = atan2(-(ty - fy), tx - fx) * 57.29577951308232;
    return a < 0 ? a + 360 : a;
}
static void steer(CInstance *p, double x, double y, double tx, double ty, double force) {
    rb_set_inst_real(p, SLOT_JDIR, dir_to(x, y, tx, ty));
    rb_set_inst_real(p, SLOT_JFORCE, force);
}

// after the game's AI picks a destination for a player
void rb_defcall_after_ai(CInstance *player) {
    if (!S.live || S.nd == 0) return;
    if (player == rb_def_controlled()) return;                  // the player's own man is his
    int i = 0;
    for (; i < S.nd; i++) if (S.d[i].inst == player) break;
    if (i == S.nd) return;
    double x = rb_get_builtin_real(player, SLOT_X), y = rb_get_builtin_real(player, SLOT_Y);
    if (x < -9000) return;
    int rusher = S.d[i].rusher || S.d[i].blitz;
    double st = S.st;

    if (st == 1 || st == 2) {                                   // QB dropping / reading
        if (!rusher) return;
        unsigned since = S.t - S.t_snap;
        if (g_call == CALL_BLITZ && g_match < 0) {              // they saw it coming: blitz gets picked up
            if (since < 18) { double cx = S.qbx + S.fwd * 50, cy = y; steer(player, x, y, cx, cy, 0.6); }
            else steer(player, x, y, S.qbx, S.qby, 0.7);
        }
        else if (g_call == CALL_BLITZ || g_match > 0) steer(player, x, y, S.qbx, S.qby, 1.0);     // get home
        else {                                                  // contain: push the pocket, don't finish
            double cx = S.qbx + S.fwd * 45, cy = S.qby + (y > S.qby ? 25 : -25);
            if (hypot(cx - x, cy - y) > 6) steer(player, x, y, cx, cy, 0.7);
        }
        return;
    }
    if (st == 3) {                                              // ball in the air
        double lx, ly, rx, ry;
        if (!rb_cpuqb_target(&lx, &ly)) return;
        int have_r = rb_cpuqb_receiver(&rx, &ry);
        double d = hypot(lx - x, ly - y);
        unsigned since = S.t - S.t_throw;
        if (g_match > 0) {                                      // right call: beat him to the spot
            if (d < 170 && since >= 4) steer(player, x, y, lx, ly, 1.0);
        } else if (have_r) {                                    // trail the receiver
            double lag = g_match < 0 ? 26 : 10, radius = g_match < 0 ? 150 : 110;
            if (d < radius) {
                double tx = rx - (lx - rx) * 0.15 - S.fwd * lag;  // a step (or three) behind him
                steer(player, x, y, tx, ry, 1.0);
            }
        }
        return;
    }
    if (st == 19 || st == 5) {                                  // run (or a catch, but then live is 0 soon)
        double bx = rb_get_builtin_real(S.ball, SLOT_X), by = rb_get_builtin_real(S.ball, SLOT_Y);
        if (S.t_throw) {                                        // after the catch
            unsigned sc = S.t - S.t_catch;
            if (!S.t_catch || g_match >= 0) return;
            // wrong call (a blitz that got read, a zone the throw sat behind...): nobody home -
            // defenders take bad angles for a moment and he turns it upfield
            int ahead = (x - bx) * S.fwd > 0;
            unsigned window = g_call == CALL_BLITZ ? 45 : 28;
            if (sc < window && ahead && hypot(bx - x, by - y) < 220) steer(player, x, y, bx - S.fwd * 45, by, 1.0);
            return;
        }
        unsigned since = S.t - S.t_hand;
        if (g_match > 0) {
            if (hypot(bx - x, by - y) < 230) steer(player, x, y, bx + S.fwd * 12, by, 1.0);   // swarm, meet him early
        } else if (g_match < 0 && since < 45 && !rusher) {
            steer(player, x, y, S.d[i].x, S.d[i].y, 0.8);       // backs turned / in their zones
        }
    }
}
