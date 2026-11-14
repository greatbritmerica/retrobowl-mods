// punt.c - punts.
//
// YOUR PUNTS. On 4th down the mod's pre-snap panel has a Punt button (tapping Punt on the game's 4th-down
// question does the same): the QB (your punter) lines up PUNTER_DEPTH yards deep. Snap it and either run
// with him (a fake) or pull back to aim: on a punt play the pull-back IS the kick - its length is the power,
// its direction the aim. Let go and the ball flies (players freeze) for FLIGHT_SECONDS, then the game's own
// punt runs and the mod puts the other team where your punt landed.
//
// CPU PUNTS. When the CPU punts to you, the game's punt runs as always, then the mod turns the ball back
// over to the CPU and sends the commentary to its KICKOFF stage: the kickoff return you know starts. On its
// first frame everybody is moved so the catch comes where the punt lands (about PUNT_DIST yards from the
// CPU's line); at the catch the CPU's cover team is brought in to PUNT_COVER yards, as on a real punt.
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
#define PUNTER_DEPTH   T(T_PUNTER_DEPTH)     // yards your punter lines up behind the line
#define PUNT_MIN       T(T_PUNT_MIN)         // yards for the weakest pull-back
#define PUNT_MAX       T(T_PUNT_MAX)         // yards for a full pull-back
#define PUNT_AIM_MAX   T(T_PUNT_AIM_MAX)     // widest aim, degrees off straight downfield
#define FLIGHT_SECONDS T(T_PUNT_FLIGHT)      // seconds the punt flies on screen
#define SHANK_POWER    T(T_SHANK_POWER)      // pull-backs weaker than this (0-1) may shank
#define SHANK_CHANCE   T(T_SHANK_CHANCE)     // chance a weak one shanks (half distance)
#define PUNT_RETURN    T(T_PUNT_RETURN)      // 1 = run back the CPU's punts
#define PUNT_DIST      T(T_PUNT_DIST)        // average CPU punt, yards
#define PUNT_SPREAD    T(T_PUNT_SPREAD)      // CPU punts land up to this much shorter or longer
#define PUNT_COVER     T(T_PUNT_COVER)       // yards from your returner to the nearest cover man at the catch
#define FULL_DRAG      100.0                 // the game's pull-back length tops out at 100

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
    CInstance *ball, *qb;      // that play's ball, the punter
    double qb_x0;              // where the punter stood before he was moved deep (0 = not moved)
    int kick;                  // kick mode on (the pull-back is the punt)
    double pow, dir;           // the pull-back while held (length, aim in screen degrees)
    int flight; unsigned ft, fn;
    double sx, sy, lx, ly, peak;
    int firing;                // our own s_action_result call
    int snapped;               // the punt play has been snapped (it ends with this play)
} P;
static CInstance *g_done_ball;         // the ball your punt left from: the game doesn't step it again
static double g_spot = NO_SPOT;        // the other team's yard line to set after your punt
static void cb_none(int i) { (void)i; }   // the kick panel has no buttons

int rb_punt_play(void) { return P.armed && P.ball; }
int rb_punt_armed(void) { return P.armed; }

// the pre-snap panel's Punt button (your ball, 4th down, before the snap)
void rb_punt_arm(int on) {
    if (P.kick) rb_ui_panel_close(RB_PANEL_KICK);
    if (!on && P.qb && P.qb_x0) rb_set_builtin_real(P.qb, SLOT_X, P.qb_x0);   // punt off: QB back under centre
    memset(&P, 0, sizeof P);
    P.armed = on && PUNT_ON;
    rb_log("punt: Punt button - %s", P.armed ? "punt play on: pull back to aim and kick, or run it (a fake)" : "punt play off");
}

int rb_punt_action_result(double code) {
    if (P.firing) return PR_PASS;
    if (g_done_ball) { rb_log("punt: result %g after the punt ignored", code); return PR_SWALLOW; }
    if (P.flight) {                                   // something else ended the play mid-flight: it's still a punt
        rb_log("punt: play ended (result %g) during the flight - punting now", code);
        P.flight = 0; P.armed = 0;
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

typedef struct { CInstance *p; } HolderCtx;
static int holder_cb(CInstance *inst, void *ctx) { ((HolderCtx *)ctx)->p = inst; return 1; }
static CInstance *ball_holder(CInstance *ball, CInstance *other) {
    RValue *h = rb_inst_var(ball, SLOT_HOLDER);
    HolderCtx c = { 0 };
    if (h && h->kind != RV_UNDEFINED) { RValue v = *h; rb_with(&v, ball, other, holder_cb, &c); }
    return c.p;
}

static void fire_punt(CInstance *ball, CInstance *other) {
    RValue code = {0}, res = {0};
    code.kind = RV_REAL; code.real = ACTION_RESULT_PUNT; res.kind = RV_UNDEFINED;
    RValue *av[1] = { &code };
    P.firing = 1;
    ((GMLScript)RB(VA_S_ACTION_RESULT))(ball, other, &res, 1, av);
    P.firing = 0;
}

// the release: work out where it lands and start the flight
static void kick(CInstance *ball, CInstance *other) {
    rb_ui_panel_close(RB_PANEL_KICK);
    double fwd = rb_off_fwd(), los = rb_off_los(), scrim = rb_get_obj_real(OBJ_CONTROLLER, SLOT_SCRIMMAGE);
    double qx = rb_get_builtin_real(ball, SLOT_X), qy = rb_get_builtin_real(ball, SLOT_Y);
    if (!fwd || los < -1e8 || scrim < -9000 || qx < -9000) {
        rb_log("punt: FALLBACK - can't read the field (fwd %g, line %.0f, yard line %.1f) - the game's own punt", fwd, los, scrim);
        g_spot = NO_SPOT; P.armed = 0;
        fire_punt(ball, other);
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
    P.flight = 1; P.ft = 0;
    P.fn = (unsigned)(FLIGHT_SECONDS * 60);
    if (P.fn < 10) P.fn = 10;
    P.sx = qx; P.sy = qy; P.lx = pl.lx; P.ly = pl.ly;
    P.peak = yards * KM_PX_PER_YD * 0.25;
    if (P.peak > 120) P.peak = 120;
}

// ================================================================ CPU punts -> live returns
enum { R_IDLE, R_SEEN, R_CONVERT, R_KICK, R_AIR, R_LIVE };
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
        if (land_cpu >= 50) {
            rb_set_obj_real(OBJ_CONTROLLER, SLOT_SCRIMMAGE, -30);
            rb_log("punt return: %.0f-yd punt into the end zone - touchback, your ball at your 20 (the game had %.1f)", yards, R.game_spot);
            R.st = R_IDLE; return;
        }
        R.land = -land_cpu;
        if (g_stage_kickoff < 0) { rb_log("punt return: FALLBACK - no kickoff seen since the app started, so its stage isn't known: the game's punt stands"); R.st = R_IDLE; return; }
        if (clock < 5) { rb_log("punt return: FALLBACK - %.0f s left in the quarter: the game's punt stands", clock); R.st = R_IDLE; return; }
        R.st = R_CONVERT;
        rb_log("punt return: %.0f-yd punt to your %.0f (the game's spot was %.1f) - turning it into a return", yards, 50 + R.land, R.game_spot);
        return;
    }
    if ((R.st == R_KICK || R.st == R_AIR) && caller == CALLER_S_KICK_OFF && before != human && after == human) {   // no live return (kick returns off)
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
    if (R.st == R_KICK) {                              // no early frame came: move it all now
        shift_to_landing(ball, other);
        R.st = R_AIR;
    }
    double st = rb_get_inst_real(ball, SLOT_STATE);
    if (R.st == R_AIR) {
        if (st == 4) return;                           // still in the air
        cover_in(ball, other);
        R.st = R_LIVE; R.ball = ball; R.last_x = rb_get_builtin_real(ball, SLOT_X);
        rb_log("punt return: live (ball state %g)", st);
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
    } else if (R.st == R_AIR) { rb_log("punt return: play torn down before the catch"); R.st = R_IDLE; }
    g_done_ball = 0;
    if (P.flight) {                                    // torn down mid-flight (match quit): drop it all
        rb_log("punt: play torn down during the flight - punt dropped");
        rb_ui_panel_close(RB_PANEL_KICK);
        memset(&P, 0, sizeof P); g_spot = NO_SPOT;
    } else if (P.armed && P.snapped) {           // the punt play is over (a fake, or it ended before the kick)
        if (P.kick) rb_ui_panel_close(RB_PANEL_KICK);
        rb_log("punt: punt play over without a kick (a fake)");
        memset(&P, 0, sizeof P);
    } else { P.ball = 0; P.qb = 0; }                    // rebuilt before the snap: find the new ones
}

// ================================================================ per ball step
int rb_punt_ball(CInstance *ball, CInstance *other) {
    if (R.st == R_KICK || R.st == R_AIR || R.st == R_LIVE) return_step(ball, other);
    if (ball == g_done_ball) return 1;
    if (!P.armed) return 0;
    if (!P.flight && possession() != human_team()) {   // the ball changed hands before your punt play: it's off
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
        rb_set_inst_real(ball, SLOT_HEIGHT, 4 * P.peak * p * (1 - p));
        if (P.ft >= P.fn) {
            rb_log("punt: ball down - the game's punt runs now");
            P.flight = 0; P.armed = 0;
            g_done_ball = ball;                        // cleared when the play is torn down
            fire_punt(ball, other);
            if (g_done_ball) rb_log("punt: the play wasn't torn down by the punt (ball state %g) - holding it", rb_get_inst_real(ball, SLOT_STATE));
        }
        return 1;                                      // the game doesn't step the ball (no throw, no tackle of the ball)
    }
    if (st == 0) {                                     // before the snap: punter deep
        double fwd = rb_off_fwd(), los = rb_off_los();
        if (!P.qb) P.qb = ball_holder(ball, other);
        if (P.qb && fwd && los > -1e8) {
            double depth = PUNTER_DEPTH, room = rb_get_obj_real(OBJ_CONTROLLER, SLOT_SCRIMMAGE) + 58;   // stay in front of the end line
            if (depth > room) depth = room;
            if (depth < 1) depth = 1;
            if (!P.qb_x0) P.qb_x0 = rb_get_builtin_real(P.qb, SLOT_X);
            rb_set_builtin_real(P.qb, SLOT_X, los - fwd * depth * KM_PX_PER_YD);
            static CInstance *noted;
            if (noted != ball) { noted = ball; rb_log("punt: punter set %.0f yds deep at x %.0f", depth, los - fwd * depth * KM_PX_PER_YD); }
        }
        return 0;
    }
    if (!P.snapped) { P.snapped = 1; rb_log("punt: snapped"); }
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

void rb_punt_after_ai(CInstance *player) {
    // the kickoff's first AI frame (after the game placed the returner): move it all before the kick
    if (R.st == R_KICK && R.ret_x) { shift_to_landing(player, player); R.st = R_AIR; }
    if (P.flight) rb_set_inst_real(player, SLOT_JFORCE, 0);      // everybody holds while the ball is up
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
