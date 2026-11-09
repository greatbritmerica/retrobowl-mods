// dbrain.c - the CPU defense learns the player's offense (Extreme only), kept in Documents/rbmod_dbrain.txt.
//  Watches: where the player throws (3 depths x 3 sides), how much they gain there, run rate and yards.
//  Adapts:  defenders near a throw into a familiar zone break on the ball sooner and from further away;
//           if the player keeps running for big gains, nearby defenders flow to the carrier harder.
#include "rb.h"
#include "sys.h"
#include "dbrain.h"

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
    // and it never reaches the extremes (0.25 - 0.85).
    g_rate[k] += ((success ? 1.0 : 0.0) - g_rate[k]) * 0.12;
    double target = 0.55 + (g_rate[k] - 0.5) * 1.2;
    if (target < 0.25) target = 0.25;
    if (target > 0.85) target = 0.85;
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
        double D = rb_dbrain_pass_dial();                               // balance: aim for ~50% of passes stopped
        double radius = (80 + 110 * f) * (0.3 + 0.9 * D);              // reads from further away
        unsigned delay = (unsigned)((12 - 8 * f) * (1.6 - D));          // and sooner
        if (P.rcv && P.t - P.t_throw >= delay && hypot(P.lx - x, P.ly - y) < radius) {
            double rx = rb_get_builtin_real(P.rcv, SLOT_X), ry = rb_get_builtin_real(P.rcv, SLOT_Y);
            if (rx > -9000) {                                           // jump the route: go to the receiver
                rb_set_inst_real(player, SLOT_JDIR, dir_to(x, y, rx, ry));
                rb_set_inst_real(player, SLOT_JFORCE, 1.0);
            }
        }
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
