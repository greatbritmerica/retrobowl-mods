// offtools.c - tools for the player's offense: play action, hot routes, pump fake.
//
// Before the snap a small panel on the left edge offers:
//   Play action (toggle) - on a pass play the back fakes the handoff and the second level (linebackers,
//                          safeties) steps up toward the line for about half a second. How hard they bite
//                          depends on how much the player has been running (the CPU defense brain's run
//                          share): no run game, no bite.
//   Hot route            - pick a receiver, then a new route (streak, post, slant, out, curl, flat). Uses the
//                          game's own route setter (s_set_my_route), so it's drawn and run like any route.
// While aiming, a PUMP button on the right edge (second finger): defenders near where the ball is aimed
// break toward that spot for about a quarter second, as if it had been thrown. Two pumps per play.
#include "rb.h"
#include "sys.h"
#include "ui.h"
#include "dbrain.h"
#include "offtools.h"
#include "input.h"

#define SLOT_MY_ROUTE   0x100fa6ce8ULL
#define VA_SET_MY_ROUTE 0x100513734ULL     // gml_Script_s_set_my_route(route_name), run as the receiver
#define VA_RV_FROM_CSTR 0x100bd6f74ULL     // YYRValue(const char*): how the game builds its string constants
#define DRAG_DECAY      0.986
#define AIR_K           8.75
#define RUSH_DEPTH      45.0
#define PA_FRAMES       42
#define PUMP_FRAMES     16
#define MAX_PUMPS       2

// s_set_my_route switches on the route's NAME (its case labels are the strings "route_streak", ...;
// read from the game's static initializers). Same names as the path assets.
static const struct { const char *name; const char *route; } ROUTES[] = {
    { "Streak", "route_streak" }, { "Post", "route_post" }, { "Slant", "route_slant" },
    { "Out", "route_out2" }, { "Curl", "route_curl" }, { "Flat", "route_flat" },
};
#define NROUTES 6

static struct {
    CInstance *ball;
    double st, prev_st;
    unsigned t, t_snap;
    double fwd, los, qbx, qby;
    CInstance *qb, *rb;
    int pa_on, pa_live, run_play;
    double bite;
    int pumps; unsigned pump_until; double plx, ply;
    int presnap_shown, pump_shown, offense_snap, scrambled;
    CInstance *rcv[6]; double rcv_y[6]; int nrcv, pick;
    char note[96];
    CInstance *df[16]; double dfx[16]; int ndf, rusher[16];
} O;

// UI callbacks arrive on the main thread between frames; the step acts on them
static volatile int g_ev_presnap, g_ev_rcv, g_ev_route, g_ev_pump;
static char g_last_pa[96];                                        // shown before the next snap

static void cb_presnap(int i) { g_ev_presnap = i ? i : -1; }
static void cb_rcv(int i)     { g_ev_rcv = i ? i : -1; }
static void cb_route(int i)   { g_ev_route = i ? i : -1; }
static void cb_pump(int i)    { g_ev_pump = i ? 1 : -1; }

static void show_presnap(void) {
    const char *b[2] = { O.pa_on ? "Play action: ON" : "Play action: off", "Hot route" };
    char msg[200];
    snprintf(msg, sizeof msg, "%s%s%s", O.note, O.note[0] && g_last_pa[0] ? "\n" : "", g_last_pa);
    rb_ui_panel(RB_PANEL_PRESNAP, RB_SIDE_LEFT, 150, "", msg, b, 2, cb_presnap);
    O.presnap_shown = 1;
}

// ---------------------------------------------------------------- players
typedef struct { double x, y; CInstance *best; double bd; } NearCtx;
static int near_cb(CInstance *inst, void *vc) {
    NearCtx *c = vc;
    double d = hypot(rb_get_builtin_real(inst, SLOT_X) - c->x, rb_get_builtin_real(inst, SLOT_Y) - c->y);
    if (d < c->bd) { c->bd = d; c->best = inst; }
    return 0;
}
typedef struct { double sx; int n; } MeanCtx;
static int qb_cb(CInstance *inst, void *ctx) { *(CInstance **)ctx = inst; return 1; }
static int mean_cb(CInstance *inst, void *vc) { MeanCtx *m = vc; m->sx += rb_get_builtin_real(inst, SLOT_X); m->n++; return 0; }

static int df_cb(CInstance *inst, void *vc) {
    (void)vc;
    if (O.ndf >= 16) return 1;
    O.df[O.ndf] = inst; O.dfx[O.ndf] = rb_get_builtin_real(inst, SLOT_X); O.rusher[O.ndf] = 0;
    O.ndf++;
    return 0;
}

// route runners: offensive players with a route, not the QB
typedef struct { CInstance *qb; } RcvCtx;
static int rcv_cb(CInstance *inst, void *vc) {
    RcvCtx *c = vc;
    if (inst == c->qb || O.nrcv >= 6) return 0;
    RValue *rv = rb_inst_var(inst, SLOT_MY_ROUTE);
    if (!rv || (rv->kind & 0xffffff) != RV_REAL || rv->real < 0) return 0;
    double y = rb_get_builtin_real(inst, SLOT_Y), x = rb_get_builtin_real(inst, SLOT_X);
    if (fabs(x - O.los) < 30 && fabs(y - O.qby) < 55) return 0;      // interior linemen
    int k = O.nrcv++;
    while (k > 0 && O.rcv_y[k - 1] > y) { O.rcv[k] = O.rcv[k - 1]; O.rcv_y[k] = O.rcv_y[k - 1]; k--; }   // top first
    O.rcv[k] = inst; O.rcv_y[k] = y;
    return 0;
}

static void open_receivers(CInstance *ball, CInstance *other) {
    O.nrcv = 0;
    RcvCtx c = { O.qb };
    RValue r = rb_obj_ref(OBJ_PLAYER_OF);
    rb_with(&r, ball, other, rcv_cb, &c);
    if (!O.nrcv) { rb_log("offtools: no route runners found"); show_presnap(); return; }
    static char lab[7][40]; const char *b[7];
    for (int i = 0; i < O.nrcv; i++) {
        if (O.rcv[i] == O.rb) snprintf(lab[i], sizeof lab[i], "Running back");
        else if (i == 0) snprintf(lab[i], sizeof lab[i], "Top receiver");
        else if (i == O.nrcv - 1 || (i == O.nrcv - 2 && O.rcv[O.nrcv - 1] == O.rb)) snprintf(lab[i], sizeof lab[i], "Bottom receiver");
        else snprintf(lab[i], sizeof lab[i], "Receiver %d from top", i + 1);
        b[i] = lab[i];
    }
    snprintf(lab[O.nrcv], sizeof lab[O.nrcv], "Cancel"); b[O.nrcv] = lab[O.nrcv];
    rb_ui_panel(RB_PANEL_PRESNAP, 0, 176, "Hot route: who?", "", b, O.nrcv + 1, cb_rcv);
}

static void open_routes(void) {
    const char *b[NROUTES + 1];
    for (int i = 0; i < NROUTES; i++) b[i] = ROUTES[i].name;
    b[NROUTES] = "Cancel";
    rb_ui_panel(RB_PANEL_PRESNAP, 0, 150, "New route", "", b, NROUTES + 1, cb_route);
}

static void apply_route(CInstance *rcv, const char *route, const char *name, const char *who) {
    RValue arg = {0};
    ((void (*)(RValue *, const char *))RB(VA_RV_FROM_CSTR))(&arg, route);
    RValue *av[1] = { &arg };
    RValue res = {0}; res.kind = RV_UNDEFINED;
    double before = rb_get_inst_real(rcv, SLOT_MY_ROUTE);
    ((GMLScript)RB(VA_SET_MY_ROUTE))(rcv, rcv, &res, 1, av);
    double after = rb_get_inst_real(rcv, SLOT_MY_ROUTE);
    ((void (*)(RValue *))RB(VA_RV_FREE))(&arg);
    ((void (*)(RValue *))RB(VA_RV_FREE))(&res);
    int ok = after != before && after >= 0;
    rb_log("offtools: hot route %s -> %s (%s, path %g -> %g)%s", who, name, route, before, after, ok ? "" : "  NOT APPLIED");
    if (ok) snprintf(O.note, sizeof O.note, "Hot: %s - %s", who, name);
    else snprintf(O.note, sizeof O.note, "Hot route didn't take");
}

// ---------------------------------------------------------------- per frame (player on offense)
void rb_off_step(CInstance *ball, CInstance *other, int human_offense) {
    O.t++;
    if (!human_offense) {
        if (O.presnap_shown) { rb_ui_panel_close(RB_PANEL_PRESNAP); O.presnap_shown = 0; }
        if (O.pump_shown) { rb_ui_panel_close(RB_PANEL_PUMP); O.pump_shown = 0; }
        O.ball = 0;
        return;
    }
    double st = rb_get_inst_real(ball, SLOT_STATE);
    if (ball != O.ball) {                                         // new play
        memset(&O, 0, sizeof O);
        O.ball = ball; O.prev_st = O.st = st;
        g_ev_presnap = g_ev_rcv = g_ev_route = g_ev_pump = 0;
    }
    O.prev_st = O.st; O.st = st;

    if (O.prev_st == 5 && (st == 1 || st == 2) && !O.scrambled) { // QB ran (carrier) then pulled up to aim again
        rb_log("offtools: scramble pull-up (run -> aim) - no windup sack on this throw");
        O.scrambled = 1;
    }
    if (st == 0 && O.t_snap) {                                    // back to 0 mid-play: scramble, pulled up to pass
        if (!O.scrambled) rb_log("offtools: scramble pull-up - no windup sack on this throw");
        O.scrambled = 1;
        return;
    }
    if (st == 0) {
        if (!O.fwd) {                                             // orient: QB, back, line
            CInstance *qb = 0;                                    // the ball's holder (as the rest of the mod finds him)
            RValue *h = rb_inst_var(ball, SLOT_HOLDER);
            if (h && h->kind != RV_UNDEFINED) { RValue c = *h; rb_with(&c, ball, other, qb_cb, &qb); }
            if (!qb) return;
            O.qb = qb;
            O.qbx = rb_get_builtin_real(O.qb, SLOT_X); O.qby = rb_get_builtin_real(O.qb, SLOT_Y);
            MeanCtx m = { 0, 0 };
            RValue d = rb_obj_ref(OBJ_PLAYER_DF);
            rb_with(&d, ball, other, mean_cb, &m);
            if (!m.n) return;
            O.fwd = (m.sx / m.n < O.qbx) ? -1 : 1;
            O.los = O.qbx + O.fwd * 35;
            NearCtx b = { O.qbx - O.fwd * 40, O.qby, 0, 1e9 };    // the back: right behind/beside the QB
            RValue r2 = rb_obj_ref(OBJ_PLAYER_OF);
            rb_with(&r2, ball, other, near_cb, &b);
            O.rb = (b.best && b.best != O.qb && b.bd < 70) ? b.best : 0;
            O.nrcv = 0;                                           // a real offensive snap has route runners
            RcvCtx rc = { O.qb };
            RValue r3 = rb_obj_ref(OBJ_PLAYER_OF);
            rb_with(&r3, ball, other, rcv_cb, &rc);
            O.offense_snap = O.nrcv >= 2;
            rb_log("offtools: your snap - %d route runners%s", O.nrcv, O.offense_snap ? "" : " (not a scrimmage play: no tools)");
        }
        if (!O.offense_snap) return;
        if (!O.presnap_shown) show_presnap();
        // menu events
        if (g_ev_presnap) {
            int e = g_ev_presnap; g_ev_presnap = 0;
            if (e == 1) { O.pa_on = !O.pa_on; rb_log("offtools: play action %s", O.pa_on ? "on" : "off"); show_presnap(); }
            else if (e == 2) open_receivers(ball, other);
            else show_presnap();
        }
        if (g_ev_rcv) {
            int e = g_ev_rcv; g_ev_rcv = 0;
            if (e >= 1 && e <= O.nrcv) { O.pick = e - 1; open_routes(); }
            else show_presnap();
        }
        if (g_ev_route) {
            int e = g_ev_route; g_ev_route = 0;
            if (e >= 1 && e <= NROUTES && O.pick < O.nrcv && O.rcv[O.pick]) {
                char who[40];
                if (O.rcv[O.pick] == O.rb) snprintf(who, sizeof who, "RB");
                else if (O.pick == 0) snprintf(who, sizeof who, "top WR");
                else snprintf(who, sizeof who, "receiver %d", O.pick + 1);
                apply_route(O.rcv[O.pick], ROUTES[e - 1].route, ROUTES[e - 1].name, who);
            }
            show_presnap();
        }
        return;
    }

    if (st == 1) return;                                          // touched, but still in the aim deadzone (or a tap on our panel)
    // ---- after the snap
    if (O.presnap_shown) { rb_ui_panel_close(RB_PANEL_PRESNAP); O.presnap_shown = 0; }
    if (!O.offense_snap) return;
    if (!O.t_snap) {
        O.t_snap = O.t;
        O.run_play = st == 19;
        O.ndf = 0;
        RValue d = rb_obj_ref(OBJ_PLAYER_DF);
        rb_with(&d, ball, other, df_cb, 0);
        for (int i = 0; i < O.ndf; i++) O.rusher[i] = fabs(O.dfx[i] - O.los) < RUSH_DEPTH;
        if (O.pa_on && !O.run_play) {
            double rs = rb_dbrain_run_share(), f = (rs - 0.15) / 0.30;   // ~30% runs already sells it well
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            O.bite = 0.25 + 0.75 * f;
            O.pa_live = 1;
            rb_log("offtools: play action - your run share %.0f%%, the defense bites %.0f%%", rs * 100, O.bite * 100);
            snprintf(g_last_pa, sizeof g_last_pa, "Last play action: they bit %s (%.0f%%)",
                     O.bite > 0.7 ? "hard" : O.bite > 0.45 ? "some" : "barely - run more to sell it", O.bite * 100);
        }
    }
    if (O.pa_live && O.t - O.t_snap >= PA_FRAMES) O.pa_live = 0;
    if (st == 3 || st == 5 || st == 19) O.pa_live = 0;              // ball's out or someone has it

    // pump fake while aiming
    if (st == 2 && O.pumps < MAX_PUMPS) {
        if (!O.pump_shown) {
            static const char *b[1] = { "PUMP" };
            rb_ui_panel(RB_PANEL_PUMP, RB_SIDE_LEFT_BOTTOM, 124, "", "", b, 1, cb_pump);
            O.pump_shown = 1;
        }
        // or a second finger anywhere on the screen
        static void *f_pressed;
        if (!f_pressed) f_pressed = rb_builtin("device_mouse_check_button_pressed");
        double a[2] = { 1, 1 };                                        // device 1 (second finger), mb_left
        if (f_pressed && rb_call(f_pressed, ball, 2, a) > 0.5 && !g_ev_pump) { g_ev_pump = 1; rb_log("offtools: second-finger tap"); }
    } else if (O.pump_shown) { rb_ui_panel_close(RB_PANEL_PUMP); O.pump_shown = 0; }
    if (g_ev_pump) {
        int e = g_ev_pump; g_ev_pump = 0;
        if (O.pump_shown) { rb_ui_panel_close(RB_PANEL_PUMP); O.pump_shown = 0; }
        if (e > 0 && st == 2 && O.t >= O.pump_until + 8) {          // (button and finger can both fire for one tap)
            double qx = rb_get_builtin_real(ball, SLOT_X), qy = rb_get_builtin_real(ball, SLOT_Y);
            double dir = rb_get_inst_real(ball, SLOT_DIREC) / 57.29577951308232;   // aim, screen degrees
            double drag = rb_get_inst_real(ball, SLOT_DRAGDIST);
            double k = O.qb ? rb_get_inst_real(O.qb, SLOT_MY_THROW_POWER) : 0.1;
            double v = drag * (k > 0.01 ? k : 0.1);
            double R = v * (1.0 - pow(DRAG_DECAY, AIR_K * v)) / (1.0 - DRAG_DECAY);
            if (R < 60) R = 60;
            O.plx = qx + cos(dir) * R; O.ply = qy + sin(dir) * R;
            O.pump_until = O.t + PUMP_FRAMES; O.pumps++;
            rb_log("offtools: pump fake %d toward %.0f,%.0f (aim %.0f deg, %.0f px)", O.pumps, O.plx, O.ply, dir * 57.3, R);
        }
    }
}

static double dir_to(double fx, double fy, double tx, double ty) {
    double a = atan2(-(ty - fy), tx - fx) * 57.29577951308232;
    return a < 0 ? a + 360 : a;
}
static void steer(CInstance *p, double x, double y, double tx, double ty, double force) {
    rb_set_inst_real(p, SLOT_JDIR, dir_to(x, y, tx, ty));
    rb_set_inst_real(p, SLOT_JFORCE, force);
}

// after the game's AI moves a player (both teams)
void rb_off_after_ai(CInstance *player) {
    if (!O.ball || !O.t_snap) return;
    double x = rb_get_builtin_real(player, SLOT_X), y = rb_get_builtin_real(player, SLOT_Y);
    if (x < -9000) return;
    unsigned since = O.t - O.t_snap;
    if (O.pa_live && player == O.rb && since < 22) {                  // the fake handoff: across the QB
        steer(player, x, y, O.qbx + O.fwd * 25, O.qby, 1.0);
        return;
    }
    int i = 0;
    for (; i < O.ndf; i++) if (O.df[i] == player) break;
    if (i == O.ndf || O.rusher[i]) return;
    if (O.t < O.pump_until && O.st == 2) {                            // bit on the pump
        if (hypot(O.plx - x, O.ply - y) < 150) steer(player, x, y, O.plx, O.ply, 1.0);
        return;
    }
    if (O.pa_live && since < PA_FRAMES) steer(player, x, y, O.los + O.fwd * 15, y, O.bite);   // step up for the run
}

double rb_off_pa_bite(void) { return O.ball ? O.bite : 0; }
int rb_off_scrambled(void) { return O.ball && O.scrambled; }
