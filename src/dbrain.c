// dbrain.c - the CPU defense learns the player's offense (Extreme only), kept in Documents/rbmod_dbrain.txt.
//  Watches: where the player throws (3 depths x 3 sides), how much they gain there, run rate and yards.
//  Adapts:  defenders near a throw into a familiar zone break on the ball sooner and from further away;
//           if the player keeps running for big gains, nearby defenders flow to the carrier harder.
#include "rb.h"
#include "sys.h"
#include "dbrain.h"
#include "offtools.h"

#define DRAG_DECAY 0.986
#define AIR_K      8.75
#define PX_PER_YD  20.5

typedef struct { double zone[9], zone_yds[9], run, run_yds; int plays, passes, runs; } DBrain;
static DBrain B;
static int g_loaded;

static struct {
    CInstance *ball;
    int extreme, kind;              // kind: 0 none, 1 pass, 2 run
    double los, fwd, qby, prev_st;
    double lx, ly, bx0, by0, down, togo; int zone, scored, pend; unsigned t, t_throw;
    CInstance *rcv;                 // intended receiver (offense player nearest the landing spot)
    int tip, tipped, whiff_logged;      // not an undercut: the defender in the way bats it down (or misses it)
    int play; double jdir; unsigned t_catch;    // this throw: PLAY_*; the going defender's heading; when it was caught
    int contest_done, broken;           // contested catch: rolled once per throw; knocked away
    int breakup;                        // this throw: the closing defender plays the ball (incompletion)
    double close;                       // how late the closing defender is this throw (0-1)
    int undercut;                       // this throw: the reader gets in front of it (pick chance) vs arrives at the catch
    double late;                        // play-action bite: the secondary reads the throw late
    CInstance *jumper;                  // the one defender reading the throw this play
} P;

static void path(char *b, int n) { const char *h = getenv("HOME"); snprintf(b, n, "%s/Documents/rbmod_dbrain.txt", h ? h : "."); }

static void load(void) {
    if (g_loaded) return;
    g_loaded = 1;
    char p[1024]; path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) { rb_log("dbrain: new (no file yet)"); return; }
    DBrain b = B; char line[512];
    if (fgets(line, sizeof line, f)) {
        int n = sscanf(line, "%lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %d %d %d",
            &b.zone[0], &b.zone[1], &b.zone[2], &b.zone[3], &b.zone[4], &b.zone[5], &b.zone[6], &b.zone[7], &b.zone[8],
            &b.zone_yds[0], &b.zone_yds[1], &b.zone_yds[2], &b.zone_yds[3], &b.zone_yds[4], &b.zone_yds[5],
            &b.zone_yds[6], &b.zone_yds[7], &b.zone_yds[8], &b.run, &b.run_yds, &b.plays, &b.passes, &b.runs);
        if (n == 23) B = b;
        double others = 0;
        for (int i = 0; i < 9; i++) if (i != 1) others += B.zone[i];
        if (B.passes > 0 && others == 0) {                    // file from the zone bug (every throw read as zone 1)
            for (int i = 0; i < 9; i++) { B.zone[i] = 0; B.zone_yds[i] = 0; }
            B.plays -= B.passes; B.passes = 0;
            rb_log("dbrain: cleared pass-zone data recorded by the zone bug (run data kept)");
        }
    }
    fclose(f);
    rb_log("dbrain: loaded %d plays (%d passes, %d runs), run share %.2f avg %.1f yds", B.plays, B.passes, B.runs, B.run, B.run_yds);
}

static void save(void) {
    char p[1024]; path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < 9; i++) fprintf(f, "%.4f ", B.zone[i]);
    for (int i = 0; i < 9; i++) fprintf(f, "%.3f ", B.zone_yds[i]);
    fprintf(f, "%.4f %.3f %d %d %d\n", B.run, B.run_yds, B.plays, B.passes, B.runs);
    fclose(f);
}

// how well the defense knows a zone: 0..1 (share of recent throws, scaled by experience)
static double familiarity(int z) {
    if (z < 0) return 0;
    double exp_ = B.passes < 30 ? B.passes / 30.0 : 1.0;
    double share = B.zone[z] * 3.0;                          // 1/3 of throws in one zone = fully familiar
    return (share > 1 ? 1 : share) * exp_;
}

// ---- balance governor: steer the CPU defense toward stopping about half of the player's passes.
// rate = recent share of the player's passes that gain yards; the dial (0.1 - 1) sets how hard the
// defense breaks on throws, how far it reads, and (hooks.c) how much acceleration it gets.
// Two dials, one per kind of play: your run game can't hold pass coverage at max (and vice versa).
//   pass dial -> how far/fast defenders jump your routes;  run dial -> how hard they pursue runs.
// Acceleration (hooks.c) follows their average.
static double g_rate[2] = { 0.5, 0.5 }, g_dial[2] = { 0.5, 0.6 };   // [0] pass, [1] run
static int g_bal_loaded;
static void bal_path(char *fp, size_t n) {
    const char *h = getenv("HOME"); snprintf(fp, n, "%s/Documents/rbmod_balance4.txt", h ? h : ".");
}
static void bal_load(void) {
    if (g_bal_loaded) return;
    g_bal_loaded = 1;
    char fp[1024]; bal_path(fp, sizeof fp);
    FILE *f = fopen(fp, "r"); if (!f) return;
    char line[160]; double r0, d0, r1, d1;
    if (fgets(line, sizeof line, f) && sscanf(line, "%lf %lf %lf %lf", &r0, &d0, &r1, &d1) == 4 &&
        d0 >= 0.1 && d0 <= 1.0 && d1 >= 0.1 && d1 <= 1.0) { g_rate[0] = r0; g_dial[0] = d0; g_rate[1] = r1; g_dial[1] = d1; }
    fclose(f);
    rb_log("balance: loaded - passes %.0f%% (dial %.2f), runs %.0f%% (dial %.2f)", g_rate[0] * 100, g_dial[0], g_rate[1] * 100, g_dial[1]);
}
static void bal_save(void) {
    char fp[1024]; bal_path(fp, sizeof fp);
    FILE *f = fopen(fp, "w"); if (!f) return;
    fprintf(f, "%.4f %.4f %.4f %.4f\n", g_rate[0], g_dial[0], g_rate[1], g_dial[1]); fclose(f);
}
double rb_dbrain_pass_dial(void) { bal_load(); return g_dial[0]; }
double rb_dbrain_run_dial(void)  { bal_load(); return g_dial[1]; }
double rb_dbrain_dial(void)      { bal_load(); return (g_dial[0] + g_dial[1]) / 2; }
static void bal_play(int success, const char *kind, double yds, double down, double togo) {
    bal_load();
    int k = kind[0] == 'r';
    // The dial follows the recent success rate directly (no running total), so it can't wind itself up
    // to the max on a hot streak and stay there: 50% -> middle, each 10% above/below moves it 0.12,
    // and it never drops below 0.25.
    g_rate[k] += ((success ? 1.0 : 0.0) - g_rate[k]) * 0.12;
    double target = 0.55 + (g_rate[k] - 0.5) * 1.2;
    if (target < 0.25) target = 0.25;
    // Pass coverage may go to 1.0: at 0.85 it sat on the ceiling for two straight games while 65% of
    // passes still worked. (Picks no longer depend on this dial, so a high value means tight coverage,
    // not an interception spree.) The run side keeps 0.85.
    double top = k == 0 ? 1.0 : 0.85;
    if (target > top) target = top;
    g_dial[k] += (target - g_dial[k]) * 0.35;
    rb_log("balance: player %s %.1f yds on %s & %.0f %s -> %s success %.0f%%, %s dial %.2f", kind, yds,
           down <= 1 ? "1st" : down == 2 ? "2nd" : down == 3 ? "3rd" : "4th", togo,
           success ? "worked" : "stopped", kind, g_rate[k] * 100, kind, g_dial[k]);
    bal_save();
}

static void learn_pass(int z, double yds) {
    for (int i = 0; i < 9; i++) B.zone[i] *= 0.95;           // recent throws count most
    B.zone[z] += 0.05;
    B.zone_yds[z] += (yds - B.zone_yds[z]) * 0.15;
    B.run *= 0.95; B.plays++; B.passes++;
    rb_log("dbrain: pass to zone %d (%s %s) %.1f yds -> familiarity %.2f", z, (const char *[]){"short", "mid", "deep"}[z / 3],
           (const char *[]){"left", "middle", "right"}[z % 3], yds, familiarity(z));
    save();
}

static void learn_run(double yds) {
    for (int i = 0; i < 9; i++) B.zone[i] *= 0.95;
    B.run = B.run * 0.95 + 0.05;
    B.run_yds += (yds - B.run_yds) * 0.15;
    B.plays++; B.runs++;
    rb_log("dbrain: run %.1f yds -> run share %.2f, avg %.1f", yds, B.run, B.run_yds);
    save();
}

// 0..1: the player is living off the run
static double run_alert(void) {
    if (B.runs < 8) return 0;
    double a = (B.run - 0.35) * 3.0 * ((B.run_yds - 4.0) / 3.0);
    return a < 0 ? 0 : a > 1 ? 1 : a;
}

static int qb_cb(CInstance *inst, void *ctx) { *(CInstance **)ctx = inst; return 1; }
typedef struct { double sx; int n; } MeanCtx;
typedef struct { double x, y, bd; CInstance *best; } NearCtx;
static int near_cb(CInstance *inst, void *ctx) {
    NearCtx *c = ctx;
    double d = hypot(rb_get_builtin_real(inst, SLOT_X) - c->x, rb_get_builtin_real(inst, SLOT_Y) - c->y);
    if (d < c->bd) { c->bd = d; c->best = inst; }
    return 0;
}
static int mean_cb(CInstance *inst, void *ctx) { MeanCtx *m = ctx; m->sx += rb_get_builtin_real(inst, SLOT_X); m->n++; return 0; }

void rb_dlearn_step(CInstance *ball, CInstance *other, int human_offense) {
    if (!human_offense) { P.ball = 0; return; }
    double st = rb_get_inst_real(ball, SLOT_STATE);
    if (ball != P.ball) {
        load();
        memset(&P, 0, sizeof P);
        P.ball = ball; P.zone = -1; P.prev_st = st;
        P.extreme = rb_get_obj_real(OBJ_MASTER, SLOT_OP_DIFFICULTY) == 4;
    }
    if (!P.extreme) return;
    P.t++;
    if (!P.fwd && st == 0) {                                    // orient on the snap set-up
        RValue *h = rb_inst_var(ball, SLOT_HOLDER);
        CInstance *qb = 0;
        if (h && h->kind != RV_UNDEFINED) { RValue c = *h; rb_with(&c, ball, other, qb_cb, &qb); }
        if (!qb) return;
        double qx = rb_get_builtin_real(qb, SLOT_X);
        MeanCtx m = { 0, 0 };
        RValue r = rb_obj_ref(OBJ_PLAYER_DF);
        rb_with(&r, ball, other, mean_cb, &m);
        if (!m.n) return;
        P.fwd = (m.sx / m.n < qx) ? -1 : 1;
        P.los = qx + P.fwd * 35;
        // (That offset is only right under centre. In the shotgun the QB is ~100 px back, which credited
        // every play ~3 extra yards; use the line the offense tools found from the real front.)
        double real = rb_off_los();
        if (real > -1e8) P.los = real;
        P.qby = rb_get_builtin_real(qb, SLOT_Y);
        P.down = rb_get_obj_real(OBJ_CONTROLLER, SLOT_DOWN);
        P.togo = rb_get_obj_real(OBJ_CONTROLLER, SLOT_YARDS);
    }
    if (P.pend && st == 3) {                                    // thrown: predict where it lands once it has speed
        // direction from the ball's actual movement (the stored direction can point back along the
        // slingshot drag), range from its speed: v^2-ish flight with drag, minus what it already covered
        double v = rb_get_inst_real(ball, SLOT_VELOCITY);
        double bx = rb_get_builtin_real(ball, SLOT_X), by = rb_get_builtin_real(ball, SLOT_Y);
        double mx = bx - P.bx0, my = by - P.by0, moved = hypot(mx, my);
        if (v > 0.5 && moved > 3) {
            double R = v * (1.0 - pow(DRAG_DECAY, AIR_K * v)) / (1.0 - DRAG_DECAY);
            P.lx = bx + mx / moved * R; P.ly = by + my / moved * R;
            double depth = (P.lx - P.los) * P.fwd / PX_PER_YD, side = (P.ly - P.qby) * (P.fwd > 0 ? 1 : -1);
            P.zone = (depth < 8 ? 0 : depth < 18 ? 1 : 2) * 3 + (side < -60 ? 0 : side > 60 ? 2 : 1);
            NearCtx c = { P.lx, P.ly, 1e9, 0 };
            RValue r = rb_obj_ref(OBJ_PLAYER_OF);
            rb_with(&r, ball, other, near_cb, &c);
            P.rcv = c.bd < 120 ? c.best : 0;
            P.pend = 0;
            rb_log("dbrain: throw lands ~%.0f yds deep, %s -> zone %d, receiver %s", depth,
                   side < -60 ? "left" : side > 60 ? "right" : "middle", P.zone, P.rcv ? "found" : "none");
        }
    }
    if (st == P.prev_st || !P.fwd) return;
    double bx = rb_get_builtin_real(ball, SLOT_X), by = rb_get_builtin_real(ball, SLOT_Y);
    if (P.prev_st == 2 && st == 3) {                            // thrown: predict where it lands
        P.bx0 = bx; P.by0 = by;
        P.kind = 1; P.t_throw = P.t; P.pend = 1; P.zone = -1; P.rcv = 0;
        P.late = rb_off_pa_bite(); P.undercut = -1; P.jumper = 0; P.tip = P.tipped = P.whiff_logged = 0; P.breakup = 0; P.contest_done = P.broken = 0;
    } else if (P.prev_st == 3 && st == 5 && P.kind == 1) { P.t_catch = P.t;  // caught
    } else if (st == 19 || (P.prev_st == 2 && st == 5)) P.kind = 2;         // handoff or QB run
    else if (!P.scored && (st == 4 || st == 6 || st == 7 || st == 9)) {
        P.scored = 1;
        double yds = (st == 7 || st == 9) ? 0 : (bx - P.los) * P.fwd / PX_PER_YD;
        if (P.kind == 1 && P.zone >= 0) learn_pass(P.zone, st == 9 ? -10 : yds);
        // a play "works" the way football counts it: 40% of the distance on 1st down, 60% on 2nd,
        // all of it on 3rd/4th (or a touchdown). A 2-yard catch on 3rd and 8 is a stop.
        if ((P.kind == 1 || P.kind == 2) && P.extreme) {
            double togo = P.togo > 0 && P.togo < 99 ? P.togo : 10;
            double need = P.down <= 1 ? 0.4 * togo : P.down == 2 ? 0.6 * togo : togo;
            int ok = st == 6 || (st == 4 && yds > 0 && yds >= need - 0.05);
            bal_play(ok, P.kind == 1 ? "pass" : "run", yds, P.down, togo);
        }
        if (P.kind == 2) learn_run(yds);
    }
    P.prev_st = st;
}

static double dir_to(double fx, double fy, double tx, double ty) {
    double a = atan2(-(ty - fy), tx - fx) * 57.29577951308232;
    return a < 0 ? a + 360 : a;
}

#define SLOT_BALLDIST   0x100fa1d78ULL     // player: distance to the ball (the game's own catch check uses <= 20)
#define SLOT_HEIGHT     0x100fa5c18ULL     // ball: height off the ground
#define VA_S_DROP_BALL  0x100395780ULL     // gml_Script_s_drop_ball(false), run as the player: the game's own tipped pass

#define WHIFF_FRAMES     20      // after a miss the defender overruns the catch this long (1/3 s)
#define CONTEST_REACH    28.0    // defender this close to the receiver at the catch: contested

// One decision per throw. Most reads end with the defender arriving AT the catch (a hit, a breakup, a
// short gain); getting in front of the ball for a pick takes a well-known route and some luck.
// What the defender who goes for the ball does with it, decided once per throw. He ALWAYS goes for it;
// the difference is how it turns out.
enum { PLAY_PICK, PLAY_WHIFF, PLAY_BREAKUP, PLAY_HIT };
static const char *PLAY_NAME[] = { "PICK ATTEMPT - gets in front of it", "goes for it and whiffs", "gets a hand in (breakup)", "arrives with the ball (hit at the catch)" };
static void decide_read(void) {
    if (P.undercut >= 0) return;
    double f = P.zone >= 0 ? familiarity(P.zone) : 0;
    if (f > 0.85) f = 0.85;
    double p = (0.12 + 0.33 * f) * (1.0 - P.late);                      // a pick takes a well-known route and some luck
    // Otherwise he misses about half the time (a bit less against tight coverage, more against loose),
    // and when he doesn't miss it is a breakup 4 times in 10, a hit on the receiver the rest.
    double whiff = 0.70 - 0.35 * rb_dbrain_pass_dial();
    if (whiff < 0.35) whiff = 0.35;
    if (whiff > 0.65) whiff = 0.65;
    whiff += 0.25 * P.late;                                             // fooled by play action: misses more
    double u = arc4random_uniform(10000) / 10000.0;
    if (u < p) P.play = PLAY_PICK;
    else {
        u = arc4random_uniform(10000) / 10000.0;
        P.play = u < whiff ? PLAY_WHIFF : arc4random_uniform(100) < 40 ? PLAY_BREAKUP : PLAY_HIT;
    }
    P.undercut = P.play == PLAY_PICK;
    P.tip = P.play == PLAY_BREAKUP;
    rb_log("dbrain: read on this throw - %s (pick chance %.0f%%, whiff chance %.0f%%, familiarity %.2f%s)",
           PLAY_NAME[P.play], p * 100, whiff * 100, f, P.late > 0 ? ", late from play action" : "");
}

// Before the game's own ball-contact check runs for a player. The base game hands any defender who is
// in the ball's path the interception; here that only happens on the throws decide_read() gave to the
// defense. On the rest the defender tips it (the game's own tipped-pass routine) or just misses.
// Returns 1 when the game's check must be skipped for this player this frame.
void rb_dbrain_forget(void) { P.ball = 0; P.rcv = 0; P.jumper = 0; P.kind = 0; }

int rb_dbrain_ball_contact(CInstance *player, CInstance *other) {
    if (!P.ball || !P.extreme || P.kind != 1) return 0;
    if (rb_get_inst_real(P.ball, SLOT_STATE) != 3) return 0;
    if (rb_get_obj_real(OBJ_CONTROLLER, SLOT_KICKING) > 0.5) return 0;  // field goals and punts: untouched
    if (rb_get_inst_real(player, SLOT_OFFENSE) > 0.5) {
        // A receiver about to make the catch. If a defender is physically on him at that moment, the
        // catch is contested: some of those are broken up (the defender gets a hand in, the game's own
        // tipped-pass routine). Decided once per throw, from where the players really are - planning a
        // breakup at the throw didn't work, the defender was hardly ever actually there.
        if (P.broken) return 1;                                         // already knocked away: nobody on offense catches it
        if (rb_get_inst_real(player, SLOT_BALLDIST) > 20) return 0;
        double bx = rb_get_builtin_real(P.ball, SLOT_X), by = rb_get_builtin_real(P.ball, SLOT_Y);
        double trav = hypot(bx - P.bx0, by - P.by0);
        if (trav < 50 || (P.zone >= 0 && trav < 0.5 * hypot(P.lx - P.bx0, P.ly - P.by0))) return 0;
        if (P.contest_done) return 0;
        P.contest_done = 1;
        decide_read();
        NearCtx n = { rb_get_builtin_real(player, SLOT_X), rb_get_builtin_real(player, SLOT_Y), 1e9, 0 };
        RValue r = rb_obj_ref(OBJ_PLAYER_DF);
        rb_with(&r, player, other, near_cb, &n);
        if (!n.best || n.bd > CONTEST_REACH) { rb_log("dbrain: open catch (nearest defender %.0f px away)", n.bd); return 0; }
        int broke = P.play == PLAY_BREAKUP && !P.tipped;
        rb_log("dbrain: contested catch (defender %.0f px away) - %s", n.bd,
               broke ? "BROKEN UP" : P.play == PLAY_WHIFF ? "defender whiffs, receiver has it" : "receiver holds on");
        if (!broke) return 0;
        P.broken = P.tipped = 1;
        RValue no = {0}; no.kind = RV_BOOL;
        RValue *av[1] = { &no };
        RValue res = {0}; res.kind = RV_UNDEFINED;
        ((GMLScript)RB(VA_S_DROP_BALL))(n.best, other, &res, 1, av);
        return 1;
    }
    if (rb_get_inst_real(player, SLOT_BALLDIST) > 20) return 0;         // not in reach: the game does nothing either
    double bx = rb_get_builtin_real(P.ball, SLOT_X), by = rb_get_builtin_real(P.ball, SLOT_Y);
    double trav = hypot(bx - P.bx0, by - P.by0);
    if (trav < 50) return 0;                                            // at the line: the game's own batted-ball rules
    if (P.zone >= 0 && trav < 0.4 * hypot(P.lx - P.bx0, P.ly - P.by0)) return 0;
    int at_catch = P.zone < 0 || trav >= 0.5 * hypot(P.lx - P.bx0, P.ly - P.by0);   // breakups happen at the catch, not at linebacker depth
    decide_read();
    if (P.undercut) return 0;                                           // this one can be picked: the game decides
    double h = rb_get_inst_real(P.ball, SLOT_HEIGHT);
    if (P.tip && !P.tipped && at_catch && h <= 45) {                                // within a leaping defender's reach (the game lets a jumping player catch up to 60)
        P.tipped = 1;
        RValue no = {0}; no.kind = RV_BOOL;
        RValue *av[1] = { &no };
        RValue res = {0}; res.kind = RV_UNDEFINED;
        ((GMLScript)RB(VA_S_DROP_BALL))(player, other, &res, 1, av);
        rb_log("dbrain: pass batted down by the defender in its path (ball height %.0f, %.0f px downfield)", h, trav);
    } else if (!P.tipped && !P.whiff_logged) {
        P.whiff_logged = 1;
        rb_log("dbrain: defender in the path doesn't get a hand on it (ball height %.0f)", h);
    }
    return 1;
}

// after the game's AI has steered a defender on the player's drive
void rb_dlearn_after_ai(CInstance *player) {
    if (!P.ball || !P.extreme || !P.fwd) return;
    if (!rb_obj_exists(OBJ_PLAYER_DF)) return;
    double st = P.prev_st;
    double x = rb_get_builtin_real(player, SLOT_X), y = rb_get_builtin_real(player, SLOT_Y);
    if (x < -9000) return;
    if (rb_get_inst_real(player, SLOT_OFFENSE) > 0.5) return;           // defenders only
    if (st == 3 && P.kind == 1 && P.zone >= 0) {                       // ball in the air (landing known)
        double f = familiarity(P.zone);
        if (f > 0.85) f = 0.85;                                         // no defense reads a route perfectly
        double D = rb_dbrain_pass_dial();
        // The defender nearest to where the ball is coming down always turns and goes for it (it used to
        // depend on how close he started, and on most throws he trailed to a spot behind the receiver,
        // which read as a defender ignoring the ball). What the balance moves now is how soon he reacts
        // and how often he comes up empty.
        double radius = 230 * (1.0 - 0.55 * P.late);
        unsigned delay = (unsigned)((12 - 8 * f) * (1.6 - D)) + (unsigned)(34 * P.late);
        decide_read();
        double dist = hypot(P.lx - x, P.ly - y);
        if (P.rcv && P.t - P.t_throw >= delay && dist < radius) {
            if (P.jumper && P.jumper != player) {                       // one man goes: the closest
                double jx = rb_get_builtin_real(P.jumper, SLOT_X), jy = rb_get_builtin_real(P.jumper, SLOT_Y);
                if (jx > -9000 && hypot(P.lx - jx, P.ly - jy) <= dist) return;
            }
            P.jumper = player;
            double rx = rb_get_builtin_real(P.rcv, SLOT_X), ry = rb_get_builtin_real(P.rcv, SLOT_Y);
            if (rx > -9000) {
                // at the ball: a step in front of the receiver, in the ball's path. Only the man who is
                // going to arrive with it (a hit) aims just behind the receiver instead.
                double fx = P.lx - P.bx0, fy = P.ly - P.by0, fl = hypot(fx, fy);
                double lag = P.play == PLAY_HIT ? 12 : -8;
                double tx = rx, ty = ry;
                if (fl > 1) {
                    tx += fx / fl * lag; ty += fy / fl * lag;
                    if (P.play == PLAY_WHIFF) {                         // the miss: he flashes across in front of the
                        double px = -fy / fl * 20, py = fx / fl * 20;   // receiver, a body-width off, and sails by
                        if (hypot(tx + px - x, ty + py - y) > hypot(tx - px - x, ty - py - y)) { px = -px; py = -py; }
                        tx += px; ty += py;
                    }
                }
                if (hypot(tx - x, ty - y) > 6) P.jdir = dir_to(x, y, tx, ty);   // (keep his heading once he's on the spot)
                rb_set_inst_real(player, SLOT_JDIR, P.jdir);
                rb_set_inst_real(player, SLOT_JFORCE, 1.0);
            }
        }
    } else if (st == 5 && P.kind == 1 && P.play == PLAY_WHIFF && player == P.jumper && P.t - P.t_catch < WHIFF_FRAMES) {
        // he went for the ball and missed: his momentum carries him past the receiver for a moment
        rb_set_inst_real(player, SLOT_JDIR, P.jdir);
        rb_set_inst_real(player, SLOT_JFORCE, 1.0);
    } else if (st == 5 && P.kind == 2) {                                // run: only if the player lives off it
        double a = run_alert() * (0.4 + 0.8 * rb_dbrain_run_dial());
        if (a <= 0) return;
        double bx = rb_get_builtin_real(P.ball, SLOT_X), by = rb_get_builtin_real(P.ball, SLOT_Y);
        if (hypot(bx - x, by - y) < 60 + 100 * a) {
            rb_set_inst_real(player, SLOT_JDIR, dir_to(x, y, bx, by));
            rb_set_inst_real(player, SLOT_JFORCE, 1.0);
        }
    }
}

double rb_dbrain_run_share(void) { load(); return B.run; }
