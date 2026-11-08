// cpuqb.c - CPU quarterback for on-field CPU drives.
// Drives the game's own touch controls through the virtual finger (input.c):
//   pass: finger down (state 0 -> 1), pull back past the 20px deadzone (-> 2, aiming),
//         read receivers, then hold the finger at the slingshot point for the chosen throw and
//         release (-> 3). The ball flies opposite the drag; speed = drag * 0.116.
//   run:  tap the running back before the snap (-> 19 handoff -> 5); the runner runs on his own.
// Ball physics measured from real throws: speed *= 0.986 per frame, zvel0 = 0.35 * speed,
// gravity 0.08 per frame -> airtime ~ 8.75 * speed frames, distance = v*(1-0.986^T)/0.014.
#include "rb.h"
#include "sys.h"
#include "input.h"
#include "cpuqb.h"
#include "brain.h"
#include "defcall.h"
#include "defense.h"

#define THROW_K      0.1159     // fallback ball speed per pixel of drag (a strong arm)
#define DRAG_DECAY   0.986
#define AIR_K        8.75       // frames of flight per unit of speed
#define DRAG_MIN     24.0
#define DRAG_MAX     140.0
#define P0X          400.0      // where the finger goes down (GUI)
#define P0Y          190.0

enum { PH_IDLE, PH_PRESNAP, PH_HANDOFF, PH_PULL, PH_READ, PH_AIM, PH_SCRAMBLE_GO, PH_SCRAMBLE, PH_KICK_TAP, PH_FG, PH_DONE };

typedef struct { CInstance *inst; double x, y, vx, vy; int seen; double hd, turn; } Track;

static struct {
    CInstance *ball;
    int phase, is_run, deep, scrambled;
    double down, yards;
    unsigned t, t_phase, read_frames, hold;
    double los_x, fwd;            // line of scrimmage x, +1/-1 = offense's forward x direction
    CInstance *qb;
    double qbx, qby;
    Track of[16], df[16];
    int nof, ndf;
    int tgt;                       // index into of[]
    double aim_x, aim_y, drag;
    double k, mtp;                // this QB: ball speed per pixel of drag, his my_throw_power
    int measured;
    int fourth_done, fg_peak_frames; double fg_last_pow;
    double btn_x, btn_y;
    unsigned sw_t, sw_cool; double sw_dir, sw_x, sw_y;   // juke swipe in progress
    double prev_st, th_open, th_hdist; int thrown, caught, ran, scored;   // results, for learning
    struct { CInstance *inst; double x, y; } d0[16]; int nd0; unsigned t_rel;   // defenders at release
    int called;                                                         // defensive call made for this play
} Q;

static int g_fg_pending;       // CPU chose a field goal; survives the kick set-up
int rb_cpuqb_fourth_msg;       // 4th-down question on screen: 1 = FG offered, 2 = punt offered
static unsigned g_fourth_wait;
static int g_fourth_answered;  // the question was answered for this down (don't punt off the field button)
int rb_cpuqb_action;           // what hooks.c should run after the ball update (ACT_*)

static double g_k_per_mtp;       // speed-per-drag / my_throw_power, measured from a real throw

static double frand(void) { return arc4random_uniform(10000) / 10000.0; }

// ---------------------------------------------------------------- tracking players
typedef struct { Track *t; int *n; } CollectCtx;
static int collect(CInstance *inst, void *vctx) {
    CollectCtx *c = vctx;
    double x = rb_get_builtin_real(inst, SLOT_X), y = rb_get_builtin_real(inst, SLOT_Y);
    for (int i = 0; i < *c->n; i++) {
        Track *t = &c->t[i];
        if (t->inst == inst) {
            double vx = x - t->x, vy = y - t->y;
            t->vx = t->vx * 0.6 + vx * 0.4; t->vy = t->vy * 0.6 + vy * 0.4;   // smoothed per-frame velocity
            if (hypot(t->vx, t->vy) > 0.5) {                                    // how hard is he cutting?
                double h = atan2(t->vy, t->vx) * 57.29577951308232, dh = h - t->hd;
                while (dh > 180) dh -= 360;
                while (dh < -180) dh += 360;
                t->turn = t->turn * 0.8 + fabs(dh) * 0.2;
                t->hd = h;
            }
            t->x = x; t->y = y; t->seen = 1;
            return 0;
        }
    }
    if (*c->n < 16) {
        Track *t = &c->t[(*c->n)++];
        t->inst = inst; t->x = x; t->y = y; t->vx = t->vy = 0; t->seen = 1;
    }
    return 0;
}

static void scan_players(CInstance *self, CInstance *other) {
    for (int i = 0; i < Q.nof; i++) Q.of[i].seen = 0;
    for (int i = 0; i < Q.ndf; i++) Q.df[i].seen = 0;
    RValue r = rb_obj_ref(OBJ_PLAYER_OF);
    CollectCtx c1 = { Q.of, &Q.nof };
    rb_with(&r, self, other, collect, &c1);
    r = rb_obj_ref(OBJ_PLAYER_DF);
    CollectCtx c2 = { Q.df, &Q.ndf };
    rb_with(&r, self, other, collect, &c2);
}

static int grab_first(CInstance *inst, void *ctx) { *(CInstance **)ctx = inst; return 1; }

static CInstance *ball_holder(CInstance *ball, CInstance *other) {
    RValue *h = rb_inst_var(ball, SLOT_HOLDER);
    if (!h || h->kind == RV_UNDEFINED) return 0;
    RValue copy = *h;
    CInstance *got = 0;
    rb_with(&copy, ball, other, grab_first, &got);
    return got;
}

// ---------------------------------------------------------------- throw math
static double throw_range(double v) {
    double T = AIR_K * v;
    return v * (1.0 - pow(DRAG_DECAY, T)) / (1.0 - DRAG_DECAY);
}

static double speed_for_range(double R) {   // invert throw_range by bisection
    double lo = 1.0, hi = 16.0;
    for (int i = 0; i < 30; i++) {
        double mid = (lo + hi) / 2;
        if (throw_range(mid) < R) lo = mid; else hi = mid;
    }
    return (lo + hi) / 2;
}

static double nearest_def(double x, double y, double t_ahead) {
    double best = 1e9;
    for (int i = 0; i < Q.ndf; i++) {
        if (!Q.df[i].seen) continue;
        double dx = Q.df[i].x + Q.df[i].vx * t_ahead - x, dy = Q.df[i].y + Q.df[i].vy * t_ahead - y;
        double d = hypot(dx, dy);
        if (d < best) best = d;
    }
    return best;
}

// lead point for receiver i: where he'll be when the ball arrives
static void lead_point(int i, double *lx, double *ly, double *speed) {
    Track *r = &Q.of[i];
    double x = r->x, y = r->y, v = 6;
    for (int k = 0; k < 4; k++) {
        v = speed_for_range(hypot(x - Q.qbx, y - Q.qby));
        double T = AIR_K * v;
        x = r->x + r->vx * T;
        y = r->y + r->vy * T;
    }
    *lx = x; *ly = y; *speed = v;
}

// Defenders react once the ball is up: assume each keeps his momentum for part of the flight and
// then gains DEF_CLOSE px/frame on the ball. Clearance = how far the nearest one still is.
#define DEF_CLOSE  (rb_brain.def_close)    // px/frame defenders gain on the ball (learned from every pass)
#define SAFE_OPEN  (rb_brain.safe_open)   // clearance needed to call a man open (learned)
#define LANE_MIN   10.0       // clearance needed along the catchable part of the flight

static double clearance_at(double bx, double by, double t) {
    double best = 1e9;
    for (int i = 0; i < Q.ndf; i++) {
        if (!Q.df[i].seen) continue;
        double react = t < 8 ? t : 8;                         // keeps his line for a few frames, then breaks on it
        double px = Q.df[i].x + Q.df[i].vx * react, py = Q.df[i].y + Q.df[i].vy * react;
        double d = hypot(px - bx, py - by) - DEF_CLOSE * (t - react);
        if (d < best) best = d;
    }
    return best;
}

// worst clearance over the low (catchable) part of the flight toward lx,ly at speed v
static double lane_clearance(double lx, double ly, double v) {
    double dx = lx - Q.qbx, dy = ly - Q.qby, len = hypot(dx, dy);
    if (len < 1) return 1e9;
    double ux = dx / len, uy = dy / len, T = AIR_K * v, worst = 1e9;
    for (double f = 0.55; f < 0.96; f += 0.1) {
        double t = f * T, sd = v * (1.0 - pow(DRAG_DECAY, t)) / (1.0 - DRAG_DECAY);
        double c = clearance_at(Q.qbx + ux * sd, Q.qby + uy * sd, t);
        if (c < worst) worst = c;
    }
    return worst;
}

// best receiver right now; returns index or -1. *open_out = min(catch, lane) clearance
static int pick_receiver(double *score_out, double *open_out) {
    int best = -1;
    double best_score = -1e9, best_open = -1e9;
    for (int i = 0; i < Q.nof; i++) {
        Track *r = &Q.of[i];
        if (!r->seen || r->inst == Q.qb) continue;
        double depth = (r->x - Q.los_x) * Q.fwd;
        double spd = hypot(r->vx, r->vy);
        if (depth > -22 && depth < 22 && spd < 1.6) continue;   // linemen: on the line and slow
        double lx, ly, v;
        lead_point(i, &lx, &ly, &v);
        if (v / Q.k > DRAG_MAX) continue;                   // out of range
        double T = AIR_K * v;
        double at_catch = clearance_at(lx, ly, T);
        double lane = lane_clearance(lx, ly, v);
        double open = at_catch < lane + 5 ? at_catch : lane + 5;
        double hx, hy;                                        // the defender the player controls is smarter:
        if (rb_def_human(&hx, &hy)) {                         // he gets extra respect, learned from results
            double hd = hypot(hx - lx, hy - ly) - DEF_CLOSE * T * 1.5 - rb_brain.hdef_margin;
            if (hd < open) open = hd;
        }
        open += rb_brain.bias;                                // learned: how much roomier throws really are
        if (r->turn > 6) open -= 15;                          // mid-cut: the lead won't hold
        double gain = (lx - Q.los_x) * Q.fwd;
        double score = open * 0.8 + gain * (Q.deep ? 0.45 : 0.25) - (open < SAFE_OPEN ? 80 : 0) + frand() * 10;
        if (gain < 40) score -= 20;                          // dump-offs at the line are a last resort
        if (Q.down >= 3 && gain < Q.yards * 20 - 20) score -= 25;   // short of the sticks on 3rd/4th
        if (score > best_score) { best_score = score; best = i; best_open = open; }
    }
    if (score_out) *score_out = best_score;
    if (open_out) *open_out = best_open;
    return best;
}

static int find_rb(void) {
    int best = -1;
    double bd = 1e9;
    for (int i = 0; i < Q.nof; i++) {
        Track *r = &Q.of[i];
        if (!r->seen || r->inst == Q.qb) continue;
        double behind = (Q.qbx - r->x) * Q.fwd;     // positive = behind the QB
        if (behind < 20) continue;
        double d = hypot(r->x - Q.qbx, r->y - Q.qby);
        if (d < bd) { bd = d; best = i; }
    }
    return bd < 220 ? best : -1;
}

// ---------------------------------------------------------------- per-frame driver
static void new_play(CInstance *ball) {
    memset(&Q, 0, sizeof Q);
    Q.ball = ball;
    Q.phase = PH_PRESNAP;
    Q.tgt = -1;
    if (g_fg_pending) {                    // field goal formation: go straight to the kick
        Q.phase = PH_FG; Q.fwd = 1; Q.fg_last_pow = -1;
        rb_log("cpuqb: field goal set up");
    }
}

static void finger(int down, double gx, double gy) {
    rb_vf.down = down; rb_vf.gui_x = gx; rb_vf.gui_y = gy;
}

// The game asks "Play 4th down or go for field goal?" (or punt) before the CPU's 4th-down play.
// Hold the human off the buttons for a moment, then answer it like a coach would.
// 4th down: the CPU always weighs all three - go for it, field goal, punt - whatever the game's question
// offered. Roughly modern-NFL: short yardage + good field position = go; in range = kick it; else punt.
// scrimmage = yards from midfield from the offense's side (-30 = own 20, +20 = opponent's 30)
double rb_cpuqb_fg_dist;        // length of the field goal the CPU chose
static int fourth_choice(int fg_offered, const char *how) {
    double yards = rb_get_obj_real(OBJ_CONTROLLER, SLOT_YARDS);
    double scr = rb_get_obj_real(OBJ_CONTROLLER, SLOT_SCRIMMAGE);
    if (!(scr > -60 && scr < 60)) scr = 0;
    double to_goal = 50 - scr, fg_dist = to_goal + 17, p;
    int in_range = fg_dist <= 57;
    rb_cpuqb_fg_dist = fg_dist;
    if (in_range)            p = yards <= 1 ? 0.55 : yards <= 2 ? 0.30 : yards <= 3 && to_goal <= 5 ? 0.35 : 0.03;
    else if (to_goal <= 45)  p = yards <= 2 ? 0.75 : yards <= 4 ? 0.50 : yards <= 6 ? 0.20 : 0.05;   // no man's land
    else if (to_goal <= 60)  p = yards <= 1 ? 0.65 : yards <= 2 ? 0.40 : yards <= 4 ? 0.15 : 0.0;    // midfield
    else if (to_goal <= 75)  p = yards <= 1 ? 0.40 : yards <= 2 ? 0.15 : 0.0;                         // own 25-40
    else                     p = yards <= 1 ? 0.15 : 0.0;                                             // backed up
    int act;
    if (yards > 0 && frand() < p) act = ACT_PLAY_4TH;
    else if (fg_dist <= 47 || (in_range && frand() < 0.6)) act = ACT_FIELD_GOAL;
    else act = ACT_PUNT;
    rb_log("cpuqb: 4th down (%s, game offered %s): 4th and %.1f, %.0f yds to goal (%.0f-yd kick), go %.0f%% -> %s",
           how, fg_offered ? "field goal" : "punt", yards, to_goal, fg_dist, p * 100,
           act == ACT_PLAY_4TH ? "GO FOR IT" : act == ACT_FIELD_GOAL ? "FIELD GOAL" : "PUNT");
    return act;
}

static int fourth_decide(int fg_offered) { return fourth_choice(fg_offered, "question"); }

// Every frame (from the controller's GUI draw, which keeps running while the game waits on a message -
// the ball's step doesn't): answer the CPU's 4th-down question a couple of frames after it appears.
void rb_cpuqb_fourth_tick(void) {
    if (!rb_cpuqb_fourth_msg) { g_fourth_wait = 0; return; }
    rb_vf.active = 1; rb_vf.down = 0;                          // the human can't press the CPU's buttons
    if (++g_fourth_wait < 3) return;
    rb_cpuqb_action = fourth_decide(rb_cpuqb_fourth_msg == 1);
    rb_cpuqb_fourth_msg = 0; g_fourth_wait = 0; g_fourth_answered = 1;   // (field goals are settled in hooks.c)
}

void rb_cpuqb_step(CInstance *ball, CInstance *other, int cpu_offense) {
    rb_vf_frame();
    if (rb_cpuqb_fourth_msg) return;                    // the question is answered from rb_cpuqb_fourth_tick
    double st = rb_get_inst_real(ball, SLOT_STATE);
    if (!cpu_offense) { rb_vf.active = 0; Q.phase = PH_IDLE; Q.ball = 0; g_fg_pending = 0; g_fourth_answered = 0; return; }
    if (ball != Q.ball) new_play(ball);
    if (st != Q.prev_st && Q.fwd) {                                     // results -> learning
        double bx = rb_get_builtin_real(ball, SLOT_X), yds = (bx - Q.los_x) * Q.fwd / 20.5;
        if (Q.thrown && Q.prev_st == 3 && (st == 5 || st == 7 || st == 9) && Q.t > Q.t_rel) {
            double T = Q.t - Q.t_rel, best = -1e9, gap = 1e9;
            int nearest = -1;
            double tx = rb_get_builtin_real(ball, SLOT_X), ty = rb_get_builtin_real(ball, SLOT_Y);
            for (int a = 0; a < Q.nd0; a++)
                for (int b = 0; b < Q.ndf; b++) {
                    if (Q.df[b].inst != Q.d0[a].inst || !Q.df[b].seen) continue;
                    double da = hypot(Q.d0[a].x - tx, Q.d0[a].y - ty), db = hypot(Q.df[b].x - tx, Q.df[b].y - ty);
                    if (db < gap) { gap = db; nearest = a; best = (da - db) / T; }
                }
            if (nearest >= 0) {
                rb_brain_closing(best, gap, st == 9 ? BR_INT : st == 7 ? BR_INCOMPLETE : BR_COMPLETE);
                if (st != 9) rb_brain_calibrate(Q.th_open - rb_brain.bias, gap);   // raw model vs reality
            }
        }
        if (Q.thrown && !Q.scored) {
            if (Q.prev_st == 3 && st == 9) { Q.scored = 1; rb_brain_pass(BR_INT, 0, Q.th_open, Q.th_hdist, rb_def_jumped()); }
            else if (Q.prev_st == 3 && st == 7) { Q.scored = 1; rb_brain_pass(BR_INCOMPLETE, 0, Q.th_open, Q.th_hdist, rb_def_jumped()); }
            else if (Q.prev_st == 3 && st == 5) Q.caught = 1;
        }
        if (!Q.scored && Q.prev_st == 2 && st == 11) { Q.scored = 1; rb_brain_gave_up(1); }
        if (Q.thrown && !Q.scored) {
            if (0) {}
            else if (Q.caught && (st == 4 || st == 6)) { Q.scored = 1; rb_brain_pass(BR_COMPLETE, yds, Q.th_open, Q.th_hdist, rb_def_jumped()); }
        } else if (Q.ran && !Q.scored && (st == 4 || st == 6)) { Q.scored = 1; rb_brain_run(yds); }
        Q.prev_st = st;
    }

    // virtual finger owns the input while the CPU's play is live
    int live = (st == 0 || st == 1 || st == 2 || st == 3 || st == 5 || st == 13 || st == 18 || st == 19)
               || Q.phase == PH_KICK_TAP || Q.phase == PH_FG;
    rb_vf.active = live;
    if (!live) { rb_vf.down = 0; return; }

    Q.t++;
    if (st == 0 || !Q.qb || Q.phase == PH_DONE) Q.qb = ball_holder(ball, other);
    if (Q.qb) { Q.qbx = rb_get_builtin_real(Q.qb, SLOT_X); Q.qby = rb_get_builtin_real(Q.qb, SLOT_Y); }
    scan_players(ball, other);
    rb_vf.room_x = Q.qbx;
    rb_vf.room_y = Q.qby + 100;     // a touch below the QB starts the aim (away from the RB)

    if (!Q.fwd) {                                              // first frame with a QB: orient
        if (!Q.qb) return;
        Q.mtp = rb_get_inst_real(Q.qb, SLOT_MY_THROW_POWER);
        if (g_k_per_mtp > 0 && Q.mtp > 0) Q.k = Q.mtp * g_k_per_mtp;
        else if (Q.mtp > 0) {
            Q.k = Q.mtp;                                        // measured: speed per drag pixel ~= my_throw_power
        }
        if (!(Q.k > 0.03 && Q.k < 0.3)) Q.k = THROW_K;
        double sx = 0; int n = 0;
        for (int i = 0; i < Q.ndf; i++) if (Q.df[i].seen) { sx += Q.df[i].x; n++; }
        if (!n) return;
        Q.fwd = (sx / n < Q.qbx) ? -1 : 1;                     // defense lines up in front
        Q.los_x = Q.qbx + Q.fwd * 35;
        Q.t_phase = 40 + arc4random_uniform(40);              // snap after this many frames
        Q.down = rb_get_obj_real(OBJ_CONTROLLER, SLOT_DOWN);
        if (Q.down < 4) g_fourth_answered = 0;
        Q.yards = rb_get_obj_real(OBJ_CONTROLLER, SLOT_YARDS);
        rb_brain_load();
        double p_run = rb_brain_run_rate();
        if (Q.yards > 0 && Q.yards <= 2) p_run += 0.25;
        else if (Q.yards >= 8) p_run -= 0.16;
        if (Q.down >= 3 && Q.yards >= 4) p_run = 0.10;
        // Play mix for the called defense: the player needs runs to read, so it doesn't follow the
        // brain's run/pass yards as strictly. Then the formation shows it (80% honest):
        //   back right behind the QB = run, back beside the QB = pass,
        //   receivers split extra wide = deep shot, pulled in tight = short.
        p_run = 0.42 + (rb_brain_run_rate() - 0.38) * 0.5;
        if (Q.yards > 0 && Q.yards <= 2) p_run += 0.25;
        else if (Q.down >= 2 && Q.yards >= 8) p_run -= 0.12;
        if (Q.down >= 3 && Q.yards >= 4) p_run = 0.12;
        double p_deep = Q.down >= 3 && Q.yards >= 10 ? 0.35 : 0.18;
        Q.is_run = frand() < p_run;
        Q.deep = !Q.is_run && frand() < p_deep;
        int show_run = Q.is_run ^ (frand() < 0.20);
        int show_deep = !show_run && (Q.deep ^ (frand() < 0.15));
        rb_defcall_disguise(show_run != Q.is_run || (!show_run && !Q.is_run && show_deep != Q.deep));
        int rbi = find_rb();
        if (rbi >= 0) {
            double side = frand() < 0.5 ? -1 : 1;
            double nx = show_run ? Q.qbx - Q.fwd * 50 : Q.qbx - Q.fwd * 26;
            double ny = show_run ? Q.qby : Q.qby + side * 44;
            rb_set_builtin_real(Q.of[rbi].inst, SLOT_X, nx); rb_set_builtin_real(Q.of[rbi].inst, SLOT_Y, ny);
            Q.of[rbi].x = nx; Q.of[rbi].y = ny;
        }
        if (!show_run) {                                       // outside receivers: wide or tight
            int top = -1, bot = -1;
            for (int i = 0; i < Q.nof; i++) {
                if (!Q.of[i].seen || Q.of[i].inst == Q.qb || i == rbi) continue;
                if (top < 0 || Q.of[i].y < Q.of[top].y) top = i;
                if (bot < 0 || Q.of[i].y > Q.of[bot].y) bot = i;
            }
            double push = show_deep ? 40 : -22;
            if (top >= 0 && Q.of[top].y < Q.qby - 60) {
                double ny = Q.of[top].y - push; if (ny < 145) ny = 145;
                rb_set_builtin_real(Q.of[top].inst, SLOT_Y, ny); Q.of[top].y = ny;
            }
            if (bot >= 0 && bot != top && Q.of[bot].y > Q.qby + 60) {
                double ny = Q.of[bot].y + push; if (ny > 455) ny = 455;
                rb_set_builtin_real(Q.of[bot].inst, SLOT_Y, ny); Q.of[bot].y = ny;
            }
        }
        rb_log("cpuqb: formation shows %s (back %s) for a %s play (run %.0f%%, deep %.0f%%)",
               show_run ? "RUN" : show_deep ? "DEEP" : "SHORT", show_run ? "behind QB" : "beside QB",
               Q.is_run ? "RUN" : Q.deep ? "DEEP" : "SHORT", p_run * 100, p_deep * 100);
        rb_defcall_reset();
        Q.called = 0;
        rb_log("cpuqb: new play, down %g yards %g (x_yards %g), los %.0f fwd %+.0f, arm %.4f -> k %.4f, %s%s",
               Q.down, Q.yards, rb_get_obj_real(OBJ_CONTROLLER, SLOT_X_YARDS), Q.los_x, Q.fwd, Q.mtp, Q.k,
               Q.is_run ? "RUN" : "PASS", Q.deep ? " (deep shot)" : "");
    }

    switch (Q.phase) {
    case PH_PRESNAP:
        finger(0, P0X, P0Y);
        if (st != 0 || Q.t < Q.t_phase) break;
        if (Q.down >= 4 && !Q.fourth_done && !g_fourth_answered) {   // 4th down: kick or go for it
            Q.fourth_done = 1;
            CInstance *btn = 0;
            RValue br = rb_obj_ref(OBJ_BTN_FIELDGOAL);
            rb_with(&br, ball, other, grab_first, &btn);
            if (btn) {
                char b[48];
                const char *txt = rb_rv_str(rb_inst_var(btn, SLOT_TXT), b, sizeof b);
                int punt = strstr(txt, "unt") || strstr(txt, "UNT");
                // The 4th-down question has already been answered by now (by the player, or the game moved
                // on). Getting here means "play 4th down" was chosen: honour it, never kick on our own.
                int go = 1;
                rb_log("cpuqb: 4th and %.1f, play 4th down was chosen -> GO FOR IT (button \"%s\")", Q.yards, txt);
                if (!go && punt) { rb_cpuqb_action = ACT_PUNT; Q.phase = PH_DONE; break; }
                if (!go) {
                    double bx = rb_get_builtin_real(btn, SLOT_X), by = rb_get_builtin_real(btn, SLOT_Y);
                    Q.btn_x = bx + rb_get_builtin_real(btn, SLOT_SPRITE_WIDTH) / 2;
                    Q.btn_y = by + rb_get_builtin_real(btn, SLOT_SPRITE_HEIGHT) / 2;
                    Q.phase = PH_KICK_TAP; Q.t_phase = Q.t;
                    break;
                }
            } else rb_log("cpuqb: 4th down but no kick button");
        }
        if (!Q.called) {                                       // the player calls the defense first
            if (!rb_defcall_ready((int)Q.down, Q.yards, Q.t)) break;
            if (Q.is_run && find_rb() < 0) { rb_log("cpuqb: no RB found, passing"); Q.is_run = 0; }
            { int r = find_rb(); if (r >= 0) rb_log("cpuqb: at the snap the back is %.0f off the QB", fabs(Q.of[r].y - Q.qby)); }
            rb_defcall_snap(Q.is_run, Q.deep);
            Q.called = 1;
        }
        if (Q.is_run) {
            int rb = find_rb();
            if (rb >= 0) {
                rb_vf.room_x = Q.of[rb].x; rb_vf.room_y = Q.of[rb].y;
                finger(1, P0X, P0Y);
                Q.phase = PH_HANDOFF; Q.t_phase = Q.t;
                rb_log("cpuqb: handoff tap at %.0f,%.0f", Q.of[rb].x, Q.of[rb].y);
                break;
            }
            rb_log("cpuqb: no RB found, passing");
            Q.is_run = 0;
        }
        finger(1, P0X, P0Y);                                   // snap: finger down
        Q.phase = PH_PULL; Q.t_phase = Q.t;
        break;

    case PH_HANDOFF:
        if (Q.t - Q.t_phase < 3) {                              // short tap on the RB
            int rb = find_rb();
            if (rb >= 0) { rb_vf.room_x = Q.of[rb].x; rb_vf.room_y = Q.of[rb].y; }
            finger(1, P0X, P0Y);
        } else {
            finger(0, P0X, P0Y);
            if (st != 0) { Q.phase = PH_DONE; Q.ran = 1; }
            else if (Q.t - Q.t_phase > 30) { Q.is_run = 0; Q.phase = PH_PRESNAP; Q.t_phase = Q.t + 10; }
        }
        break;

    case PH_PULL:                                               // drag back past the deadzone
        if (Q.scrambled) { rb_vf.room_x = Q.qbx; rb_vf.room_y = Q.qby; }
        finger(1, P0X - Q.fwd * 30, P0Y);
        if (st == 2) {
            Q.phase = PH_READ; Q.t_phase = Q.t;
            Q.read_frames = Q.scrambled ? 6 : 35 + arc4random_uniform(60);
        } else if (Q.t - Q.t_phase > 40) {                      // didn't take: lift and retry
            finger(0, P0X, P0Y); Q.phase = PH_PRESNAP; Q.t_phase = Q.t + 10;
        }
        break;

    case PH_READ: {
        finger(1, P0X - Q.fwd * 30, P0Y);
        if (st != 2) { Q.phase = PH_DONE; break; }
        unsigned held = Q.t - Q.t_phase;
        double pressure = nearest_def(Q.qbx, Q.qby, 0);
        double score, open;
        int r = pick_receiver(&score, &open);
        // escape lanes: the middle is always full of linemen, so look across the whole front
        double lane = -1;
        for (int k = -4; k <= 4; k++) {
            double ly = Q.qby + k * 30;
            if (ly < 150 || ly > 450) continue;
            double room = nearest_def(Q.qbx + Q.fwd * 70, ly, 10) - (k < 0 ? -k : k) * 5;
            if (room > lane) lane = room;
        }
        int can_scramble = !Q.scrambled && lane > 45;
        int go = 0, away = 0, run = 0;
        if (rb_defcall_blitz() && !Q.deep && held >= 14 && r >= 0 && open > SAFE_OPEN - 12) {
            go = 1;                                                             // blitz: hit the hot route
            rb_log("cpuqb: blitz read - hot route");
        }
        else if (held >= Q.read_frames && r >= 0 && open > SAFE_OPEN) go = 1;     // open man
        else if (!Q.scrambled && held > 15 && pressure < 80 && open < SAFE_OPEN && lane > 50 && frand() < 0.15)
            run = 1;                                                            // pocket breaking down: take off
        else if (pressure < 40 && held > 8) {                                   // about to be hit
            if (r >= 0 && open > SAFE_OPEN - 8) go = 1;                         // tight but catchable
            else if (can_scramble) run = 1;
            else away = 1;                                                      // a sack or throw-away beats a pick
        } else if (held >= Q.read_frames + 90) {                                // out of time
            if (r >= 0 && open > SAFE_OPEN - 5) go = 1;
            else if (can_scramble) run = 1;
            else away = 1;
        }
        if (run) {
            Q.phase = PH_SCRAMBLE_GO; Q.hold = 0; Q.scrambled = 1;
            rb_log("cpuqb: scramble (pressure %.0f, lane %.0f, best open %.0f)", pressure, lane, open);
        } else if (go) {
            Q.tgt = r; Q.phase = PH_AIM; Q.hold = 0; Q.th_open = open;
            rb_log("cpuqb: throw to #%d open %.0f pressure %.0f after %u frames", r, open, pressure, held);
        } else if (away) {
            Q.tgt = -1; Q.phase = PH_AIM; Q.hold = 0;
            rb_log("cpuqb: throwing it away (pressure %.0f, best open %.0f)", pressure, open);
            rb_brain_gave_up(0);
        }
        break;
    }

    case PH_AIM: {
        if (st != 2) { Q.phase = PH_DONE; finger(0, P0X, P0Y); break; }
        double lx, ly, v;
        if (Q.tgt >= 0 && Q.of[Q.tgt].seen) lead_point(Q.tgt, &lx, &ly, &v);
        else {                                                  // throw-away: hard to the near sideline
            lx = Q.qbx + Q.fwd * 110; ly = Q.qby + (Q.qby < 300 ? -320 : 320);
            v = speed_for_range(hypot(lx - Q.qbx, ly - Q.qby) * 1.25);
        }
        double dx = lx - Q.qbx, dy = ly - Q.qby, len = hypot(dx, dy);
        if (len < 1) len = 1;
        double drag = v / Q.k;
        if (drag < DRAG_MIN) drag = DRAG_MIN;
        if (drag > DRAG_MAX) drag = DRAG_MAX;
        // slingshot: the ball goes opposite the drag
        finger(1, P0X - dx / len * drag, P0Y - dy / len * drag);
        Q.aim_x = lx; Q.aim_y = ly; Q.drag = drag;
        if (++Q.hold >= 14) {                                   // let the game's drag lerp settle
            finger(0, rb_vf.gui_x, rb_vf.gui_y);
            Q.phase = PH_DONE;
            if (Q.tgt >= 0) {
                double hx, hy;
                Q.thrown = 1;
                Q.th_hdist = rb_def_human(&hx, &hy) ? hypot(hx - lx, hy - ly) : 999;
                Q.nd0 = 0; Q.t_rel = Q.t;
                for (int a = 0; a < Q.ndf && Q.nd0 < 16; a++)
                    if (Q.df[a].seen) { Q.d0[Q.nd0].inst = Q.df[a].inst; Q.d0[Q.nd0].x = Q.df[a].x; Q.d0[Q.nd0].y = Q.df[a].y; Q.nd0++; }
            }
            rb_log("cpuqb: release toward %.0f,%.0f drag %.0f (qb %.0f,%.0f)", lx, ly, drag, Q.qbx, Q.qby);
        }
        break;
    }

    case PH_KICK_TAP:                                           // tap the field goal button
        finger(Q.t - Q.t_phase < 3, Q.btn_x, Q.btn_y);
        if (Q.t - Q.t_phase >= 4) { Q.phase = PH_FG; Q.t_phase = Q.t; Q.fg_last_pow = -1; Q.fg_peak_frames = 0; g_fg_pending = 1; }
        break;

    case PH_FG: {                                               // experimental: hold, release at peak power
        unsigned t = Q.t - Q.t_phase;
        double pow_ = rb_get_inst_real(ball, SLOT_KICKPOW), dir = rb_get_inst_real(ball, SLOT_DIREC);
        if (t % 5 == 0)
            rb_log("cpuqb: fg t%u state %g kickpow %.2f direc %.1f flip %g", t, st, pow_, dir,
                   rb_get_inst_real(ball, SLOT_KICKFLIP));
        if (t < 20) { finger(0, P0X, P0Y); break; }            // let the kick set up
        if (pow_ <= Q.fg_last_pow + 1e-6 && Q.fg_last_pow > 0) Q.fg_peak_frames++; else Q.fg_peak_frames = 0;
        Q.fg_last_pow = pow_;
        int hold = t < 200 && Q.fg_peak_frames < 3;
        finger(hold, P0X, P0Y);
        if (!hold) { rb_log("cpuqb: fg release at kickpow %.2f direc %.1f", pow_, dir); Q.phase = PH_DONE; g_fg_pending = 0; }
        break;
    }

    case PH_SCRAMBLE_GO: {                                      // "throw it backwards" = QB takes off
        if (st != 2) { Q.phase = st == 5 ? PH_SCRAMBLE : PH_DONE; finger(0, P0X, P0Y); break; }
        finger(1, P0X + Q.fwd * 45, P0Y);                       // drag forward -> ball would go backward
        if (++Q.hold >= 10) { finger(0, rb_vf.gui_x, rb_vf.gui_y); Q.phase = PH_SCRAMBLE; Q.t_phase = Q.t; }
        break;
    }

    case PH_SCRAMBLE: {                                         // QB running: look for a throw on the run
        finger(0, P0X, P0Y);
        if (st != 5) { if (st == 1 || st == 2) { Q.phase = PH_PULL; Q.t_phase = Q.t; } else if (Q.t - Q.t_phase > 20) Q.phase = PH_DONE; break; }
        if (!Q.qb) break;
        double depth = (Q.qbx - Q.los_x) * Q.fwd;
        if (depth > -5) break;                                  // past the line: just run
        if (Q.t - Q.t_phase < 15) break;
        double score, open;
        int r = pick_receiver(&score, &open);
        if (r >= 0 && open > SAFE_OPEN + 15) {
            // touch the QB himself: the scramble mod puts him back into aiming
            rb_vf.room_x = Q.qbx; rb_vf.room_y = Q.qby;
            finger(1, P0X, P0Y);
            Q.phase = PH_PULL; Q.t_phase = Q.t;
            rb_log("cpuqb: pulling up to throw on the run (open %.0f)", open);
        }
        break;
    }

    default:
        if (!Q.sw_t) finger(0, rb_vf.gui_x, rb_vf.gui_y);
        if (st == 3 && !Q.measured && Q.drag > 0) {            // ball just left his hand: calibrate
            Q.measured = 1;
            double vel = rb_get_inst_real(ball, SLOT_VELOCITY), dd = rb_get_inst_real(ball, SLOT_DRAGDIST);
            if (vel > 0.5 && dd > 10) {
                double k = vel / dd;
                if (Q.mtp > 0) g_k_per_mtp = k / Q.mtp;
                rb_log("cpuqb: measured k %.4f (planned %.4f, drag %.1f/%.1f, arm %.4f)", k, Q.k, dd, Q.drag, Q.mtp);
                Q.k = k;
            }
        }
        break;
    }

    // ---- ball carrier jukes: a quick vertical flick, away from the defender closing in his path
    int carrying = st == 5 && Q.qb && (Q.phase == PH_DONE || Q.phase == PH_SCRAMBLE) && !rb_vf.down;
    if (Q.sw_t) {                                   // swipe in progress: 5 frames of drag, then lift
        Q.sw_t++;
        if (Q.sw_t <= 6) { rb_vf.room_x = Q.qbx; rb_vf.room_y = Q.qby + 100; finger(1, Q.sw_x, Q.sw_y + Q.sw_dir * 9.0 * (Q.sw_t - 1)); }
        else { finger(0, rb_vf.gui_x, rb_vf.gui_y); Q.sw_t = 0; Q.sw_cool = Q.t + 22; }
    } else if (carrying && Q.t >= Q.sw_cool) {
        double best = 1e9, threat_dy = 0;
        for (int i = 0; i < Q.ndf; i++) {
            if (!Q.df[i].seen) continue;
            double ahead = (Q.df[i].x - Q.qbx) * Q.fwd, dy = Q.df[i].y - Q.qby;
            if (ahead < -10 || ahead > 95 || fabs(dy) > 50) continue;
            double d = hypot(ahead, dy);
            if (d < best) { best = d; threat_dy = dy; }
        }
        // running lanes: look ahead for the y with the most room and cut toward it
        double lane_best = Q.qby, room_best = -1, room_here = -1;
        for (int k = -4; k <= 4; k++) {
            double ly = Q.qby + k * 30;
            if (ly < 150 || ly > 450) continue;
            double room = 999;
            for (int i = 0; i < Q.ndf; i++) {
                if (!Q.df[i].seen) continue;
                double ahead = (Q.df[i].x - Q.qbx) * Q.fwd;
                if (ahead < -15 || ahead > 170) continue;
                double d = hypot(ahead * 0.55, Q.df[i].y - ly);
                if (d < room) room = d;
            }
            room -= (k < 0 ? -k : k) * 4;                    // cutting costs a step
            if (k == 0) room_here = room;
            if (room > room_best) { room_best = room; lane_best = ly; }
        }
        if (best >= 80 && room_best > room_here + 22 && fabs(lane_best - Q.qby) >= 25) {
            best = 0; threat_dy = Q.qby - lane_best;    // reuse the juke: move away from "threat" = toward the lane
            if (Q.t % 30 == 0) rb_log("cpuqb: cutting toward a lane %.0f %s (room %.0f vs %.0f)",
                                     fabs(lane_best - Q.qby), lane_best > Q.qby ? "down" : "up", room_best, room_here);
        }
        if (best < 80) {
            double dir = threat_dy > 0 ? -1 : 1;    // screen y: +1 = down
            if (Q.qby < 200) dir = 1;               // don't juke into the sideline
            if (Q.qby > 430) dir = -1;
            Q.sw_t = 1; Q.sw_dir = dir; Q.sw_x = P0X; Q.sw_y = P0Y - dir * 20;
            rb_vf.room_x = Q.qbx; rb_vf.room_y = Q.qby + 100;
            finger(1, Q.sw_x, Q.sw_y);
            rb_log("cpuqb: juke %s (defender %.0f away)", dir > 0 ? "down" : "up", best);
        }
    }
}

void rb_cpuqb_info(double *los_x, double *fwd, double *qbx, double *qby) {
    *los_x = Q.los_x; *fwd = Q.fwd; *qbx = Q.qbx; *qby = Q.qby;
}

// where the CPU's last throw is headed (0 if none this play)
// the receiver the CPU threw to (current position)
int rb_cpuqb_receiver(double *x, double *y) {
    if (Q.tgt < 0 || Q.tgt >= Q.nof || !Q.of[Q.tgt].seen) return 0;
    *x = Q.of[Q.tgt].x; *y = Q.of[Q.tgt].y;
    return 1;
}

int rb_cpuqb_target(double *x, double *y) {
    if (Q.drag <= 0) return 0;
    *x = Q.aim_x; *y = Q.aim_y;
    return 1;
}

// the 4th-down question was closed by someone else (e.g. the player tapped it): drop our answer
void rb_cpuqb_fourth_closed(void) {
    if (rb_cpuqb_fourth_msg && g_fourth_wait == 0) return;     // the game clearing old messages as it opens this one
    if (rb_cpuqb_fourth_msg) {
        rb_log("cpuqb: 4th-down question answered by the player, keeping their choice");
        rb_cpuqb_fourth_msg = 0; g_fourth_wait = 0; g_fourth_answered = 1;
    }
}
