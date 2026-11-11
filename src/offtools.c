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
#include "tune.h"
#include "rules.h"
#include "input.h"

#define SLOT_MY_ROUTE   0x100fa6ce8ULL
#define VA_SET_MY_ROUTE 0x100513734ULL     // gml_Script_s_set_my_route(route_name), run as the receiver
#define VA_RV_FROM_CSTR 0x100bd6f74ULL     // YYRValue(const char*): how the game builds its string constants
#define DRAG_DECAY      0.986
#define AIR_K           8.75
#define RUSH_DEPTH      45.0
// (the numbers below are tunable from the website: tools/tunables.json -> tune.h)
#define PA_FRAMES       ((unsigned)T(T_PA_FRAMES))
#define PUMP_FRAMES     ((unsigned)T(T_PUMP_FRAMES))
#define MAX_PUMPS       ((int)T(T_MAX_PUMPS))

// s_set_my_route switches on the route's NAME (its case labels are the strings "route_streak", ...;
// read from the game's static initializers). Same names as the path assets.
static const struct { const char *name; const char *route; } ROUTES[] = {
    // Shapes from the game's path assets (forward, then the break; ~20.5 px a yard), shortest first:
    //   hitch   5 yds, stop and turn back          slant   5 yds, then a long diagonal inside
    //   dig     9 yds, square in                   curl    11 yds, back to 8 inside
    //   post    9 yds, angle inside to 12          corner  9 yds, angle outside to 12
    // Dropped after 150 uses in the logs: Flat (a back's route - 1.5 yds up, 3 across, then he stands
    // at the line holding his defender there), Out (1 of 4 worked) and Streak (called once).
    { "Hitch", "route_hitch" }, { "Slant", "route_slant" }, { "Dig", "route_dig2" },
    { "Curl", "route_curl" }, { "Post", "route_post" }, { "Corner", "route_corner" },
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
    int fingers; int rules_fired, nhot, push_frames; int hail, hail_set, hail_over, nhdf; CInstance *hdf[10]; double hail_goal, hail_tx, hail_lo, hail_hi, hdf_y[10]; int sneak, kneel, sneak_hold; unsigned sneak_t, t_run; double push; int legs_done; double slide_read; int prot, prot_frames, prot_noted;                            // this play's protection, how long it held someone up
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
#define SLIDE_READ_FROM T(T_SLIDE_READ_FROM) //     // lean at which they start to catch on (about the 3rd same slide in a row)
#define SLIDE_EDGE_MAX  T(T_SLIDE_EDGE_MAX) //     // the weak-side edge rusher's extra acceleration when they're sure

// The back's legs. Every carry costs some (more for a long one); a play he doesn't carry gives a little
// back; staying in on max protect costs a little instead. Tired legs are slower off the handoff. He
// starts every one of your drives fresh (what a long drive costs him later is a question for after the
// defense work).
static double g_legs = 1.0;
#define LEGS_CARRY      T(T_LEGS_CARRY) //     // per carry
#define LEGS_PER_YARD   T(T_LEGS_PER_YARD) //    // extra per yard gained (to 20 yards)
#define LEGS_REST       T(T_LEGS_REST) //     // back on a play he doesn't carry (0.10 let one pass undo most of a carry: he never tired)
#define LEGS_BLOCK      T(T_LEGS_BLOCK) //      // staying in to block: no rest, no cost (0.03 a play ran him to gassed without a carry)
#define LEGS_FRESH      T(T_LEGS_FRESH) //     // at or above: full speed
#define LEGS_WINDED     T(T_LEGS_WINDED) //     // below: gassed
#define LEGS_FLOOR      T(T_LEGS_FLOOR) //     // acceleration with nothing left
#define PX_PER_YD       20.5
static const char *legs_word(void) { return g_legs >= LEGS_FRESH ? "fresh" : g_legs >= LEGS_WINDED ? "winded" : "gassed"; }
static char g_last_prot[96];                                      // shown before the next snap
#define BLOCK_REACH   17.0        // blocker and rusher this close: engaged
#define MAX_HOLD      T(T_MAX_HOLD) //       // rusher's drive while the back has him
#define SLIDE_HOLD    T(T_SLIDE_HOLD) //       // rusher's drive against the slid line

// UI callbacks arrive on the main thread between frames; the step acts on them
static volatile int g_ev_presnap, g_ev_rcv, g_ev_route, g_ev_pump;
static char g_last_pa[96];                                        // shown before the next snap

static void cb_presnap(int i) { g_ev_presnap = i ? i : -1; }
static void cb_rcv(int i)     { g_ev_rcv = i ? i : -1; }
static void cb_route(int i)   { g_ev_route = i ? i : -1; }
static void cb_pump(int i)    { g_ev_pump = i ? 1 : -1; }

#define RULE_BTNS 3
static struct { int valid; double down, togo, scrim, poss; } g_pre;      // where the play in progress started
static struct { int pending, replay; double yards; } g_pen;             // a penalty a rule called on it
static void sneak_step(CInstance *ball, CInstance *other, double st);
static int push_player(CInstance *player, double x, double y);
static int hail_player(CInstance *p, double x, double y, unsigned since);
static void hail_throw(CInstance *ball);
#define FIELD_TOP_Y 141.0       // sidelines (y)
#define FIELD_BOT_Y 458.0
static CInstance *g_other;        // "other" for instance loops started from a rule
// what rule conditions can see about your offense right now
static void fill_ctx(void) {
    rb_rctx.down = rb_get_obj_real(OBJ_CONTROLLER, SLOT_DOWN);
    rb_rctx.togo = rb_get_obj_real(OBJ_CONTROLLER, SLOT_YARDS);
    rb_rctx.legs = g_legs * 100;
    rb_rctx.run_share = rb_dbrain_run_share() * 100;
    rb_rctx.pass_dial = rb_dbrain_pass_dial() * 100;
    rb_rctx.run_dial = rb_dbrain_run_dial() * 100;
    rb_rctx.play_action = O.pa_on;
    rb_rctx.protection = g_prot;
    rb_rctx.scrambled = O.scrambled;
    rb_rctx.is_run = O.run_play;
    rb_rctx.quarter = rb_get_obj_real(OBJ_CONTROLLER, SLOT_QUARTER);
    rb_rctx.clock = rb_get_obj_real(OBJ_CONTROLLER, SLOT_MINUTES) * 60 + rb_get_obj_real(OBJ_CONTROLLER, SLOT_SECONDS);
    int us = -1, them = -1;
    rb_score(&us, &them);
    rb_rctx.score_diff = us >= 0 && them >= 0 ? us - them : 0;
    rb_rctx.goal = 50 - rb_get_obj_real(OBJ_CONTROLLER, SLOT_SCRIMMAGE);     // the game's own conversion
}
static void show_presnap(void) {
    const char *b[3 + RULE_BTNS] = { O.pa_on ? "Play action: ON" : "Play action: off", "Hot route", PROT_BTN[g_prot] };
    // buttons your rules asked for (a pressed one shows ON)
    static char rl[RULE_BTNS][48];
    const char *rb[RULE_BTNS];
    int nb = 3, nr = rb_rules_buttons(rb, RULE_BTNS);
    for (int i = 0; i < nr; i++) {
        snprintf(rl[i], sizeof rl[i], rb_rules_button_pressed(i) ? "%s: ON" : "%s", rb[i]);
        b[nb++] = rl[i];
    }
    char msg[520]; int n = 0;
    msg[0] = 0;
    n += snprintf(msg + n, sizeof msg - n, "Back: %s", legs_word());
    if (O.note[0]) n += snprintf(msg + n, sizeof msg - n, "\n%s", O.note);
    if (g_last_pa[0]) n += snprintf(msg + n, sizeof msg - n, "%s%s", n ? "\n" : "", g_last_pa);
    if (g_last_prot[0]) n += snprintf(msg + n, sizeof msg - n, "%s%s", n ? "\n" : "", g_last_prot);
    if (rb_rules_msg()[0]) n += snprintf(msg + n, sizeof msg - n, "%s%s", n ? "\n" : "", rb_rules_msg());
    rb_ui_panel(RB_PANEL_PRESNAP, RB_SIDE_LEFT, 164, "", msg, b, nb, cb_presnap);
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
    if (O.sneak) {
        sneak_step(ball, other, st);
        if (st == 0 || st == 1) {                                 // the play is being started for you: no menu
            if (O.presnap_shown) { rb_ui_panel_close(RB_PANEL_PRESNAP); O.presnap_shown = 0; }
            return;
        }
    }

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
        g_other = other;
        if (!O.rules_fired) {                                     // your rules for "before the snap", once a play
            O.rules_fired = 1;
            rb_rctx.held = rb_rctx.depth = rb_rctx.yards = rb_rctx.worked = 0;
            fill_ctx();
            // where this play starts, for a penalty that replays the down
            g_pre.down = rb_rctx.down; g_pre.togo = rb_rctx.togo; g_pre.scrim = rb_get_obj_real(OBJ_CONTROLLER, SLOT_SCRIMMAGE);
            g_pre.poss = rb_get_obj_real(OBJ_CONTROLLER, SLOT_POSSESSION); g_pre.valid = 1;
            g_pen.pending = 0;
            rb_log("offtools: situation - quarter %.0f, %.0f s left, down %.0f & %.1f, yard line %.1f (%.0f to the goal), score %+.0f",
                   rb_rctx.quarter, rb_rctx.clock, rb_rctx.down, rb_rctx.togo, g_pre.scrim, rb_rctx.goal, rb_rctx.score_diff);
            rb_rules_fire("presnap", 0);
        }
        if (!O.presnap_shown) show_presnap();
        // menu events
        if (g_ev_presnap) {
            int e = g_ev_presnap; g_ev_presnap = 0;
            if (e == 1) { O.pa_on = !O.pa_on; rb_log("offtools: play action %s", O.pa_on ? "on" : "off"); show_presnap(); }
            else if (e == 2) open_receivers(ball, other);
            else if (e == 3) { g_prot = (g_prot + 1) % PROT_N; rb_log("offtools: protection set to %s", PROT_NAME[g_prot]); show_presnap(); }
            else if (e >= 4 && e < 4 + RULE_BTNS) { fill_ctx(); rb_rules_press(e - 4); show_presnap(); }
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
                O.nhot++;
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
        g_other = other;
        fill_ctx();
        rb_rules_snapped();
        rb_rules_fire("snap", 0);                                 // your rules for "at the snap"
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
        O.prot = O.run_play || O.sneak ? PROT_NORMAL : g_prot;
        g_last_prot[0] = 0;
        if (O.prot) {
            if (O.prot == PROT_MAX && !O.rb) O.prot = PROT_NORMAL;      // empty backfield: nobody to keep in
            rb_log("offtools: protection %s - %d linemen%s", PROT_NAME[O.prot], O.nol, O.prot == PROT_MAX ? ", back stays in" : "");
        }
        if (O.pa_on && !O.run_play && !O.sneak) {
            double rs = rb_dbrain_run_share(), f = (rs - 0.15) / (T(T_PA_FULL_RUNS) - 0.15 > 0.01 ? T(T_PA_FULL_RUNS) - 0.15 : 0.01);   // ~30% runs already sells it well
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
        if (st == 11) { fill_ctx(); rb_rules_fire("sack", 0); }
        if (O.push > 0) rb_log("offtools: push at %.0f%% - defenders in the pile were held for %d frames", O.push * 100, O.push_frames);
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
    if (O.hail && O.prev_st == 2 && st == 3) hail_throw(ball);
    if (O.hail && !O.hail_over && O.t_snap && (st == 5 || st == 4 || st == 6 || st == 7 || st == 8 || st == 9 || st == 11)) {
        if (!(st == 5 && O.prev_st != 3)) { O.hail_over = 1; rb_log("offtools: Hail Mary over (ball state %.0f)", st); }
    }
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
    if (st == 2 && O.pumps < MAX_PUMPS && !O.sneak) {
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
    if (O.hail && !O.hail_over && hail_player(player, x, y, since)) return;
    if (O.push > 0 && since < (unsigned)T(T_PUSH_FRAMES) && (O.st == 1 || O.st == 2 || O.st == 5) && push_player(player, x, y)) return;
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
// ---- the push (short yardage): the line fires straight ahead, the back gets in behind the QB and
// shoves, and any defender in contact with the pile drives at part strength. Real bodies, as with the
// protection calls. Returns 1 when this player was steered.
static int push_player(CInstance *player, double x, double y) {
    double qx = rb_get_builtin_real(O.qb, SLOT_X), qy = rb_get_builtin_real(O.qb, SLOT_Y);
    if (qx < -9000) return 0;
    if (player == O.qb) return 0;
    if (player == O.rb) { steer(player, x, y, qx - O.fwd * 10, qy, 1.0); return 1; }
    for (int k = 0; k < O.nol; k++)
        if (O.ol[k] == player) { steer(player, x, y, x + O.fwd * 40, y + (qy - y) * 0.3, 1.0); return 1; }
    for (int i = 0; i < O.ndf; i++) {
        if (O.df[i] != player) continue;
        int in_pile = hypot(qx - x, qy - y) < BLOCK_REACH + 4 ||
                      (O.rb && hypot(rb_get_builtin_real(O.rb, SLOT_X) - x, rb_get_builtin_real(O.rb, SLOT_Y) - y) < BLOCK_REACH + 4);
        for (int k = 0; k < O.nol && !in_pile; k++)
            in_pile = hypot(rb_get_builtin_real(O.ol[k], SLOT_X) - x, rb_get_builtin_real(O.ol[k], SLOT_Y) - y) < BLOCK_REACH + 4;
        if (in_pile) {
            double hold = 1.0 - (1.0 - T(T_PUSH_HOLD)) * (O.push > 1.5 ? 1.5 : O.push);
            if (hold < 0.1) hold = 0.1;
            if (rb_get_inst_real(player, SLOT_JFORCE) > hold) rb_set_inst_real(player, SLOT_JFORCE, hold);
            O.push_frames++;
        }
        return 0;                                                       // (his own path, just slower)
    }
    return 0;
}

// ---- the Hail Mary. Everyone who matters races to the end zone - your receivers and as many deep
// defenders - faster than anyone can really run, arriving together a set time after the snap. Then they
// stay in the end zone, and the throw carries there in whatever direction you aim. It ends as a jump
// ball, decided by the usual pick / breakup / catch logic (the Hail Mary rules turn those up).
static void hail_setup(void) {
    double scrim = rb_get_obj_real(OBJ_CONTROLLER, SLOT_SCRIMMAGE), goal = 50 - scrim;
    O.hail_goal = O.los + O.fwd * goal * PX_PER_YD;
    O.hail_tx = O.hail_goal + O.fwd * T(T_HAIL_DEPTH) * PX_PER_YD;
    O.hail_lo = O.hail_goal + O.fwd * 0.8 * PX_PER_YD;                  // inside the end zone
    O.hail_hi = O.hail_goal + O.fwd * 9.2 * PX_PER_YD;
    // the deepest defenders go back with them, each to a receiver's lane
    int want = O.nrcv + (int)T(T_HAIL_DEFENDERS), used[16] = {0};
    O.nhdf = 0;
    for (int n = 0; n < want && O.nhdf < 10; n++) {
        int best = -1; double bd = -1e9;
        for (int i = 0; i < O.ndf; i++) {
            if (used[i]) continue;
            double d = (rb_get_builtin_real(O.df[i], SLOT_X) - O.los) * O.fwd;
            if (d > bd) { bd = d; best = i; }
        }
        if (best < 0) break;
        used[best] = 1;
        O.hdf[O.nhdf] = O.df[best];
        double ry = O.nrcv ? rb_get_builtin_real(O.rcv[n % O.nrcv], SLOT_Y) : O.qby;
        O.hdf_y[O.nhdf] = ry + (n < O.nrcv ? 16 : -16);
        O.nhdf++;
    }
    O.hail_set = 1;
    rb_log("offtools: Hail Mary - goal line %.0f px away (%.0f yds), %d receivers and %d defenders racing back",
           (O.hail_goal - O.los) * O.fwd, goal, O.nrcv, O.nhdf);
}
static int hail_player(CInstance *p, double x, double y, unsigned since) {
    if (!O.hail_set) hail_setup();
    double ty = -1;
    for (int i = 0; i < O.nrcv; i++) if (O.rcv[i] == p) ty = y;
    for (int i = 0; i < O.nhdf && ty < 0; i++) if (O.hdf[i] == p) ty = O.hdf_y[i];
    if (ty < 0) return 0;
    if (ty < FIELD_TOP_Y + 30) ty = FIELD_TOP_Y + 30;
    if (ty > FIELD_BOT_Y - 30) ty = FIELD_BOT_Y - 30;
    double left = (O.hail_tx - x) * O.fwd;
    if (left > 3) {                                                     // racing back: carried there on time
        double budget = T(T_HAIL_FRAMES), frames = budget > since + 1 ? budget - since : 1;
        double step = left / frames;
        if (step < 3) step = 3;
        if (step > left) step = left;
        rb_set_builtin_real(p, SLOT_X, x + O.fwd * step);
        rb_set_builtin_real(p, SLOT_Y, y + (ty - y) * (step / (left + 1)));
        steer(p, x, y, O.hail_tx, ty, 1.0);
        return 1;
    }
    // there: free to move for the ball, but they stay in the end zone
    double lo = O.fwd > 0 ? O.hail_lo : O.hail_hi, hi = O.fwd > 0 ? O.hail_hi : O.hail_lo;
    if (x < lo) rb_set_builtin_real(p, SLOT_X, lo);
    if (x > hi) rb_set_builtin_real(p, SLOT_X, hi);
    return 0;
}
// the throw: whatever the drag, it carries to the end zone along the line you aimed
static void hail_throw(CInstance *ball) {
    if (!O.hail_set) return;
    double qx = rb_get_builtin_real(ball, SLOT_X), qy = rb_get_builtin_real(ball, SLOT_Y);
    double dir = rb_get_inst_real(ball, SLOT_DIREC) / 57.29577951308232;
    double dx = cos(dir), dy = sin(dir);
    if (dx * O.fwd < 0.25) { rb_log("offtools: Hail Mary throw not aimed downfield - left as thrown"); return; }
    double D = (O.hail_tx - qx) / dx;
    double ly = qy + dy * D;
    if (ly < FIELD_TOP_Y + 20 && dy < 0) D = (FIELD_TOP_Y + 20 - qy) / dy;
    if (ly > FIELD_BOT_Y - 20 && dy > 0) D = (FIELD_BOT_Y - 20 - qy) / dy;
    if (D < 60) return;
    double lo = 1, hi = 80;                                             // speed that carries exactly D
    for (int k = 0; k < 50; k++) {
        double v = (lo + hi) / 2, R = v * (1.0 - pow(DRAG_DECAY, AIR_K * v)) / (1.0 - DRAG_DECAY);
        if (R < D) lo = v; else hi = v;
    }
    double v = (lo + hi) / 2, was = rb_get_inst_real(ball, SLOT_VELOCITY);
    rb_set_inst_real(ball, SLOT_VELOCITY, v);
    rb_set_inst_real(ball, SLOT_ZVEL, 0.35 * v);
    rb_log("offtools: Hail Mary throw carries %.0f px (%.0f yds) - speed %.2f -> %.2f", D, D / PX_PER_YD, was, v);
}
// edge icons: where each receiver is, while you aim and while the ball is up
static void *g_fn[8];
static double gcall(int i, const char *name, CInstance *self, int argc, double a, double b, double c, double d, double e, double f, double g) {
    if (!g_fn[i]) g_fn[i] = rb_builtin(name);
    if (!g_fn[i]) return 0;
    double av[7] = { a, b, c, d, e, f, g };
    return rb_call(g_fn[i], self, argc, av);
}
void rb_off_gui(CInstance *self) {
    if (!O.hail || O.hail_over || !O.t_snap || !O.hail_set) return;
    double cam = gcall(0, "view_get_camera", self, 1, 0, 0, 0, 0, 0, 0, 0);
    double vx = gcall(1, "camera_get_view_x", self, 1, cam, 0, 0, 0, 0, 0, 0), vy = gcall(2, "camera_get_view_y", self, 1, cam, 0, 0, 0, 0, 0, 0);
    double vw = gcall(3, "camera_get_view_width", self, 1, cam, 0, 0, 0, 0, 0, 0), vh = gcall(4, "camera_get_view_height", self, 1, cam, 0, 0, 0, 0, 0, 0);
    double gw = gcall(5, "display_get_gui_width", self, 0, 0, 0, 0, 0, 0, 0, 0), gh = gcall(6, "display_get_gui_height", self, 0, 0, 0, 0, 0, 0, 0, 0);
    if (!(vw > 1 && vh > 1 && gw > 1 && gh > 1)) return;
    static const double COL[6] = { 65535, 16776960, 16711935, 4235519, 65280, 16777215 };   // yellow cyan magenta orange lime white (BGR)
    double cx = gw / 2, cy = gh / 2, m = 30;
    for (int i = 0; i < O.nrcv; i++) {
        if (!O.rcv[i]) continue;
        double rx = rb_get_builtin_real(O.rcv[i], SLOT_X), ry = rb_get_builtin_real(O.rcv[i], SLOT_Y);
        if (rx < -9000) continue;
        double gx = (rx - vx) * gw / vw, gy = (ry - vy) * gh / vh;
        if (gx > m && gx < gw - m && gy > m && gy < gh - m) continue;    // on screen: no icon needed
        double ux = gx - cx, uy = gy - cy, s = 1e9;
        if (fabs(ux) > 1e-6) { double q = (cx - m) / fabs(ux); if (q < s) s = q; }
        if (fabs(uy) > 1e-6) { double q = (cy - m) / fabs(uy); if (q < s) s = q; }
        double ix = cx + ux * s, iy = cy + uy * s, ul = hypot(ux, uy), nx = ux / ul, ny = uy / ul;
        gcall(7, "draw_set_alpha", self, 1, 0.92, 0, 0, 0, 0, 0, 0);
        if (!g_fn[7]) return;
        static void *f_col, *f_circ, *f_tri;
        if (!f_col) { f_col = rb_builtin("draw_set_colour"); f_circ = rb_builtin("draw_circle"); f_tri = rb_builtin("draw_triangle"); }
        if (!f_col || !f_circ || !f_tri) return;
        double a1[1] = { 0 }, c1[4] = { ix, iy, 17, 0 }, c2[4] = { ix, iy, 13, 0 };
        rb_call(f_col, self, 1, a1); rb_call(f_circ, self, 4, c1);                  // dark ring
        double tip[7] = { ix + nx * 27, iy + ny * 27, ix + nx * 13 - ny * 9, iy + ny * 13 + nx * 9, ix + nx * 13 + ny * 9, iy + ny * 13 - nx * 9, 0 };
        rb_call(f_tri, self, 7, tip);                                               // pointer toward him
        double a2[1] = { COL[i % 6] };
        rb_call(f_col, self, 1, a2); rb_call(f_circ, self, 4, c2);                  // his colour
    }
    double w[1] = { 16777215 }, one[1] = { 1 };
    if (g_fn[7]) { rb_call(g_fn[7], self, 1, one); }
    static void *f_col2; if (!f_col2) f_col2 = rb_builtin("draw_set_colour");
    if (f_col2) rb_call(f_col2, self, 1, w);
}

// ---- the sneak and the kneel: the mod starts the play for you with the same gesture you would make
// (finger down, pull back to aim, then "throw it backwards" - which is how the QB takes off), then
// hands the controls straight back. A kneel then puts the ball down with the game's own routine.
#define SN_P0X 400.0
#define SN_P0Y 190.0
#define VA_S_SET_BALL_DOWN 0x1005015d4ULL    // gml_Script_s_set_ball_down(), run as the ball
static void sneak_step(CInstance *ball, CInstance *other, double st) {
    unsigned dt = O.t - O.sneak_t;
    if (O.sneak == 1) {
        if (st == 2) { O.sneak = 2; O.sneak_hold = 0; }
        else if (dt > 60 || (st != 0 && st != 1)) {
            rb_vf.down = 0; rb_vf.active = 0; O.sneak = 0; O.kneel = 0;
            rb_log("offtools: sneak could not start the play (ball state %.0f) - controls are yours", st);
            return;
        }
        rb_vf.active = 1;
        rb_vf.room_x = O.qbx - O.fwd * 120; rb_vf.room_y = O.qby;       // an empty spot behind the QB, not on a player
        rb_vf.down = 1; rb_vf.gui_x = dt < 2 ? SN_P0X : SN_P0X - O.fwd * 30; rb_vf.gui_y = SN_P0Y;
    }
    if (O.sneak == 2) {
        if (st != 2 || O.sneak_hold > 45) {
            rb_vf.down = 0; rb_vf.active = 0; O.sneak = 3; O.t_run = O.t;
            rb_log("offtools: sneak - %s", st == 5 ? "the QB has it and is running" : "the QB did not take off (controls are yours)");
            if (st != 5) O.kneel = 0;
            return;
        }
        rb_vf.active = 1;
        rb_vf.room_x = O.qbx - O.fwd * 120; rb_vf.room_y = O.qby;
        rb_vf.down = ++O.sneak_hold < 10; rb_vf.gui_x = SN_P0X + O.fwd * 45; rb_vf.gui_y = SN_P0Y;
    }
    if (O.sneak == 3 && O.kneel == 1 && st == 5 && O.t - O.t_run >= 8) {
        O.kneel = 2;
        RValue res = {0}; res.kind = RV_UNDEFINED;
        ((GMLScript)RB(VA_S_SET_BALL_DOWN))(ball, other, &res, 0, 0);
        rb_log("offtools: kneel - ball put down");
    }
}
void rb_rules_do_play(const char *what, double v) {
    if (!O.ball || !O.fwd) return;
    if (!strcmp(what, "push")) { O.push = v > 0 ? v / 100 : 1.0; rb_log("offtools: rule - push at %.0f%%", O.push * 100); return; }
    if (O.t_snap || O.st != 0) { rb_log("offtools: rule '%s' only works before the snap", what); return; }
    if (!strcmp(what, "hailmary")) {
        for (int i = 0; i < O.nrcv; i++) if (O.rcv[i]) apply_route(O.rcv[i], "route_streak", "Streak", "everyone");
        O.hail = 1;
        snprintf(O.note, sizeof O.note, "HAIL MARY: everyone to the end zone");
        rb_log("offtools: rule - Hail Mary set for this play");
        return;
    }
    if (!strcmp(what, "deep")) {
        for (int i = 0; i < O.nrcv; i++) if (O.rcv[i]) apply_route(O.rcv[i], "route_streak", "Streak", "everyone");
        snprintf(O.note, sizeof O.note, "Everyone goes deep");
        return;
    }
    if (O.sneak) return;
    O.sneak = 1; O.sneak_t = O.t; O.kneel = !strcmp(what, "kneel");
    rb_log("offtools: rule - %s: starting the play", what);
}

// ---- penalties. A rule calls one after a play; it is applied when the game tears the play down, to the
// game's own down, distance and yard line (-50 is your goal line, +50 theirs).
void rb_rules_do_penalty(double yards, int replay) {
    if (!g_pre.valid) return;
    g_pen.pending = 1; g_pen.yards = yards; g_pen.replay = replay;
    rb_log("offtools: rule - penalty of %+.0f yards called (%s)", yards, replay ? "replay the down" : "added to the play");
}
void rb_off_apply_penalty(void) {
    if (!g_pen.pending) return;
    g_pen.pending = 0;
    double poss = rb_get_obj_real(OBJ_CONTROLLER, SLOT_POSSESSION);
    double down = rb_get_obj_real(OBJ_CONTROLLER, SLOT_DOWN), togo = rb_get_obj_real(OBJ_CONTROLLER, SLOT_YARDS);
    double scrim = rb_get_obj_real(OBJ_CONTROLLER, SLOT_SCRIMMAGE);
    if (poss != g_pre.poss || O.st == 6 || O.st == 9) {                 // a score or a change of possession stands
        rb_log("offtools: penalty dropped - the ball changed hands or you scored (state %.0f)", O.st);
        return;
    }
    double from = g_pen.replay ? g_pre.scrim : scrim, base_togo = g_pen.replay ? g_pre.togo : togo;
    double to = from + g_pen.yards;
    if (to > 49) to = 49;                                               // never past a goal line
    if (to < -49) to = -49;
    double moved = to - from, ntogo = base_togo - moved, ndown = g_pen.replay ? g_pre.down : down;
    if (ntogo <= 0) { ndown = 1; ntogo = 50 - to < 10 ? 50 - to : 10; }  // enough for a first down
    rb_set_obj_real(OBJ_CONTROLLER, SLOT_SCRIMMAGE, to);
    rb_set_obj_real(OBJ_CONTROLLER, SLOT_YARDS, ntogo);
    rb_set_obj_real(OBJ_CONTROLLER, SLOT_DOWN, ndown);
    rb_log("offtools: penalty applied - yard line %.1f -> %.1f, down %.0f & %.1f -> down %.0f & %.1f", scrim, to, down, togo, ndown, ntogo);
}

// ---- what a rule can do to your players
void rb_rules_do_legs(char op, double pct) {
    double before = g_legs;
    g_legs = op == '=' ? pct / 100 : g_legs + pct / 100;
    if (g_legs > 1) g_legs = 1;
    if (g_legs < 0) g_legs = 0;
    rb_log("offtools: rule set the back's legs %.0f%% -> %.0f%%", before * 100, g_legs * 100);
}
typedef struct { int mode; double mult; int n; } SpeedCtx;         // mode 0 all defenders, 1 rushers, 2 coverage
static void speed_one(CInstance *inst, double mult, int *n) {
    if (!inst) return;
    double a = rb_get_inst_real(inst, SLOT_MY_ACCEL);
    if (a > 0 && a < 100) { rb_set_inst_real(inst, SLOT_MY_ACCEL, a * mult); (*n)++; }
}
static int speed_df_cb(CInstance *inst, void *vc) {
    SpeedCtx *c = vc;
    int rusher = fabs(rb_get_builtin_real(inst, SLOT_X) - O.los) < RUSH_DEPTH;
    if (c->mode == 0 || (c->mode == 1) == rusher) speed_one(inst, c->mult, &c->n);
    return 0;
}
// this play only (players are rebuilt every play): scale a group's acceleration, which drives speed
void rb_rules_do_speed(const char *group, double pct) {
    if (!O.ball || !O.fwd || pct <= 0) return;
    double mult = pct / 100;
    int n = 0;
    if (!strcmp(group, "qb")) speed_one(O.qb, mult, &n);
    else if (!strcmp(group, "back")) speed_one(O.rb, mult, &n);
    else if (!strcmp(group, "receivers")) { for (int i = 0; i < O.nrcv; i++) speed_one(O.rcv[i], mult, &n); }
    else if (!strcmp(group, "line")) { for (int i = 0; i < O.nol; i++) speed_one(O.ol[i], mult, &n); }
    else {
        SpeedCtx c = { !strcmp(group, "rushers") ? 1 : !strcmp(group, "coverage") ? 2 : 0, mult, 0 };
        if (c.mode == 0 && strcmp(group, "defense")) { rb_log("offtools: rule speed - unknown group '%s'", group); return; }
        RValue d = rb_obj_ref(OBJ_PLAYER_DF);
        rb_with(&d, O.ball, g_other, speed_df_cb, &c);
        n = c.n;
    }
    rb_log("offtools: rule speed %s x%.2f (%d players)", group, mult, n);
}
// for the play report
void rb_off_report(char *buf, size_t n) {
    snprintf(buf, n, "legs=%.0f pa=%d bite=%.2f prot=%d scr=%d pumps=%d hot=%d", g_legs * 100, O.pa_on, O.pa_on ? O.bite : 0.0,
             O.prot, O.scrambled, O.pumps, O.nhot);
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
// the QB or an interior lineman: never the man a pass is going to
int rb_off_not_a_target(CInstance *inst) {
    if (!O.ball) return 0;
    if (inst == O.qb) return 1;
    for (int k = 0; k < O.nol; k++) if (O.ol[k] == inst) return 1;
    return 0;
}
int rb_off_scrambled(void) { return O.ball && O.scrambled; }
