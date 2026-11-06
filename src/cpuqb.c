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

#define THROW_K      0.1159     // fallback ball speed per pixel of drag (a strong arm)
#define DRAG_DECAY   0.986
#define AIR_K        8.75       // frames of flight per unit of speed
#define DRAG_MIN     24.0
#define DRAG_MAX     140.0
#define P0X          400.0      // where the finger goes down (GUI)
#define P0Y          190.0

enum { PH_IDLE, PH_PRESNAP, PH_HANDOFF, PH_PULL, PH_READ, PH_AIM, PH_SCRAMBLE_GO, PH_SCRAMBLE, PH_KICK_TAP, PH_FG, PH_DONE };

typedef struct { CInstance *inst; double x, y, vx, vy; int seen; } Track;

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
} Q;

int rb_cpuqb_punt_pending;     // hooks.c performs the punt after the ball update returns
static int g_fg_pending;       // CPU tapped the field goal button; survives the kick set-up

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
#define DEF_CLOSE  0.55
#define SAFE_OPEN  22.0       // clearance needed to call a man open
#define LANE_MIN   14.0       // clearance needed along the catchable part of the flight

static double clearance_at(double bx, double by, double t) {
    double best = 1e9;
    for (int i = 0; i < Q.ndf; i++) {
        if (!Q.df[i].seen) continue;
        double px = Q.df[i].x + Q.df[i].vx * t * 0.4, py = Q.df[i].y + Q.df[i].vy * t * 0.4;
        double d = hypot(px - bx, py - by) - DEF_CLOSE * t;
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
        double open = at_catch < lane + (SAFE_OPEN - LANE_MIN) ? at_catch : lane + (SAFE_OPEN - LANE_MIN);
        double gain = (lx - Q.los_x) * Q.fwd;
        double score = open + gain * (Q.deep ? 0.30 : 0.12) - (open < SAFE_OPEN ? 80 : 0) + frand() * 10;
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

void rb_cpuqb_step(CInstance *ball, CInstance *other, int cpu_offense) {
    rb_vf_frame();
    double st = rb_get_inst_real(ball, SLOT_STATE);
    if (!cpu_offense) { rb_vf.active = 0; Q.phase = PH_IDLE; Q.ball = 0; g_fg_pending = 0; return; }
    if (ball != Q.ball) new_play(ball);

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
            Q.k = Q.mtp * 1.2;                                  // velocity = drag * my_throw_power (x or / 1.2)
            if (Q.k > 0.2 || Q.k < 0.04) Q.k = Q.mtp / 1.2;
        }
        if (!(Q.k > 0.03 && Q.k < 0.3)) Q.k = THROW_K;
        double sx = 0; int n = 0;
        for (int i = 0; i < Q.ndf; i++) if (Q.df[i].seen) { sx += Q.df[i].x; n++; }
        if (!n) return;
        Q.fwd = (sx / n < Q.qbx) ? -1 : 1;                     // defense lines up in front
        Q.los_x = Q.qbx + Q.fwd * 35;
        Q.t_phase = 40 + arc4random_uniform(40);              // snap after this many frames
        Q.down = rb_get_obj_real(OBJ_CONTROLLER, SLOT_DOWN);
        Q.yards = rb_get_obj_real(OBJ_CONTROLLER, SLOT_YARDS);
        double p_run = 0.42;
        if (Q.yards > 0 && Q.yards <= 2) p_run = 0.65;
        else if (Q.yards >= 8) p_run = 0.22;
        if (Q.down >= 3 && Q.yards >= 4) p_run = 0.10;
        Q.is_run = frand() < p_run;
        Q.deep = !Q.is_run && frand() < 0.18;
        rb_log("cpuqb: new play, down %g yards %g (x_yards %g), los %.0f fwd %+.0f, arm %.4f -> k %.4f, %s%s",
               Q.down, Q.yards, rb_get_obj_real(OBJ_CONTROLLER, SLOT_X_YARDS), Q.los_x, Q.fwd, Q.mtp, Q.k,
               Q.is_run ? "RUN" : "PASS", Q.deep ? " (deep shot)" : "");
    }

    switch (Q.phase) {
    case PH_PRESNAP:
        finger(0, P0X, P0Y);
        if (st != 0 || Q.t < Q.t_phase) break;
        if (Q.down >= 4 && !Q.fourth_done) {                    // 4th down: kick or go for it
            Q.fourth_done = 1;
            CInstance *btn = 0;
            RValue br = rb_obj_ref(OBJ_BTN_FIELDGOAL);
            rb_with(&br, ball, other, grab_first, &btn);
            if (btn) {
                char b[48];
                const char *txt = rb_rv_str(rb_inst_var(btn, SLOT_TXT), b, sizeof b);
                int punt = strstr(txt, "unt") || strstr(txt, "UNT");
                int go = Q.yards > 0 && Q.yards <= 1.2 && frand() < (punt ? 0.25 : 0.35);
                rb_log("cpuqb: 4th and %g, button \"%s\" -> %s", Q.yards, txt, go ? "GO FOR IT" : punt ? "PUNT" : "FIELD GOAL");
                if (!go && punt) { rb_cpuqb_punt_pending = 1; Q.phase = PH_DONE; break; }
                if (!go) {
                    double bx = rb_get_builtin_real(btn, SLOT_X), by = rb_get_builtin_real(btn, SLOT_Y);
                    Q.btn_x = bx + rb_get_builtin_real(btn, SLOT_SPRITE_WIDTH) / 2;
                    Q.btn_y = by + rb_get_builtin_real(btn, SLOT_SPRITE_HEIGHT) / 2;
                    Q.phase = PH_KICK_TAP; Q.t_phase = Q.t;
                    break;
                }
            } else rb_log("cpuqb: 4th down but no kick button");
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
            if (st != 0) Q.phase = PH_DONE;
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
        int go = 0;
        if (!Q.scrambled && held > 20 && pressure < 70 && open < SAFE_OPEN) {       // pocket collapsing, nobody open
            double lane = nearest_def(Q.qbx + Q.fwd * 80, Q.qby, 10);
            if (lane > 55 && frand() < 0.08) {                              // checked every frame -> ~quickly
                Q.phase = PH_SCRAMBLE_GO; Q.hold = 0; Q.scrambled = 1;
                rb_log("cpuqb: scramble (pressure %.0f, lane %.0f)", pressure, lane);
                break;
            }
        }
        if (pressure < 45 && held > 8) go = 1;                              // about to be hit
        else if (held >= Q.read_frames && r >= 0 && open > SAFE_OPEN) go = 1;   // open man
        else if (held >= Q.read_frames + 70) go = 1;                        // out of time
        if (go && r >= 0 && open < SAFE_OPEN * 0.4) r = -1;                 // nothing safe: throw it away
        if (go && r >= 0) {
            Q.tgt = r; Q.phase = PH_AIM; Q.hold = 0;
            rb_log("cpuqb: throw to #%d open %.0f pressure %.0f after %u frames", r, open, pressure, held);
        } else if (go) {
            Q.tgt = -1; Q.phase = PH_AIM; Q.hold = 0;            // nobody: throw it away downfield
            rb_log("cpuqb: nobody open, throwing away (pressure %.0f)", pressure);
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
        finger(0, rb_vf.gui_x, rb_vf.gui_y);
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
}

void rb_cpuqb_info(double *los_x, double *fwd, double *qbx, double *qby) {
    *los_x = Q.los_x; *fwd = Q.fwd; *qbx = Q.qbx; *qby = Q.qby;
}
