// brain.c - what the CPU offense learns, kept across games in Documents/rbmod_brain.txt.
// A few dials adjusted from results, not a neural network:
//   safe_open   clearance he wants before calling a man open (up after picks, down after tight completions)
//   hdef_margin extra room he gives the defender the PLAYER controls (up when that defender makes plays)
//   run/pass yards per attempt -> how often he runs
#include "rb.h"
#include "sys.h"
#include "brain.h"

Brain rb_brain = { 15.0, 20.0, 0, 0, 0, 0, 0, 0, 0 };
static int g_loaded;

static void path(char *buf, int n) {
    const char *home = getenv("HOME");
    snprintf(buf, n, "%s/Documents/rbmod_brain.txt", home ? home : ".");
}

void rb_brain_load(void) {
    if (g_loaded) return;
    g_loaded = 1;
    char p[1024]; path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) { rb_log("brain: new (no file yet)"); return; }
    char line[256];
    if (fgets(line, sizeof line, f)) {
        Brain b = rb_brain;
        int n = sscanf(line, "%lf %lf %lf %d %lf %d %d %d %d", &b.safe_open, &b.hdef_margin, &b.run_yds, &b.runs,
                       &b.pass_yds, &b.passes, &b.ints, &b.comps, &b.games);
        if (n >= 8) rb_brain = b;
    }
    fclose(f);
    rb_log("brain: loaded safe_open %.1f hdef %.1f runs %d (%.1f yds) passes %d (%.1f yds) ints %d comps %d",
           rb_brain.safe_open, rb_brain.hdef_margin, rb_brain.runs, rb_brain.run_yds, rb_brain.passes,
           rb_brain.pass_yds, rb_brain.ints, rb_brain.comps);
}

static void save(void) {
    char p[1024]; path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "%.3f %.3f %.3f %d %.3f %d %d %d %d\n", rb_brain.safe_open, rb_brain.hdef_margin, rb_brain.run_yds,
            rb_brain.runs, rb_brain.pass_yds, rb_brain.passes, rb_brain.ints, rb_brain.comps, rb_brain.games);
    fclose(f);
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

// running averages that favour recent games
static void avg(double *a, int *n, double v) { (*n)++; double w = *n < 40 ? 1.0 / *n : 1.0 / 40; *a += (v - *a) * w; }

void rb_brain_pass(int result, double yards, double open, double hdef_dist, int human_jumped) {
    switch (result) {
    case BR_INT:
        rb_brain.ints++;
        rb_brain.safe_open += 4;
        if (human_jumped || hdef_dist < 80) rb_brain.hdef_margin += 8;
        avg(&rb_brain.pass_yds, &rb_brain.passes, -15);          // a pick costs about a punt
        break;
    case BR_INCOMPLETE:
        rb_brain.safe_open += 0.5;
        avg(&rb_brain.pass_yds, &rb_brain.passes, 0);
        break;
    case BR_COMPLETE:
        rb_brain.comps++;
        if (open < rb_brain.safe_open + 10) rb_brain.safe_open -= 0.6;   // the tight one worked
        if (hdef_dist < 80) rb_brain.hdef_margin -= 1.0;                   // got it past the player
        avg(&rb_brain.pass_yds, &rb_brain.passes, yards);
        break;
    }
    rb_brain.safe_open = clampd(rb_brain.safe_open, 8, 40);
    rb_brain.hdef_margin = clampd(rb_brain.hdef_margin, 0, 80);
    rb_log("brain: pass %s %.1f yds (open %.0f, player's defender %.0f away%s) -> safe_open %.1f hdef %.1f",
           result == BR_INT ? "INTERCEPTED" : result == BR_INCOMPLETE ? "incomplete" : "complete", yards, open,
           hdef_dist, human_jumped ? ", jumped" : "", rb_brain.safe_open, rb_brain.hdef_margin);
    save();
}

void rb_brain_run(double yards) {
    avg(&rb_brain.run_yds, &rb_brain.runs, yards);
    rb_log("brain: run %.1f yds -> avg run %.1f (%d) vs pass %.1f (%d)", yards, rb_brain.run_yds, rb_brain.runs,
           rb_brain.pass_yds, rb_brain.passes);
    save();
}

double rb_brain_run_rate(void) {
    if (rb_brain.runs < 4 || rb_brain.passes < 4) return 0.38;
    return clampd(0.38 + (rb_brain.run_yds - rb_brain.pass_yds) * 0.04, 0.2, 0.6);
}
