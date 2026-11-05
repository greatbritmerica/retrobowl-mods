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

#define THROW_K      0.1159     // ball speed per pixel of drag
#define DRAG_DECAY   0.986
#define AIR_K        8.75       // frames of flight per unit of speed
#define DRAG_MIN     24.0
#define DRAG_MAX     115.0
#define P0X          400.0      // where the finger goes down (GUI)
#define P0Y          190.0

enum { PH_IDLE, PH_PRESNAP, PH_HANDOFF, PH_PULL, PH_READ, PH_AIM, PH_DONE };

typedef struct { CInstance *inst; double x, y, vx, vy; int seen; } Track;

static struct {
    CInstance *ball;
    int phase, is_run;
    unsigned t, t_phase, read_frames, hold;
    double los_x, fwd;            // line of scrimmage x, +1/-1 = offense's forward x direction
    CInstance *qb;
    double qbx, qby;
    Track of[16], df[16];
    int nof, ndf;
    int tgt;                       // index into of[]
    double aim_x, aim_y, drag;
} Q;

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

// best receiver right now; returns index or -1. *open_out = separation at the catch point
static int pick_receiver(double *score_out, double *open_out) {
    int best = -1;
    double best_score = -1e9, best_open = 0;
    for (int i = 0; i < Q.nof; i++) {
        Track *r = &Q.of[i];
        if (!r->seen || r->inst == Q.qb) continue;
        double depth = (r->x - Q.los_x) * Q.fwd;
        double spd = hypot(r->vx, r->vy);
        if (depth < 25 && spd < 1.6) continue;              // linemen: at the line and slow
        double lx, ly, v;
        lead_point(i, &lx, &ly, &v);
        if (v / THROW_K > DRAG_MAX) continue;               // out of range
        double T = AIR_K * v;
        double open = nearest_def(lx, ly, T * 0.7);
        double gain = (lx - Q.los_x) * Q.fwd;
        double score = open + gain * 0.12 - (open < 30 ? 60 : 0) + frand() * 12;
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
}

static void finger(int down, double gx, double gy) {
    rb_vf.down = down; rb_vf.gui_x = gx; rb_vf.gui_y = gy;
}

void rb_cpuqb_step(CInstance *ball, CInstance *other, int cpu_offense) {
    rb_vf_frame();
    double st = rb_get_inst_real(ball, SLOT_STATE);
    if (!cpu_offense) { rb_vf.active = 0; Q.phase = PH_IDLE; Q.ball = 0; return; }
    if (ball != Q.ball) new_play(ball);

    // virtual finger owns the input while the CPU's play is live
    int live = (st == 0 || st == 1 || st == 2 || st == 3 || st == 5 || st == 13 || st == 18 || st == 19);
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
        double sx = 0; int n = 0;
        for (int i = 0; i < Q.ndf; i++) if (Q.df[i].seen) { sx += Q.df[i].x; n++; }
        if (!n) return;
        Q.fwd = (sx / n < Q.qbx) ? -1 : 1;                     // defense lines up in front
        Q.los_x = Q.qbx + Q.fwd * 35;
        Q.t_phase = 40 + arc4random_uniform(40);              // snap after this many frames
        Q.is_run = frand() < 0.35;
        rb_log("cpuqb: new play, los %.0f fwd %+.0f, %s", Q.los_x, Q.fwd, Q.is_run ? "RUN" : "PASS");
    }

    switch (Q.phase) {
    case PH_PRESNAP:
        finger(0, P0X, P0Y);
        if (st != 0 || Q.t < Q.t_phase) break;
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
        finger(1, P0X - Q.fwd * 30, P0Y);
        if (st == 2) {
            Q.phase = PH_READ; Q.t_phase = Q.t;
            Q.read_frames = 35 + arc4random_uniform(60);
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
        if (pressure < 45 && held > 8) go = 1;                              // about to be hit
        else if (held >= Q.read_frames && r >= 0 && open > 40) go = 1;      // open man
        else if (held >= Q.read_frames + 70) go = 1;                        // out of time
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
        else { lx = Q.qbx + Q.fwd * 420; ly = Q.qby + (Q.qby < 300 ? -260 : 260); v = 9; }   // sideline, deep
        double dx = lx - Q.qbx, dy = ly - Q.qby, len = hypot(dx, dy);
        if (len < 1) len = 1;
        double drag = v / THROW_K;
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

    default:
        finger(0, rb_vf.gui_x, rb_vf.gui_y);
        break;
    }
}

void rb_cpuqb_info(double *los_x, double *fwd, double *qbx, double *qby) {
    *los_x = Q.los_x; *fwd = Q.fwd; *qbx = Q.qbx; *qby = Q.qby;
}
