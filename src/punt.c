// punt.c - punts.
//
// YOUR PUNTS. On 4th down the mod's pre-snap panel has a Punt button (tapping Punt on the game's 4th-down
// question does the same). Snap it and either run with the QB (a fake) or pull back to aim: on a punt play
// the pull-back IS the kick - its length is the power, its direction the aim. At the snap their deepest man
// drops to PUNT_RET_DEPTH as the returner. Let go and the punter plays the field goal kicker's kick
// animation (s_kick_ball's s_set_anim(15)); the ball hangs FLIGHT_SECONDS while your cover team runs at it,
// the returner runs under it and his blockers set up in front of him. Then:
//   out of bounds / into the end zone: the game's own punt runs (fired from the controller's GUI step, never
//   inside the ball's step: the punt deletes the ball) with the spot set (touchback = their 20);
//   the returner within PUNT_CATCH yds: he catches it (s_receive_ball as him, ball state 9 = the game's
//   interception return): a live return your players chase. PUNT_MUFF of catches are fumbled
//   (s_drop_ball(true) as him): a live loose ball. When the play ends, the CPU's ball where it's down
//   (made a punt result), unless your team holds it (a recovered muff: the game's result stands);
//   otherwise it lands and rolls up to PUNT_ROLL yds: your cover man touching it downs it, the returner
//   reaching it picks it up and runs.
//
// CPU PUNTS. When the CPU punts to you, the game's punt runs as always, then the mod turns the ball back
// over to the CPU and sends the commentary to its KICKOFF stage: the kickoff return you know starts. On its
// catch everybody is moved so it comes where the punt lands (about PUNT_DIST yards from the CPU's line) and
// the CPU's cover team is brought in to PUNT_COVER yards, as on a real punt. (Moving them before the kick
// froze the kickoff in the v98 test: the kicker never kicked. The ball isn't stepped in the air, so the
// catch is the first chance.)
// Where your returner goes down is where your drive starts. If anything is missing (the
// kickoff stage number isn't known yet, end of the quarter, kick returns off) the game's punt stands.
#include "rb.h"
#include "sys.h"
#include "ui.h"
#include "tune.h"
#include "input.h"
#include "offtools.h"
#include "kickmath.h"
#include "punt.h"

// ---- tunables (Workshop: Punting, Punt returns)
#define PUNT_ON        T(T_PUNT_ON)          // 1 = play your punts
#define PUNT_MIN       T(T_PUNT_MIN)         // yards for the weakest pull-back
#define PUNT_MAX       T(T_PUNT_MAX)         // yards for a full pull-back
#define PUNT_AIM_MAX   T(T_PUNT_AIM_MAX)     // widest aim, degrees off straight downfield
#define FLIGHT_SECONDS T(T_PUNT_FLIGHT)      // seconds the punt flies on screen
#define SHANK_POWER    T(T_SHANK_POWER)      // pull-backs weaker than this (0-1) may shank
#define SHANK_CHANCE   T(T_SHANK_CHANCE)     // chance a weak one shanks (half distance)
#define PUNT_RETURN    T(T_PUNT_RETURN)      // 1 = run back the CPU's punts
#define PUNT_DIST      T(T_PUNT_DIST)        // average CPU punt, yards
#define PUNT_SPREAD    T(T_PUNT_SPREAD)      // CPU punts land up to this much shorter or longer
#define PUNT_PIN       T(T_PUNT_PIN)         // closest yard line (from your goal) a CPU punt lands on
#define PUNT_COVER     T(T_PUNT_COVER)      // yards from your returner to the nearest cover man at the catch
#define PUNT_RET_DEPTH T(T_PUNT_RET_DEPTH)   // yards past the line their returner waits for your punt
#define PUNT_CATCH     T(T_PUNT_CATCH)       // yards: the returner catches it if he's this close where it lands
#define PUNT_MUFF      T(T_PUNT_MUFF)        // chance a catch is fumbled (a live loose ball)
#define PUNT_ROLL      T(T_PUNT_ROLL)        // most yards an uncaught punt rolls
#define FULL_DRAG      100.0                 // the game's pull-back length tops out at 100
#define DOWNED_YDS     1.5                   // one of your cover men this close to a rolling ball downs it
#define ROLL_PX        1.6                   // px per frame a punt rolls
#define BLOCK_YDS      8.0                   // their blockers set up this far in front of the catch
#define MUFF_FRAMES    10                    // frames after the catch the ball comes loose on a muff

#define VA_S_RECEIVE_BALL 0x1004d0974ULL     // a catch, run as the catcher: holder = him, ball state 5
#define VA_S_DROP_BALL    0x100395780ULL     // run as the holder: (true) = fumble, a live loose ball
#define VA_S_SET_ANIM     0x1004fb230ULL     // run as a player: his animation
#define ANIM_KICK         15                 // the one s_kick_ball plays on the kicker
#define BALL_INT_RETURN   9                  // ball state of the game's interception return
#define SLOT_SPRITE_INDEX 0x100fb08d8ULL

#define VA_S_ACTION_RESULT 0x1000f7200ULL
#define CALLER_S_PUNT      0x1004cedacULL    // s_change_possession called from s_punt
#define CALLER_S_KICK_OFF  0x1004315e4ULL    // ... from s_kick_off (an instant kickoff)
#define ACTION_RESULT_PUNT 5
#define FIELD_TOP_Y 141.0
#define FIELD_BOT_Y 458.0
#define NO_SPOT -1000.0

static double rnd(void) { return arc4random_uniform(1000000) / 1000000.0; }
static double human_team(void) { return rb_get_obj_real(OBJ_MASTER, SLOT_PLAYERTEAMINDEX); }
static double possession(void) { return rb_get_obj_real(OBJ_CONTROLLER, SLOT_POSSESSION); }

// ================================================================ your punts
static struct {
    int armed;                 // you tapped Punt: the coming 4th-down play is a punt play
    CInstance *ball;           // that play's ball
    int fire;                  // the flight is over: fire the game's punt from the controller's GUI step
    int kick;                  // kick mode on (the pull-back is the punt)
    double pow, dir;           // the pull-back while held (length, aim in screen degrees)
    int flight; unsigned ft, fn;
    double sx, sy, lx, ly, peak;
    int firing;                // our own s_action_result call
    int snapped;               // the punt play has been snapped (it ends with this play)
    double fwd, los, scrim;    // the field at the kick
    double real_lx; int out, tb;   // where it really comes down (lx is held short of the goal line on screen)
    CInstance *ret, *qb;       // their returner, your punter
    CInstance *of[12]; int nof;    // your players (cover team)
    int roll; unsigned rt, rn; double rx, ry, rdx, rdy;   // an uncaught punt rolling
    int live; unsigned t, muff_at; double last_x; int holder_mine;   // the return, in the game's hands
} P;
static CInstance *g_done_ball;         // the ball your punt left from: the game doesn't step it again
static double g_spot = NO_SPOT;        // the other team's yard line to set after your punt
static void cb_none(int i) { (void)i; }   // the kick panel has no buttons

int rb_punt_play(void) { return P.armed && P.ball; }
int rb_punt_flying(void) { return P.flight || P.roll || g_done_ball; }
int rb_punt_armed(void) { return P.armed; }

// the pre-snap panel's Punt button (your ball, 4th down, before the snap)
void rb_punt_arm(int on) {
    if (P.kick) rb_ui_panel_close(RB_PANEL_KICK);
    memset(&P, 0, sizeof P);
    P.armed = on && PUNT_ON;
    rb_log("punt: Punt button - %s", P.armed ? "punt play on: pull back to aim and kick, or run it (a fake)" : "punt play off");
}

int rb_punt_action_result(double code) {
    if (P.firing) return PR_PASS;
    if (P.live) {                                     // the return is over
        P.live = 0;
        double yl = km_yardline(P.scrim, P.los, P.fwd, P.last_x);
        if (code == 1 || yl <= -50) { rb_log("punt return: result %g at your yard line %.1f (touchdown?) - the game's result stands", code, yl); return PR_PASS; }
        if (P.holder_mine) { rb_log("punt return: your team has the ball (recovered the muff) - the game's result %g stands", code); return PR_PASS; }
        if (possession() != human_team()) {           // the game already gave them the ball: its spot stands
            rb_log("punt return: result %g - possession already %g, the game's result stands (down at your yard line %.1f)", code, possession(), yl);
            return PR_PASS;
        }
        g_spot = km_spot_after_punt(yl);
        rb_log("punt return: result %g - down at x %.0f (your yard line %.1f): made a punt, their ball at %.1f", code, P.last_x, yl, g_spot);
        return PR_AS_PUNT;
    }
    if (g_done_ball) { rb_log("punt: result %g after the punt ignored", code); return PR_SWALLOW; }
    if (P.flight || P.roll) {                         // something else ended the play mid-flight: it's still a punt
        rb_log("punt: play ended (result %g) during the %s - punting now", code, P.roll ? "roll" : "flight");
        P.flight = 0; P.roll = 0; P.armed = 0;
        return PR_AS_PUNT;
    }
    if (code != ACTION_RESULT_PUNT || !PUNT_ON) return PR_PASS;
    double h = human_team(), poss = possession();
    if (h < 0 || poss != h) return PR_PASS;           // the CPU's punts (cpuqb presses the same button) go through
    memset(&P, 0, sizeof P);
    P.armed = 1;
    rb_log("punt: you chose to punt - the game's instant punt is skipped; pull back to aim and kick, or run it (a fake)");
    return PR_SWALLOW;                                // the button then closes the question: your 4th-down play comes up
}

static void fire_punt(CInstance *ball, CInstance *other) {
    RValue code = {0}, res = {0};
    code.kind = RV_REAL; code.real = ACTION_RESULT_PUNT; res.kind = RV_UNDEFINED;
    RValue *av[1] = { &code };
    P.firing = 1;
    ((GMLScript)RB(VA_S_ACTION_RESULT))(ball, other, &res, 1, av);
    P.firing = 0;
}

static double px_x(CInstance *p) { return rb_get_builtin_real(p, SLOT_X); }
static double px_y(CInstance *p) { return rb_get_builtin_real(p, SLOT_Y); }
static double dist_to(CInstance *p, double x, double y) { return hypot(px_x(p) - x, px_y(p) - y); }
static int is_mine(CInstance *p) { for (int i = 0; i < P.nof; i++) if (P.of[i] == p) return 1; return 0; }

// a player runs at (tx, ty) flat out, and stops within stop px
static void steer(CInstance *p, double tx, double ty, double stop) {
    double x = px_x(p), y = px_y(p);
    if (hypot(tx - x, ty - y) < stop) { rb_set_inst_real(p, SLOT_JFORCE, 0); return; }
    double a = atan2(-(ty - y), tx - x) * 57.29577951308232;
    rb_set_inst_real(p, SLOT_JDIR, a < 0 ? a + 360 : a);
    rb_set_inst_real(p, SLOT_JFORCE, 1.0);
}

static RValue *call0(uint64_t va, CInstance *self, CInstance *other, int argc, RValue *arg) {
    static RValue res;
    RValue *av[1] = { arg };
    res.kind = RV_UNDEFINED; res.i64 = 0;
    return ((GMLScript)RB(va))(self, other, &res, argc, argc ? av : 0);
}

static int of_cb(CInstance *inst, void *c) { (void)c; if (P.nof < 12) P.of[P.nof++] = inst; return 0; }
typedef struct { double x, y, best; CInstance *who; } NearCtx;
static int near_cb(CInstance *inst, void *vc) {
    NearCtx *c = vc;
    double d = dist_to(inst, c->x, c->y);
    if (d < c->best) { c->best = d; c->who = inst; }
    return 0;
}
typedef struct { double los, fwd, best; CInstance *who; } DeepCtx;
static int deep_cb(CInstance *inst, void *vc) {
    DeepCtx *c = vc;
    double d = (px_x(inst) - c->los) * c->fwd;
    if (d > c->best) { c->best = d; c->who = inst; }
    return 0;
}

// at the snap: their deepest man drops back to field the punt
static void place_returner(CInstance *ball, CInstance *other) {
    double fwd = rb_off_fwd(), los = rb_off_los();
    if (!fwd || los < -1e8) { rb_log("punt: no returner - can't read the field (fwd %g, line %.0f)", fwd, los); return; }
    DeepCtx c = { los, fwd, -1e9, 0 };
    RValue df = rb_obj_ref(OBJ_PLAYER_DF);
    rb_with(&df, ball, other, deep_cb, &c);
    if (!c.who) { rb_log("punt: no returner - no defenders found"); return; }
    double goal = KM_MID_X + fwd * 50 * KM_PX_PER_YD, x = los + fwd * PUNT_RET_DEPTH * KM_PX_PER_YD;
    if ((goal - x) * fwd < 5 * KM_PX_PER_YD) x = goal - fwd * 5 * KM_PX_PER_YD;   // no deeper than their 5
    double y = (FIELD_TOP_Y + FIELD_BOT_Y) / 2;
    P.ret = c.who;
    rb_log("punt: returner from x %.0f to %.0f,%.0f (%.0f yds past the line)", px_x(c.who), x, y, (x - los) * fwd / KM_PX_PER_YD);
    rb_set_builtin_real(c.who, SLOT_X, x);
    rb_set_builtin_real(c.who, SLOT_Y, y);
}

// the ball's down in the field, out or in the end zone: the game's punt runs with their spot from field x
static void finish(CInstance *ball, double x, const char *how) {
    double yl = km_yardline(P.scrim, P.los, P.fwd, x);
    g_spot = km_spot_after_punt(yl);
    rb_log("punt: %s at your yard line %.1f -> their ball at %.1f%s", how, yl, g_spot, yl >= 50 ? " (touchback)" : "");
    P.flight = 0; P.roll = 0; P.armed = 0; P.kick = 0;
    g_done_ball = ball;                                // held still until the play is torn down
    P.fire = 1;                                        // rb_punt_controller fires the punt
}

// the returner has it: the game's catch, then its interception return (your players chase him)
static void catch_it(CInstance *ball, CInstance *other, int in_air) {
    double x = px_x(P.ret), y = px_y(P.ret), st0 = rb_get_inst_real(ball, SLOT_STATE);
    double lim = 49 * KM_PX_PER_YD;                    // never in an end zone: a held ball there is a score to the game
    if (x < KM_MID_X - lim) x = KM_MID_X - lim;
    if (x > KM_MID_X + lim) x = KM_MID_X + lim;
    rb_set_builtin_real(P.ret, SLOT_X, x);
    rb_set_builtin_real(ball, SLOT_X, x);
    rb_set_builtin_real(ball, SLOT_Y, y);
    rb_set_inst_real(ball, SLOT_HEIGHT, 0);
    call0(VA_S_RECEIVE_BALL, P.ret, ball, 0, 0);
    double st1 = rb_get_inst_real(ball, SLOT_STATE);
    rb_set_inst_real(ball, SLOT_STATE, BALL_INT_RETURN);
    P.flight = 0; P.roll = 0; P.armed = 0; P.kick = 0;
    P.live = 1; P.t = 0; P.muff_at = 0; P.last_x = x; P.holder_mine = 0;
    if (in_air && rnd() < PUNT_MUFF) P.muff_at = MUFF_FRAMES;
    rb_log("punt: %s by the returner at x %.0f (your yard line %.1f) - ball state %g -> %g (catch) -> %d: live return%s",
           in_air ? "caught" : "picked up", x, km_yardline(P.scrim, P.los, P.fwd, x), st0, st1, BALL_INT_RETURN,
           P.muff_at ? " - MUFFED, the ball comes loose" : "");
}

static void land(CInstance *ball, CInstance *other) {
    if (P.out) { finish(ball, P.real_lx, "out of bounds"); return; }
    if (P.tb) { finish(ball, P.real_lx, "into the end zone"); return; }
    double reach = P.ret ? dist_to(P.ret, P.lx, P.ly) / KM_PX_PER_YD : -1;
    if (P.ret && reach <= PUNT_CATCH) { catch_it(ball, other, 1); return; }
    double dx = P.lx - P.sx, dy = P.ly - P.sy, n = hypot(dx, dy);
    double yds = rnd() * PUNT_ROLL;
    P.rn = (unsigned)(yds * KM_PX_PER_YD / ROLL_PX);
    rb_log("punt: lands at x %.0f y %.0f, returner %.1f yds away - rolls %.1f yds", P.lx, P.ly, reach, yds);
    if (!P.rn || n < 1) { finish(ball, P.lx, "downed where it landed"); return; }
    P.roll = 1; P.rt = 0; P.rx = P.lx; P.ry = P.ly;
    P.rdx = dx / n * ROLL_PX; P.rdy = dy / n * ROLL_PX;
    P.flight = 0;
}

static void roll_step(CInstance *ball, CInstance *other) {
    P.rt++;
    P.rx += P.rdx; P.ry += P.rdy;
    double lim = 49 * KM_PX_PER_YD, vx = P.rx;        // on screen held short of the goal lines (a touchdown to the game)
    if (vx < KM_MID_X - lim) vx = KM_MID_X - lim;
    if (vx > KM_MID_X + lim) vx = KM_MID_X + lim;
    rb_set_builtin_real(ball, SLOT_X, vx);
    rb_set_builtin_real(ball, SLOT_Y, P.ry);
    rb_set_inst_real(ball, SLOT_HEIGHT, 3);           // never quite down: a ball on the ground ends the play
    NearCtx c = { P.rx, P.ry, 1e9, 0 };
    for (int i = 0; i < P.nof; i++) if (P.of[i] != P.qb) near_cb(P.of[i], &c);
    if (P.ry < FIELD_TOP_Y || P.ry > FIELD_BOT_Y) finish(ball, P.rx, "rolled out of bounds");
    else if (km_yardline(P.scrim, P.los, P.fwd, P.rx) >= 50) finish(ball, P.rx, "rolled into the end zone");
    else if (P.ret && dist_to(P.ret, P.rx, P.ry) <= PUNT_CATCH * KM_PX_PER_YD) catch_it(ball, other, 0);
    else if (c.who && c.best <= DOWNED_YDS * KM_PX_PER_YD) finish(ball, P.rx, "downed by your cover team");
    else if (P.rt >= P.rn) finish(ball, P.rx, "stopped rolling - downed");
}

typedef struct { CInstance *who; } HolderCtx;
static int holder_cb(CInstance *inst, void *vc) { ((HolderCtx *)vc)->who = inst; return 1; }

// each ball step of the return (the game runs it): where the ball is, who has it, the muff
static void live_step(CInstance *ball, CInstance *other) {
    P.t++;
    P.last_x = px_x(ball);
    RValue *h = rb_inst_var(ball, SLOT_HOLDER);
    HolderCtx hc = { 0 };
    if (h && h->kind != RV_UNDEFINED) { RValue c = *h; rb_with(&c, ball, other, holder_cb, &hc); }
    int mine = hc.who && is_mine(hc.who);
    if (mine != P.holder_mine) rb_log("punt return: %s has the ball (state %g)", mine ? "your team" : "their team", rb_get_inst_real(ball, SLOT_STATE));
    P.holder_mine = mine;
    if (P.muff_at && P.t >= P.muff_at) {
        P.muff_at = 0;
        double st = rb_get_inst_real(ball, SLOT_STATE);
        if (st != BALL_INT_RETURN || hc.who != P.ret) { rb_log("punt return: muff skipped - ball state %g, returner %s the ball", st, hc.who == P.ret ? "has" : "doesn't have"); return; }
        RValue yes = {0}; yes.kind = RV_BOOL; yes.real = 1;
        call0(VA_S_DROP_BALL, P.ret, ball, 1, &yes);  // (other = the ball, as the game's collision check calls it)
        rb_log("punt return: muff - ball loose (state %g -> %g)", st, rb_get_inst_real(ball, SLOT_STATE));
    }
}

// the release: work out where it lands and start the flight
static void kick(CInstance *ball, CInstance *other) {
    rb_ui_panel_close(RB_PANEL_KICK);
    double fwd = rb_off_fwd(), los = rb_off_los(), scrim = rb_get_obj_real(OBJ_CONTROLLER, SLOT_SCRIMMAGE);
    double qx = rb_get_builtin_real(ball, SLOT_X), qy = rb_get_builtin_real(ball, SLOT_Y);
    if (!fwd || los < -1e8 || scrim < -9000 || qx < -9000) {
        rb_log("punt: FALLBACK - can't read the field (fwd %g, line %.0f, yard line %.1f) - the game's own punt", fwd, los, scrim);
        g_spot = NO_SPOT; P.armed = 0;
        g_done_ball = ball; P.fire = 1;
        return;
    }
    double power = P.pow / FULL_DRAG;
    if (power < 0) power = 0;
    if (power > 1) power = 1;
    double yards = PUNT_MIN + power * (PUNT_MAX - PUNT_MIN);
    int shank = power < SHANK_POWER && rnd() < SHANK_CHANCE;
    if (shank) yards *= 0.5;
    PuntLand pl;
    km_punt_land(qx, qy, fwd, P.dir, yards, PUNT_AIM_MAX, FIELD_TOP_Y, FIELD_BOT_Y, &pl);
    double yl = km_yardline(scrim, los, fwd, pl.lx);
    g_spot = km_spot_after_punt(yl);
    rb_log("punt: kick - power %.0f%% (pull-back %.0f), aim %.0f deg, %.0f yds%s -> lands at x %.0f y %.0f%s, your yard line %.1f -> their ball at %.1f%s",
           power * 100, P.pow, P.dir, yards, shank ? " SHANKED" : "", pl.lx, pl.ly, pl.out ? " (OUT OF BOUNDS)" : "",
           yl, g_spot, yl >= 50 ? " (touchback)" : "");
    P.flight = 1; P.ft = 0; P.kick = 0;               // (no power bar from here on)
    P.fn = (unsigned)(FLIGHT_SECONDS * 60);
    if (P.fn < 10) P.fn = 10;
    P.sx = qx; P.sy = qy; P.lx = pl.lx; P.ly = pl.ly;
    P.fwd = fwd; P.los = los; P.scrim = scrim;
    P.real_lx = pl.lx; P.out = pl.out; P.tb = !pl.out && yl >= 50;
    P.nof = 0;
    RValue of = rb_obj_ref(OBJ_PLAYER_OF);
    rb_with(&of, ball, other, of_cb, 0);
    NearCtx nc = { qx, qy, 1e9, 0 };
    for (int i = 0; i < P.nof; i++) near_cb(P.of[i], &nc);
    P.qb = nc.who;
    if (P.qb) {                                        // the field goal kicker's kick, not a throw
        double s0 = rb_get_builtin_real(P.qb, SLOT_SPRITE_INDEX);
        RValue a = {0}; a.kind = RV_REAL; a.real = ANIM_KICK;
        call0(VA_S_SET_ANIM, P.qb, P.qb, 1, &a);
        rb_log("punt: punter's kick animation (sprite %g -> %g), %d cover men, returner %s",
               s0, rb_get_builtin_real(P.qb, SLOT_SPRITE_INDEX), P.nof - 1, P.ret ? "waiting" : "NONE");
    }
    // the ball on screen stops short of the goal lines: a ball over one reads as a touchdown to the game
    double lim = 49 * KM_PX_PER_YD;
    if (P.lx < KM_MID_X - lim) P.lx = KM_MID_X - lim;
    if (P.lx > KM_MID_X + lim) P.lx = KM_MID_X + lim;
    P.peak = yards * KM_PX_PER_YD * 0.25;
    if (P.peak > 120) P.peak = 120;
}

// ================================================================ CPU punts -> live returns
enum { R_IDLE, R_SEEN, R_CONVERT, R_KICK, R_LIVE };
static struct { int st; double cpu_scrim, cpu_team, land, game_spot, ret_x, goal_side, last_x; CInstance *ball; } R;
static double g_stage_kickoff = -1;     // commentary stage number of KICKOFF, learned at the first kickoff

void rb_punt_header(const char *name, double stage) {
    if (!strcmp(name, "MATCH_START")) {                 // a new match: nothing carries over
        if (P.kick) rb_ui_panel_close(RB_PANEL_KICK);
        memset(&P, 0, sizeof P); memset(&R, 0, sizeof R); g_spot = NO_SPOT; g_done_ball = 0;
        return;
    }
    if (!strcmp(name, "KICKOFF")) {
        if (stage != g_stage_kickoff) rb_log("punt return: commentary stage KICKOFF = %g", stage);
        g_stage_kickoff = stage;
        return;
    }
    if (strcmp(name, "PUNT")) return;
    double h = human_team(), poss = possession();
    if (h < 0 || poss < 0 || poss == h || !PUNT_RETURN) return;
    memset(&R, 0, sizeof R);
    R.st = R_SEEN;
    R.cpu_team = poss;
    R.cpu_scrim = rb_get_obj_real(OBJ_CONTROLLER, SLOT_SCRIMMAGE);
    rb_log("punt return: the CPU punts from its own %.0f (yard line %.1f)", 50 + R.cpu_scrim, R.cpu_scrim);
}

void rb_punt_possession(unsigned long long caller, double before, double after, double human) {
    // your punt: the other team starts where it landed
    if (g_spot != NO_SPOT && caller == CALLER_S_PUNT && before == human && after != human) {
        double was = rb_get_obj_real(OBJ_CONTROLLER, SLOT_SCRIMMAGE);
        rb_set_obj_real(OBJ_CONTROLLER, SLOT_SCRIMMAGE, g_spot);
        rb_log("punt: their ball at yard line %.1f (the game had %.1f)", g_spot, was);
        g_spot = NO_SPOT;
    }
    if (R.st == R_SEEN && caller == CALLER_S_PUNT && before != human && after == human) {
        R.game_spot = rb_get_obj_real(OBJ_CONTROLLER, SLOT_SCRIMMAGE);
        double clock = rb_get_obj_real(OBJ_CONTROLLER, SLOT_MINUTES) * 60 + rb_get_obj_real(OBJ_CONTROLLER, SLOT_SECONDS);
        double yards = PUNT_DIST + PUNT_SPREAD * (2 * rnd() - 1), land_cpu = R.cpu_scrim + yards;
        if (land_cpu > 50 - PUNT_PIN) {                 // would reach your end zone: pinned short of it instead (no touchback)
            land_cpu = 50 - PUNT_PIN - 10 * rnd();
            if (land_cpu < R.cpu_scrim + 10) land_cpu = R.cpu_scrim + 10;   // at least 10 yds downfield
            if (land_cpu > 49) land_cpu = 49;
            rb_log("punt return: %.0f-yd punt would reach the end zone - pinned at your %.0f instead", yards, 50 - land_cpu);
            yards = land_cpu - R.cpu_scrim;
        }
        R.land = -land_cpu;
        if (g_stage_kickoff < 0) { rb_log("punt return: FALLBACK - no kickoff seen since the app started, so its stage isn't known: the game's punt stands"); R.st = R_IDLE; return; }
        if (clock < 5) { rb_log("punt return: FALLBACK - %.0f s left in the quarter: the game's punt stands", clock); R.st = R_IDLE; return; }
        R.st = R_CONVERT;
        rb_log("punt return: %.0f-yd punt to your %.0f (the game's spot was %.1f) - turning it into a return", yards, 50 + R.land, R.game_spot);
        return;
    }
    if (R.st == R_KICK && caller == CALLER_S_KICK_OFF && before != human && after == human) {   // no live return (kick returns off)
        rb_set_obj_real(OBJ_CONTROLLER, SLOT_SCRIMMAGE, R.land);
        rb_log("punt return: FALLBACK - the kickoff was instant (kick returns off?) - your ball where the punt landed, yard line %.1f", R.land);
        R.st = R_IDLE;
    }
}

void rb_punt_after_commentary(void) {
    if (R.st != R_CONVERT) return;
    rb_set_obj_real(OBJ_CONTROLLER, SLOT_POSSESSION, R.cpu_team);
    rb_set_obj_real(OBJ_CONTROLLER, SLOT_COMM_STAGE, g_stage_kickoff);
    R.st = R_KICK; R.ret_x = 0;
    rb_log("punt return: ball back to the CPU, commentary sent to KICKOFF (%g)", g_stage_kickoff);
}

void rb_punt_slog(const char *line) {
    const char *o = strstr(line, "Override with a returner: x = ");
    if (o && R.st == R_KICK) {
        double x = 0;
        if (sscanf(o + 30, "%lf", &x) == 1) R.ret_x = x;
    }
}

typedef struct { double dx; int n; } ShiftCtx;
static int shift_cb(CInstance *inst, void *vc) {
    ShiftCtx *c = vc;
    double x = rb_get_builtin_real(inst, SLOT_X);
    if (x > -9000) { rb_set_builtin_real(inst, SLOT_X, x + c->dx); c->n++; }
    return 0;
}

// everybody (both teams and the ball) moved so the returner stands where the punt lands
static void shift_to_landing(CInstance *self, CInstance *other) {
    if (!R.ret_x) { R.ret_x = 2400; rb_log("punt return: no returner line seen - assuming x 2400"); }
    R.goal_side = R.ret_x > KM_MID_X ? 1 : -1;
    double to = km_x_of_yardline(R.land, R.goal_side), dx = to - R.ret_x;
    ShiftCtx c = { dx, 0 }, cb = { dx, 0 };
    RValue of = rb_obj_ref(OBJ_PLAYER_OF), df = rb_obj_ref(OBJ_PLAYER_DF), bl = rb_obj_ref(OBJ_BALL);
    rb_with(&of, self, other, shift_cb, &c);
    rb_with(&df, self, other, shift_cb, &c);
    rb_with(&bl, self, other, shift_cb, &cb);
    rb_log("punt return: catch moved from x %.0f to %.0f (your %.0f) - %d players and %d balls moved %.0f px",
           R.ret_x, to, 50 + R.land, c.n, cb.n, dx);
}

// at the catch: the cover team (the side farther from the ball) comes in to PUNT_COVER yards
typedef struct { double bx, gs, sum, near; int n; } SideCtx;
static int side_cb(CInstance *inst, void *vc) {
    SideCtx *c = vc;
    double x = rb_get_builtin_real(inst, SLOT_X);
    if (x < -9000) return 0;
    double d = (c->bx - x) * c->gs;                    // yards-in-px upfield of the returner
    c->sum += fabs(c->bx - x); c->n++;
    if (d < c->near) c->near = d;
    return 0;
}
static void cover_in(CInstance *ball, CInstance *other) {
    double bx = rb_get_builtin_real(ball, SLOT_X);
    SideCtx so = { bx, R.goal_side, 0, 1e9, 0 }, sd = so;
    RValue of = rb_obj_ref(OBJ_PLAYER_OF), df = rb_obj_ref(OBJ_PLAYER_DF);
    rb_with(&of, ball, other, side_cb, &so);
    rb_with(&df, ball, other, side_cb, &sd);
    if (!so.n || !sd.n) { rb_log("punt return: cover team not found (%d / %d players)", so.n, sd.n); return; }
    int of_covers = so.sum / so.n > sd.sum / sd.n;
    SideCtx *k = of_covers ? &so : &sd;
    double gap = PUNT_COVER * KM_PX_PER_YD, pull = k->near - gap;
    if (pull <= 0) { rb_log("punt return: cover team already %.0f yds away", k->near / KM_PX_PER_YD); return; }
    ShiftCtx c = { pull * R.goal_side, 0 };
    RValue r = rb_obj_ref(of_covers ? OBJ_PLAYER_OF : OBJ_PLAYER_DF);
    rb_with(&r, ball, other, shift_cb, &c);
    rb_log("punt return: catch at x %.0f - cover team (%s, %d players) brought in from %.0f to %.0f yds",
           bx, of_covers ? "OF" : "DF", c.n, k->near / KM_PX_PER_YD, PUNT_COVER);
}

static void return_step(CInstance *ball, CInstance *other) {
    if (R.st == R_KICK) {                              // the kickoff ball's first step: the catch
        shift_to_landing(ball, other);
        cover_in(ball, other);
        R.st = R_LIVE; R.ball = ball; R.last_x = rb_get_builtin_real(ball, SLOT_X);
        rb_log("punt return: live (ball state %g)", rb_get_inst_real(ball, SLOT_STATE));
        return;
    }
    if (R.st == R_LIVE) {
        if (ball != R.ball) { R.st = R_IDLE; return; }
        R.last_x = rb_get_builtin_real(ball, SLOT_X);
    }
}

void rb_punt_destroy(void) {
    if (R.st == R_LIVE) {                              // the return is over: check the spot the game gave you
        double want = km_yardline_of_x(R.last_x, R.goal_side), got = rb_get_obj_real(OBJ_CONTROLLER, SLOT_SCRIMMAGE);
        double h = human_team(), poss = possession(), down = rb_get_obj_real(OBJ_CONTROLLER, SLOT_DOWN);
        rb_log("punt return: down at x %.0f = your %.0f (game's yard line %.1f, possession %g, down %g)", R.last_x, 50 + want, got, poss, down);
        if (poss == h && down == 1 && want > -50 && want < 50 && fabs(want - got) > 2) {
            if (want > 49) want = 49;
            if (want < -49) want = -49;
            rb_set_obj_real(OBJ_CONTROLLER, SLOT_SCRIMMAGE, want);
            rb_log("punt return: spot corrected %.1f -> %.1f", got, want);
        }
        R.st = R_IDLE;
    }
    g_done_ball = 0;
    if (P.live) {                                      // torn down with no result seen (match quit)
        rb_log("punt return: play torn down - return tracking dropped");
        memset(&P, 0, sizeof P);
    } else if (P.flight || P.roll) {                   // torn down mid-flight (match quit): drop it all
        rb_log("punt: play torn down during the flight - punt dropped");
        rb_ui_panel_close(RB_PANEL_KICK);
        memset(&P, 0, sizeof P); g_spot = NO_SPOT;
    } else if (P.armed && P.snapped) {           // the punt play is over (a fake, or it ended before the kick)
        if (P.kick) rb_ui_panel_close(RB_PANEL_KICK);
        rb_log("punt: punt play over without a kick (a fake)");
        memset(&P, 0, sizeof P);
    } else if (P.fire) {                               // torn down before the punt fired: nothing to punt from
        rb_log("punt: play torn down before the punt fired - dropped");
        memset(&P, 0, sizeof P); g_spot = NO_SPOT;
    } else P.ball = 0;                                  // rebuilt before the snap: find the new one
}

// ================================================================ per ball step
int rb_punt_ball(CInstance *ball, CInstance *other) {
    if (R.st == R_KICK || R.st == R_LIVE) return_step(ball, other);
    if (ball == g_done_ball) return 1;
    if (P.live) {                                      // the return: the game steps the ball
        if (ball == P.ball) live_step(ball, other);
        else { rb_log("punt return: a new play - return tracking dropped"); memset(&P, 0, sizeof P); }
        return 0;
    }
    if (!P.armed) return 0;
    if (!P.flight && !P.roll && possession() != human_team()) {   // the ball changed hands before your punt play: it's off
        rb_log("punt: not your ball any more - punt play dropped");
        if (P.kick) rb_ui_panel_close(RB_PANEL_KICK);
        memset(&P, 0, sizeof P);
        return 0;
    }
    double st = rb_get_inst_real(ball, SLOT_STATE);
    if (!P.ball) {
        if (st != 0) return 0;                         // waiting for the 4th-down play to line up
        P.ball = ball;
        rb_log("punt: punt play lined up");
    } else if (ball != P.ball) {                       // a new play: this one is over
        if (!P.flight) rb_log("punt: played as a fake (no kick)");
        if (P.kick) rb_ui_panel_close(RB_PANEL_KICK);
        memset(&P, 0, sizeof P);
        return 0;
    }
    if (P.flight) {
        P.ft++;
        double p = (double)P.ft / P.fn;
        if (p > 1) p = 1;
        rb_set_builtin_real(ball, SLOT_X, P.sx + (P.lx - P.sx) * p);
        rb_set_builtin_real(ball, SLOT_Y, P.sy + (P.ly - P.sy) * p);
        rb_set_inst_real(ball, SLOT_HEIGHT, 4 * P.peak * p * (1 - p) + 3);   // never quite down: a ball on the ground ends the play
        if (P.ft >= P.fn) {
            land(ball, other);
            if (P.live) return 0;                      // caught: the game steps the ball from here
        }
        return 1;                                      // the game doesn't step the ball (no throw, no tackle of the ball)
    }
    if (P.roll) { roll_step(ball, other); return !P.live; }
    if (st == 0) return 0;                             // before the snap
    if (!P.snapped) { P.snapped = 1; rb_log("punt: snapped"); place_returner(ball, other); }
    if (!P.kick) {
        if (st != 2) return 0;                          // aiming on a punt play: the pull-back is the kick
        double fwd = rb_off_fwd(), los = rb_off_los(), qx = rb_get_builtin_real(ball, SLOT_X);
        if (fwd && los > -1e8 && (qx - los) * fwd >= 0) return 0;   // past the line: no kick (a fake run)
        P.kick = 1; P.pow = rb_get_inst_real(ball, SLOT_DRAGDIST); P.dir = rb_get_inst_real(ball, SLOT_DIREC);
        static const char *none[1];
        rb_ui_panel(RB_PANEL_KICK, RB_SIDE_RIGHT_BOTTOM, 150, "KICK", "Pull back for power and aim, let go to punt", none, 0, cb_none);
        rb_log("punt: kick mode on (ball state %g, QB at x %.0f)", st, qx);
        return 0;
    }
    int down; double gx, gy;
    rb_real_touch(ball, &down, &gx, &gy);
    if (down) {
        if (st == 2) { P.pow = rb_get_inst_real(ball, SLOT_DRAGDIST); P.dir = rb_get_inst_real(ball, SLOT_DIREC); }
        if (st == 1 || st == 2) return 0;
        rb_log("punt: kick mode off - ball state %g (the play went on without the kick)", st);
        rb_ui_panel_close(RB_PANEL_KICK);
        P.kick = 0;
        return 0;
    }
    if (st == 1 || st == 2) { kick(ball, other); return 1; }   // let go: punt (the game never sees the release)
    return 0;
}

// while the punt is up or rolling: your punter holds, your cover team runs at it, their returner gets under
// it and the rest of them set up in front of him
void rb_punt_after_ai(CInstance *player) {
    if (!P.flight && !P.roll) {
        if (player == P.ret && P.kick) rb_set_inst_real(player, SLOT_JFORCE, 0);   // he waits deep while you aim (free on a fake)
        return;
    }
    if (player == P.qb) { rb_set_inst_real(player, SLOT_JFORCE, 0); return; }
    double tx = P.roll ? P.rx : P.lx, ty = P.roll ? P.ry : P.ly, y = px_y(player);
    if (player == P.ret) steer(player, tx, ty, 4);
    else if (is_mine(player)) steer(player, tx, (2 * ty + y) / 3, 10);
    else steer(player, tx - P.fwd * BLOCK_YDS * KM_PX_PER_YD, (ty + y) / 2, 10);
}

// controller GUI step (where the CPU's punts and kicks are fired too): fire your punt once the flight is over
void rb_punt_controller(CInstance *self, CInstance *other) {
    if (!P.fire) return;
    P.fire = 0;
    rb_log("punt: firing the game's punt");
    fire_punt(self, other);
    if (g_done_ball) { rb_log("punt: the play wasn't torn down by the punt - holding the ball, alarm 0 left alone"); return; }
    // The play is gone now. A play-over timer the game queued during the flight (v99: the ball was set down,
    // alarm 0 then ran on the deleted play and crashed the game) must not run. A normal punt never uses it.
    static void *f_get, *f_set;
    if (!f_get) { f_get = rb_builtin("alarm_get"); f_set = rb_builtin("alarm_set"); }
    double a0[1] = { 0 }, cancel[2] = { 0, -1 };
    double was = f_get ? rb_call(f_get, self, 1, a0) : -99;
    if (f_set) rb_call(f_set, self, 2, cancel);
    rb_log("punt: controller alarm 0 was %g - %s", was, f_set ? "cancelled" : "alarm_set NOT FOUND");
}

// ---- kick power bar while in kick mode
void rb_punt_gui(CInstance *self) {
    if (!P.kick || P.flight || !P.ball) return;
    static void *f_col, *f_rect, *f_gw, *f_gh;
    if (!f_col) { f_col = rb_builtin("draw_set_colour"); f_rect = rb_builtin("draw_rectangle"); f_gw = rb_builtin("display_get_gui_width"); f_gh = rb_builtin("display_get_gui_height"); }
    if (!f_col || !f_rect || !f_gw || !f_gh) return;
    double gw = rb_call(f_gw, self, 0, 0), gh = rb_call(f_gh, self, 0, 0);
    if (gw < 10 || gh < 10) return;
    double p = P.pow / FULL_DRAG;
    if (p < 0) p = 0;
    if (p > 1) p = 1;
    double w = gw * 0.4, x0 = (gw - w) / 2, y0 = gh - 26, y1 = gh - 12;
    double black[1] = { 0 }, fill[1] = { p < SHANK_POWER ? 255 : 65280 }, white[1] = { 16777215 };   // BGR: red when weak, else green
    double r1[5] = { x0 - 2, y0 - 2, x0 + w + 2, y1 + 2, 0 }, r2[5] = { x0, y0, x0 + w * p, y1, 0 };
    rb_call(f_col, self, 1, black); rb_call(f_rect, self, 5, r1);
    if (p > 0) { rb_call(f_col, self, 1, fill); rb_call(f_rect, self, 5, r2); }
    rb_call(f_col, self, 1, white);
}
