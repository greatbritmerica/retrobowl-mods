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
    CInstance *ol[8]; double ol_d[8], front, rb_d; int nol;                                    // interior linemen (protection calls)
    int fingers; int legs_done; double slide_read; int prot, prot_frames, prot_noted;                            // this play's protection, how long it held someone up
} O;

// Protection call. It stays as set from play to play (shown on the button) until changed.
//   max    - the back stays in and picks up the free rusher closest to the QB (one fewer route)
//   slide  - the line shades up or down the screen: rushers on that side are held up longer and the
//            free linemen help there, so the other side is where pressure (and your escape) comes from
enum { PROT_NORMAL, PROT_MAX, PROT_UP, PROT_DOWN, PROT_N };
static const char *PROT_BTN[PROT_N] = { "Protect: normal", "Protect: MAX", "Protect: slide up", "Protect: slide down" };
static const char *PROT_NAME[PROT_N] = { "Normal", "Max protect", "Slide up", "Slide down" };
static int g_prot;
// Which way you've been sliding lately (-1 = always up, +1 = always down). A defense that keeps seeing
// the same slide brings speed off the other edge; mixing the calls keeps it honest.
static double g_slide_lean;
#define SLIDE_READ_FROM 0.40      // lean at which they start to catch on (about the 3rd same slide in a row)
#define SLIDE_EDGE_MAX  0.30      // the weak-side edge rusher's extra acceleration when they're sure

// The back's legs. Every carry costs some (more for a long one); a play he doesn't carry gives a little
// back; staying in on max protect costs a little instead. Tired legs are slower off the handoff. He
// starts every one of your drives fresh (what a long drive costs him later is a question for after the
// defense work).
static double g_legs = 1.0;
#define LEGS_CARRY      0.14      // per carry
#define LEGS_PER_YARD   0.006     // extra per yard gained (to 20 yards)
#define LEGS_REST       0.06      // back on a play he doesn't carry (0.10 let one pass undo most of a carry: he never tired)
#define LEGS_BLOCK      0.0       // staying in to block: no rest, no cost (0.03 a play ran him to gassed without a carry)
#define LEGS_FRESH      0.75      // at or above: full speed
#define LEGS_WINDED     0.40      // below: gassed
#define LEGS_FLOOR      0.65      // acceleration with nothing left
#define PX_PER_YD       20.5
static const char *legs_word(void) { return g_legs >= LEGS_FRESH ? "fresh" : g_legs >= LEGS_WINDED ? "winded" : "gassed"; }
static char g_last_prot[96];                                      // shown before the next snap
#define BLOCK_REACH   17.0        // blocker and rusher this close: engaged
#define MAX_HOLD      0.30        // rusher's drive while the back has him
#define SLIDE_HOLD    0.50        // rusher's drive against the slid line

// UI callbacks arrive on the main thread between frames; the step acts on them
static volatile int g_ev_presnap, g_ev_rcv, g_ev_route, g_ev_pump;
static char g_last_pa[96];                                        // shown before the next snap

static void cb_presnap(int i) { g_ev_presnap = i ? i : -1; }
static void cb_rcv(int i)     { g_ev_rcv = i ? i : -1; }
static void cb_route(int i)   { g_ev_route = i ? i : -1; }
static void cb_pump(int i)    { g_ev_pump = i ? 1 : -1; }

static void show_presnap(void) {
    const char *b[3] = { O.pa_on ? "Play action: ON" : "Play action: off", "Hot route", PROT_BTN[g_prot] };
    char msg[300]; int n = 0;
    msg[0] = 0;
    n += snprintf(msg + n, sizeof msg - n, "Back: %s", legs_word());
    if (O.note[0]) n += snprintf(msg + n, sizeof msg - n, "\n%s", O.note);
    if (g_last_pa[0]) n += snprintf(msg + n, sizeof msg - n, "%s%s", n ? "\n" : "", g_last_pa);
    if (g_last_prot[0]) n += snprintf(msg + n, sizeof msg - n, "%s%s", n ? "\n" : "", g_last_prot);
    rb_ui_panel(RB_PANEL_PRESNAP, RB_SIDE_LEFT, 164, "", msg, b, 3, cb_presnap);
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

// front of the offense near the ball (in the direction of play)
static int front_cb(CInstance *inst, void *vc) {
    (void)vc;
    if (inst == O.qb) return 0;
    double x = rb_get_builtin_real(inst, SLOT_X), y = rb_get_builtin_real(inst, SLOT_Y);
    if (fabs(y - O.qby) < 55 && x * O.fwd > O.front) O.front = x * O.fwd;
    return 0;
}
// interior linemen: on that front (the five closest to the ball are kept after the scan)
static int ol_cb(CInstance *inst, void *vc) {
    (void)vc;
    if (inst == O.qb || O.nol >= 8) return 0;
    double x = rb_get_builtin_real(inst, SLOT_X), y = rb_get_builtin_real(inst, SLOT_Y);
    if (fabs(x * O.fwd - O.front) > 16 || fabs(y - O.qby) >= 55) return 0;
    O.ol[O.nol] = inst; O.ol_d[O.nol] = fabs(y - O.qby); O.nol++;
    return 0;
}
// the back: behind the line, closest to the spot just behind the QB (not the QB, not a lineman)
static int back_cb(CInstance *inst, void *vc) {
    (void)vc;
    if (inst == O.qb) return 0;
    for (int k = 0; k < O.nol; k++) if (O.ol[k] == inst) return 0;
    double x = rb_get_builtin_real(inst, SLOT_X), y = rb_get_builtin_real(inst, SLOT_Y);
    if ((x - O.los) * O.fwd > -20) return 0;
    double d = hypot(x - (O.qbx - O.fwd * 40), y - O.qby);
    if (d < 110 && d < O.rb_d) { O.rb_d = d; O.rb = inst; }
    return 0;
}

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
            // The real line: the front of the offense around the ball. (Under centre that is ~40 px ahead
            // of the QB, in the shotgun ~100 - a fixed offset from the QB missed the whole line there.)
            RValue r2 = rb_obj_ref(OBJ_PLAYER_OF);
            O.front = -1e9; O.nol = 0; O.rb = 0; O.rb_d = 1e9;
            rb_with(&r2, ball, other, front_cb, 0);
            double ahead = O.front - O.qbx * O.fwd;
            if (ahead > 10 && ahead < 170) {
                O.los = O.front * O.fwd + O.fwd * 6;
                RValue r4 = rb_obj_ref(OBJ_PLAYER_OF);
                rb_with(&r4, ball, other, ol_cb, 0);
                for (int i = 0; i < O.nol; i++)                       // closest to the ball first
                    for (int j = i + 1; j < O.nol; j++)
                        if (O.ol_d[j] < O.ol_d[i]) {
                            CInstance *ti = O.ol[i]; O.ol[i] = O.ol[j]; O.ol[j] = ti;
                            double td = O.ol_d[i]; O.ol_d[i] = O.ol_d[j]; O.ol_d[j] = td;
                        }
                if (O.nol > 5) O.nol = 5;
            }
            RValue r5 = rb_obj_ref(OBJ_PLAYER_OF);
            rb_with(&r5, ball, other, back_cb, 0);
            rb_log("offtools: line %.0f px ahead of the QB, %d linemen, back %s", ahead, O.nol, O.rb ? "found" : "none");
            if (O.rb && g_legs < LEGS_FRESH) {                    // tired legs (players are rebuilt every play, so this is per play)
                double m = LEGS_FLOOR + (g_legs / LEGS_FRESH) * (1.0 - LEGS_FLOOR), a = rb_get_inst_real(O.rb, SLOT_MY_ACCEL);
                if (a > 0) rb_set_inst_real(O.rb, SLOT_MY_ACCEL, a * m);
                rb_log("offtools: back is %s (legs %.0f%%) - acceleration x%.2f (%.3f -> %.3f)", legs_word(), g_legs * 100, m, a, a * m);
            }
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
            else if (e == 3) { g_prot = (g_prot + 1) % PROT_N; rb_log("offtools: protection set to %s", PROT_NAME[g_prot]); show_presnap(); }
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
        if (!O.run_play) {
            double side = g_prot == PROT_UP ? -1 : g_prot == PROT_DOWN ? 1 : 0;
            if (side != 0 && g_slide_lean * side > SLIDE_READ_FROM) {
                O.slide_read = (g_slide_lean * side - SLIDE_READ_FROM) / (1.0 - SLIDE_READ_FROM);
                int e = -1; double far = 0;                           // widest rusher on the side you slid away from
                for (int i = 0; i < O.ndf; i++) {
                    if (!O.rusher[i]) continue;
                    double off = (rb_get_builtin_real(O.df[i], SLOT_Y) - O.qby) * -side;
                    if (off > 8 && off > far) { far = off; e = i; }
                }
                if (e >= 0) {
                    double a = rb_get_inst_real(O.df[e], SLOT_MY_ACCEL), m = 1.0 + SLIDE_EDGE_MAX * O.slide_read;
                    if (a > 0) rb_set_inst_real(O.df[e], SLOT_MY_ACCEL, a * m);
                    rb_log("offtools: same slide again (lean %.2f) - weak-side edge rusher x%.2f acceleration", g_slide_lean, m);
                } else O.slide_read = 0;
            }
            g_slide_lean += (side - g_slide_lean) * 0.25;
        }
        O.prot = O.run_play ? PROT_NORMAL : g_prot;
        g_last_prot[0] = 0;
        if (O.prot) {
            if (O.prot == PROT_MAX && !O.rb) O.prot = PROT_NORMAL;      // empty backfield: nobody to keep in
            rb_log("offtools: protection %s - %d linemen%s", PROT_NAME[O.prot], O.nol, O.prot == PROT_MAX ? ", back stays in" : "");
        }
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
    if (!O.legs_done && (st == 4 || st == 6 || st == 7 || st == 8 || st == 9 || st == 11)) {   // play over: the back's legs
        O.legs_done = 1;
        double before = g_legs;
        if (O.run_play) {
            double yds = (rb_get_builtin_real(ball, SLOT_X) - O.los) * O.fwd / PX_PER_YD;
            if (yds < 0) yds = 0;
            if (yds > 20) yds = 20;
            g_legs -= LEGS_CARRY + LEGS_PER_YARD * yds;
            if (g_legs < 0) g_legs = 0;
            rb_log("offtools: carry for %.0f yds - back's legs %.0f%% -> %.0f%% (%s)", yds, before * 100, g_legs * 100, legs_word());
        } else {
            g_legs += O.prot == PROT_MAX ? -LEGS_BLOCK : LEGS_REST;
            if (g_legs > 1) g_legs = 1;
            if (g_legs < 0) g_legs = 0;
            if (g_legs != before) rb_log("offtools: back %s - legs %.0f%% -> %.0f%% (%s)", O.prot == PROT_MAX ? "stayed in to block" : "rested",
                                         before * 100, g_legs * 100, legs_word());
        }
    }
    if (O.pa_live && O.t - O.t_snap >= PA_FRAMES) O.pa_live = 0;
    if (st == 3 || st == 5 || st == 19) O.pa_live = 0;              // ball's out or someone has it
    if (O.prot && !O.prot_noted && (st == 3 || st == 5 || st == 11)) { // how the protection did, for the next huddle
        O.prot_noted = 1;
        int held = O.prot_frames > 8;
        if (O.prot == PROT_MAX)
            snprintf(g_last_prot, sizeof g_last_prot, held ? "Max protect: your back picked up a rusher" : "Max protect: no free rusher came");
        else
            snprintf(g_last_prot, sizeof g_last_prot,
                     O.slide_read > 0.15 ? "%s again: they sent speed off the other edge" : held ? "%s: that side held" : "%s: the rush came from the other side",
                     PROT_NAME[O.prot]);
        rb_log("offtools: %s%s (rushers held for %d frames, %.1f s to %s)", g_last_prot, st == 11 ? " - sacked anyway" : "",
               O.prot_frames, (O.t - O.t_snap) / 60.0, st == 3 ? "throw" : st == 5 ? "scramble" : "sack");
    }

    // pump fake while aiming
    if (st == 2 && O.pumps < MAX_PUMPS) {
        if (!O.pump_shown) {
            static const char *b[1] = { "PUMP" };
            rb_ui_panel(RB_PANEL_PUMP, RB_SIDE_LEFT_BOTTOM, 124, "", "", b, 1, cb_pump);
            O.pump_shown = 1;
        }
        // or a second finger anywhere on the screen
        // The engine's "pressed this step" flag for the second finger never once fired in any log, so
        // watch the held state of every extra finger and catch the moment one goes down ourselves.
        static void *f_held;
        if (!f_held) f_held = rb_builtin("device_mouse_check_button");
        int down = 0;
        for (int dev = 1; f_held && dev <= 3; dev++) {
            double a[2] = { dev, 1 };                                  // finger dev+1, mb_left
            if (rb_call(f_held, ball, 2, a) > 0.5) down |= 1 << dev;
        }
        if ((down & ~O.fingers) && !g_ev_pump) { g_ev_pump = 2; rb_log("offtools: extra finger down (mask %d)", down); }
        O.fingers = down;
    } else {
        O.fingers = 0;
        if (O.pump_shown) { rb_ui_panel_close(RB_PANEL_PUMP); O.pump_shown = 0; }
    }
    if (g_ev_pump) {
        int e = g_ev_pump; g_ev_pump = 0;
        if (O.pump_shown) { rb_ui_panel_close(RB_PANEL_PUMP); O.pump_shown = 0; }
        if (e > 0 && !(st == 2 && O.t >= O.pump_until + 8))            // say why a tap did nothing
            rb_log("offtools: pump %s ignored - %s", e == 2 ? "finger" : "button",
                   st != 2 ? "not aiming any more" : "still in the last pump");
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
            rb_log("offtools: pump (%s) registered", e == 2 ? "finger" : "button");
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

// Protection, while the QB is in the pocket. Blocks are bodies meeting (the game's own collisions); on
// top of that a rusher who is engaged with the kept-in back, or with the slid line, drives at part
// strength for as long as they are in contact. Returns 1 when this player was steered.
static int protect(CInstance *player, double x, double y, int is_def) {
    double qx = rb_get_builtin_real(O.qb, SLOT_X), qy = rb_get_builtin_real(O.qb, SLOT_Y);
    if (qx < -9000) return 0;
    double side = O.prot == PROT_UP ? -1 : 1;                           // screen y grows downward
    if (is_def) {
        double hold = 1.0;
        if (O.prot == PROT_MAX) {
            if (O.rb && hypot(rb_get_builtin_real(O.rb, SLOT_X) - x, rb_get_builtin_real(O.rb, SLOT_Y) - y) < BLOCK_REACH) hold = MAX_HOLD;
        } else if ((y - qy) * side > -8) {                              // on the slide side (or right up the middle)
            for (int k = 0; k < O.nol; k++)
                if (hypot(rb_get_builtin_real(O.ol[k], SLOT_X) - x, rb_get_builtin_real(O.ol[k], SLOT_Y) - y) < BLOCK_REACH) { hold = SLIDE_HOLD; break; }
        }
        if (hold < 1.0) {
            double jf = rb_get_inst_real(player, SLOT_JFORCE);            // (a cap, so it can't compound frame to frame)
            if (jf > hold) rb_set_inst_real(player, SLOT_JFORCE, hold);
            O.prot_frames++;
        }
        return 0;                                                       // (still his own path, just slower)
    }
    if (O.prot == PROT_MAX && player == O.rb) {
        // the free rusher closest to the QB; meet him a few steps out, between him and the QB
        int best = -1, btaken = 0; double bd = 240, bx = 0, by = 0;       // (170 never reached the line from the shotgun, ~100 px away)
        for (int k = 0; k < O.ndf; k++) {
            double dx = rb_get_builtin_real(O.df[k], SLOT_X), dy = rb_get_builtin_real(O.df[k], SLOT_Y);
            if (dx < -9000) continue;
            int taken = 0;                                              // a lineman already has him
            for (int j = 0; j < O.nol && !taken; j++)
                taken = hypot(rb_get_builtin_real(O.ol[j], SLOT_X) - dx, rb_get_builtin_real(O.ol[j], SLOT_Y) - dy) < BLOCK_REACH;
            double d = hypot(dx - qx, dy - qy) + (taken ? 60 : 0);
            if (d < bd) { bd = d; best = k; bx = dx; by = dy; btaken = taken; }
        }
        if (best >= 0) {
            // a free rusher: meet him a few steps out. One a lineman already has: go and help on him
            // (the back used to wait by the QB for a free man who rarely came).
            double d = hypot(bx - qx, by - qy), out = btaken ? d - 10 : (d * 0.6 < 34 ? d * 0.6 : 34);
            if (out < 0) out = 0;
            steer(player, x, y, qx + (bx - qx) / d * out, qy + (by - qy) / d * out, 1.0);
        } else steer(player, x, y, qx - O.fwd * 4, qy + (y < qy ? -24 : 24), 0.6);   // nobody free: set up beside the QB
        return 1;
    }
    if (O.prot == PROT_UP || O.prot == PROT_DOWN) {                     // a lineman with nobody on him helps the slide side
        int k = 0;
        for (; k < O.nol; k++) if (O.ol[k] == player) break;
        if (k == O.nol) return 0;
        int best = -1; double bd = 110, bx = 0, by = 0;
        for (int j = 0; j < O.ndf; j++) {
            double dx = rb_get_builtin_real(O.df[j], SLOT_X), dy = rb_get_builtin_real(O.df[j], SLOT_Y);
            if (dx < -9000) continue;
            double d = hypot(dx - x, dy - y);
            if (d < BLOCK_REACH + 5) return 0;                          // he's busy
            if ((dy - qy) * side > 0 && fabs(dx - qx) < 90 && d < bd) { bd = d; best = j; bx = dx; by = dy; }
        }
        if (best < 0) return 0;
        steer(player, x, y, bx, by, 0.8);
        return 1;
    }
    return 0;
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
    if (O.prot && (O.st == 1 || O.st == 2) && protect(player, x, y, i < O.ndf)) return;
    if (i == O.ndf || O.rusher[i]) return;
    if (O.t < O.pump_until && O.st == 2) {                            // bit on the pump
        if (hypot(O.plx - x, O.ply - y) < 150) steer(player, x, y, O.plx, O.ply, 1.0);
        return;
    }
    if (O.pa_live && since < PA_FRAMES) steer(player, x, y, O.los + O.fwd * 15, y, O.bite);   // step up for the run
}

double rb_off_pa_bite(void) { return O.ball ? O.bite : 0; }
// The back who stayed in to block stands in front of the QB: the throw leaves over him, he isn't the target.
int rb_off_blocker_ignores_ball(CInstance *player) {
    if (!O.ball || O.prot != PROT_MAX || player != O.rb || !O.qb) return 0;
    if (rb_get_inst_real(O.ball, SLOT_STATE) != 3) return 0;
    double d = hypot(rb_get_builtin_real(O.ball, SLOT_X) - rb_get_builtin_real(O.qb, SLOT_X),
                     rb_get_builtin_real(O.ball, SLOT_Y) - rb_get_builtin_real(O.qb, SLOT_Y));
    return d < 70;
}
// The match objects are being destroyed (every play is rebuilt from scratch): drop every pointer.
void rb_off_forget(void) { O.ball = 0; O.qb = O.rb = 0; O.nol = O.ndf = O.nrcv = 0; O.t_snap = 0; O.prot = 0; }
// The other team is running its offense: your back is fresh again for your next drive.
void rb_off_cpu_drive(void) {
    if (g_legs < 1.0) { rb_log("offtools: other team has the ball - back fresh for your next drive (was %.0f%%)", g_legs * 100); g_legs = 1.0; }
}
// the real line of scrimmage this play (x), or a huge negative before it is known
double rb_off_los(void) { return O.ball && O.fwd ? O.los : -1e9; }
// seconds since the snap on your pass play (-1 when there is none)
double rb_off_snap_seconds(void) { return O.ball && O.t_snap ? (O.t - O.t_snap) / 60.0 : -1; }
int rb_off_scrambled(void) { return O.ball && O.scrambled; }
