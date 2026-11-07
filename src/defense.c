// defense.c - the player controls one defender on CPU drives.
//  - at the snap: the defender nearest a linebacker spot (a little past the line, at the QB's y)
//  - drag anywhere = joystick: direction from where the finger went down, full speed at 35px
//  - quick tap = switch to the defender nearest the ball
//  - finger up = the game's AI moves him (so he never just stands there)
// Steering is applied after the game's own AI (s_get_destination) by overwriting jdir/jforce.
#include "rb.h"
#include "sys.h"
#include "input.h"
#include "defense.h"
#include "cpuqb.h"
#include "camera3d.h"

#define VA_S_DO_DIVE 0x10012b150ULL   // the game's dive move (used for the human ball carrier)
#define DIVE_RANGE   55.0
#define DIVE_FRAMES  24
#define BREAK_FRAMES 70

#define STICK_FULL   35.0
#define STICK_DEAD   6.0
#define TAP_FRAMES   12
#define TAP_MOVE     12.0

static struct {
    int active;               // CPU play live, we're on defense
    CInstance *ball;
    CInstance *ctrl;          // controlled defender
    int down; unsigned t_down; double ax, ay, gx, gy, moved;
    unsigned t;
    double jdir, jforce; int steering;
    double prev_st, bx, by, bvx, bvy;     // ball / carrier tracking
    unsigned dive_until, break_until; double dive_dir, tx, ty;
    int dove;                             // one dive per play: after that he's on the ground
    unsigned break_start; int jumped;
} D;

typedef struct { double x, y; CInstance *best; double bd; } NearCtx;
static int nearest_cb(CInstance *inst, void *vctx) {
    NearCtx *c = vctx;
    double d = hypot(rb_get_builtin_real(inst, SLOT_X) - c->x, rb_get_builtin_real(inst, SLOT_Y) - c->y);
    if (d < c->bd) { c->bd = d; c->best = inst; }
    return 0;
}
static CInstance *nearest_defender(CInstance *self, CInstance *other, double x, double y) {
    NearCtx c = { x, y, 0, 1e9 };
    RValue r = rb_obj_ref(OBJ_PLAYER_DF);
    rb_with(&r, self, other, nearest_cb, &c);
    return c.best;
}

static double dir_to(double fx, double fy, double tx, double ty) {
    double a = atan2(-(ty - fy), tx - fx) * 57.29577951308232;
    return a < 0 ? a + 360 : a;
}

static void tap_action(CInstance *ball, CInstance *other, double st) {
    double cx = rb_get_builtin_real(D.ctrl, SLOT_X), cy = rb_get_builtin_real(D.ctrl, SLOT_Y);
    if (D.ctrl && st == 5 && D.dove) return;                                     // already dove this play
    if (D.ctrl && st == 5 && hypot(D.bx - cx, D.by - cy) < DIVE_RANGE) {          // dive at the carrier
        double lx = D.bx + D.bvx * 10, ly = D.by + D.bvy * 10;                    // where he's going
        D.dive_dir = dir_to(cx, cy, lx, ly);
        D.dive_until = D.t + DIVE_FRAMES;
        D.dove = 1;
        rb_set_inst_real(D.ctrl, SLOT_JDIR, D.dive_dir);
        rb_set_inst_real(D.ctrl, SLOT_JFORCE, 1.0);
        RValue r = {0}; r.kind = RV_UNDEFINED;
        ((GMLScript)RB(VA_S_DO_DIVE))(D.ctrl, D.ctrl, &r, 0, 0);
        rb_log("defense: DIVE at carrier (%.0f away, heading %.0f)", hypot(D.bx - cx, D.by - cy), D.dive_dir);
        return;
    }
    if (D.ctrl && st == 3 && rb_cpuqb_receiver(&D.tx, &D.ty)) {                   // jump the route
        D.break_start = D.t + 10;                                                  // a defender's reaction time
        D.break_until = D.break_start + BREAK_FRAMES;
        D.jumped = 1;
        rb_log("defense: jumping the route toward the receiver");
        return;
    }
    CInstance *n = nearest_defender(ball, other, D.bx, D.by);                        // switch
    if (n) { D.ctrl = n; D.dive_until = D.break_until = 0; rb_log("defense: switched to %p", (void *)n); }
}

void rb_def_step(CInstance *ball, CInstance *other, int cpu_offense, double los_x, double fwd, double qbx, double qby) {
    double st = rb_get_inst_real(ball, SLOT_STATE);
    int live = cpu_offense && (st == 0 || st == 1 || st == 2 || st == 3 || st == 5 || st == 13 || st == 18 || st == 19);
    if (!live) { D.active = 0; D.ctrl = 0; D.ball = 0; D.steering = 0; return; }
    if (ball != D.ball) { memset(&D, 0, sizeof D); D.ball = ball; }
    D.active = 1;
    D.t++;

    if (!D.ctrl && fwd != 0 && qbx != 0)                   // linebacker spot
    {
        D.ctrl = nearest_defender(ball, other, los_x + fwd * 110, qby);
        if (D.ctrl) rb_log("defense: controlling %p", (void *)D.ctrl);
    }

    double bx = rb_get_builtin_real(ball, SLOT_X), by = rb_get_builtin_real(ball, SLOT_Y);
    if (D.t > 1) { D.bvx = D.bvx * 0.6 + (bx - D.bx) * 0.4; D.bvy = D.bvy * 0.6 + (by - D.by) * 0.4; }
    D.bx = bx; D.by = by;
    int down; double gx, gy;
    rb_real_touch(ball, &down, &gx, &gy);
    if (st != D.prev_st && !down) {                    // auto-switch to whoever is in on the play
        double tx, ty;
        if (st == 3 && rb_cpuqb_target(&tx, &ty)) {
            CInstance *n = nearest_defender(ball, other, tx, ty);
            if (n && n != D.ctrl) { D.ctrl = n; rb_log("defense: auto-switch to the target area"); }
        } else if (st == 5 && D.ctrl) {
            double cx = rb_get_builtin_real(D.ctrl, SLOT_X), cy = rb_get_builtin_real(D.ctrl, SLOT_Y);
            if (hypot(bx - cx, by - cy) > 120) {
                CInstance *n = nearest_defender(ball, other, bx, by);
                if (n && n != D.ctrl) { D.ctrl = n; rb_log("defense: auto-switch to the ball carrier"); }
            }
        }
    }
    D.prev_st = st;
    if (down && !D.down) { D.t_down = D.t; D.ax = gx; D.ay = gy; D.moved = 0; }
    if (down) {
        double m = hypot(gx - D.ax, gy - D.ay);
        if (m > D.moved) D.moved = m;
        double dx = gx - D.ax, dy = gy - D.ay;
        if (rb_cam3d_active()) { double rx, ry; rb_cam3d_rotate_drag(dx, dy, &rx, &ry); dx = rx; dy = ry; }
        if (m > STICK_DEAD) {
            double a = atan2(-dy, dx) * 57.29577951308232;
            if (a < 0) a += 360;
            D.jdir = a;
            D.jforce = m >= STICK_FULL ? 1.0 : m / STICK_FULL;
            if (!D.steering || D.t % 30 == 0)
                rb_log("defense: steer dir %.0f force %.2f (player at %.0f,%.0f)", D.jdir, D.jforce,
                       rb_get_builtin_real(D.ctrl, SLOT_X), rb_get_builtin_real(D.ctrl, SLOT_Y));
            D.steering = 1;
        } else D.steering = 0;
    } else {
        if (D.down && D.t - D.t_down <= TAP_FRAMES && D.moved < TAP_MOVE)      // tap -> dive / break / switch
            tap_action(ball, other, st);
        D.steering = 0;
    }
    D.down = down;
}

// after the AI has picked a heading for a player
void rb_def_after_ai(CInstance *player) {
    if (!D.active || player != D.ctrl) return;
    if (D.t < D.dive_until) {                                   // keep the lunge going where he aimed it
        rb_set_inst_real(player, SLOT_JDIR, D.dive_dir);
        rb_set_inst_real(player, SLOT_JFORCE, 1.0);
        return;
    }
    if (D.steering) {
        rb_set_inst_real(player, SLOT_JDIR, D.jdir);
        rb_set_inst_real(player, SLOT_JFORCE, D.jforce);
        D.break_until = 0;
        return;
    }
    if (D.t >= D.break_start && D.t < D.break_until) {         // chase the receiver (not the ball's landing spot)
        rb_cpuqb_receiver(&D.tx, &D.ty);
        double x = rb_get_builtin_real(player, SLOT_X), y = rb_get_builtin_real(player, SLOT_Y);
        if (hypot(D.tx - x, D.ty - y) > 6) {
            rb_set_inst_real(player, SLOT_JDIR, dir_to(x, y, D.tx, D.ty));
            rb_set_inst_real(player, SLOT_JFORCE, 1.0);
        }
    }
}

// marker on the controlled defender: ring at his feet + dot over his head (drawn after him, on top)
static void *f_circle;
void rb_def_draw(CInstance *player) {
    if (!D.active || player != D.ctrl) return;
    if (!f_circle) f_circle = rb_builtin("draw_circle_colour");
    if (!f_circle) return;
    double x = rb_get_builtin_real(player, SLOT_X), y = rb_get_builtin_real(player, SLOT_Y);
    double col = 65535;    // c_yellow (BGR)
    for (int r = 13; r <= 15; r++) {
        double a[6] = { x, y, r, col, col, 1 };
        rb_call(f_circle, player, 6, a);
    }
    double dot[6] = { x, y - 38, 5, col, col, 0 };
    rb_call(f_circle, player, 6, dot);
}

int rb_def_human(double *x, double *y) {
    if (!D.active || !D.ctrl) return 0;
    *x = rb_get_builtin_real(D.ctrl, SLOT_X); *y = rb_get_builtin_real(D.ctrl, SLOT_Y);
    return *x > -9000;
}
int rb_def_jumped(void) { return D.jumped; }

CInstance *rb_def_controlled(void) { return D.active ? D.ctrl : 0; }
