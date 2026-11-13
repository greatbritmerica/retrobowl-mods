// ocbrain.c - see ocbrain.h.
#include "sys.h"
#include "ocbrain.h"

// Where each route ends: depth bucket as dbrain.c's zones (0 under 8 yds, 1 under 18), and how it moves
// sideways: +1 toward the middle, -1 toward the sideline. Shapes from ROUTES[] in offtools.c:
// hitch 5 yds, slant 5 then inside, dig 9 square in, curl 11 back to 8, post 9 to 12 inside, corner 9 to 12 out.
static const int DEPTH[6] = { 0, 0, 1, 1, 1, 1 };
static const int SHIFT[6] = { 0, 1, 1, 0, 1, -1 };

int oc_situation(double togo) { return togo <= 3 ? 0 : togo <= 7 ? 1 : 2; }

int oc_zone(int route, int side) {
    if (route < 0 || route >= 6) return -1;
    int s = side;
    if (SHIFT[route] > 0 && s != 1) s = 1;                   // breaks inside
    else if (SHIFT[route] < 0 && s == 1) s = 2;              // a middle receiver's corner: pick a side
    return DEPTH[route] * 3 + s;
}

int oc_pick(const OcBrain *b, int pos, int side, int sit, const OcCtx *c, const int used[7],
            double (*rnd)(void), int *why) {
    int allowed[6], n = 0;
    for (int pass = 0; pass < 3 && !n; pass++)                // relax: first no repeats + no short on long
        for (int r = 0; r < 6; r++) {
            if (pass < 1 && used[r]) continue;
            if (pass < 2 && sit == 2 && DEPTH[r] == 0) continue;
            allowed[n++] = r;
        }
    double x = rnd();
    if (x < c->keep) { *why = OC_WHY_KEEP; return OC_KEEP; }
    if (x < c->keep + c->surprise) {
        *why = OC_WHY_SURPRISE;
        int k = (int)(rnd() * n); return allowed[k < n ? k : n - 1];
    }
    int best = allowed[0]; double bs = -1e9, mean_pen = 0, pen[6], best_pen = 0;
    for (int i = 0; i < n; i++) {
        int r = allowed[i], z = oc_zone(r, side);
        pen[i] = 0.5 * c->counter * (z >= 0 ? c->fam[z] : 0);
        mean_pen += pen[i] / n;
        double s = (b->ok[r][pos][sit] + 1) / (b->att[r][pos][sit] + 2) - pen[i];
        if (s > bs) { bs = s; best = r; best_pen = pen[i]; }
    }
    // the reason is whichever term lifted the winner most: its own record, or the defense not knowing its zone
    double gain_ok = (b->ok[best][pos][sit] + 1) / (b->att[best][pos][sit] + 2) - 0.5, gain_fam = mean_pen - best_pen;
    if (b->att[best][pos][sit] >= 2 && gain_ok > 0 && gain_ok >= gain_fam) *why = OC_WHY_WORKS;
    else *why = gain_fam > 1e-9 ? OC_WHY_UNSEEN : OC_WHY_NEW;
    return best;
}

void oc_learn(OcBrain *b, int route, int pos, int sit, int ok, double yds) {
    if (route < 0 || route >= 7 || pos < 0 || pos > 2 || sit < 0 || sit > 2) return;
    b->att[route][pos][sit] += 1; b->ok[route][pos][sit] += ok ? 1 : 0; b->yds[route][pos][sit] += yds;
}

void oc_fade(OcBrain *b, double keep) {
    double *v = &b->att[0][0][0];
    for (size_t i = 0; i < sizeof *b / sizeof *v; i++) v[i] *= keep;
}

int oc_save(const OcBrain *b, FILE *f) {
    if (!f || fprintf(f, "ocbrain 1\n") < 0) return 0;
    for (int r = 0; r < 7; r++) for (int p = 0; p < 3; p++) for (int s = 0; s < 3; s++)
        if (fprintf(f, "%.4f %.4f %.4f\n", b->att[r][p][s], b->ok[r][p][s], b->yds[r][p][s]) < 0) return 0;
    return 1;
}

int oc_load(OcBrain *b, FILE *f) {
    char line[128];
    memset(b, 0, sizeof *b);
    if (!f || !fgets(line, sizeof line, f) || strncmp(line, "ocbrain 1", 9) || (line[9] && line[9] != '\n' && line[9] != '\r')) return 0;
    OcBrain t;
    for (int r = 0; r < 7; r++) for (int p = 0; p < 3; p++) for (int s = 0; s < 3; s++) {
        double *a = &t.att[r][p][s], *o = &t.ok[r][p][s], *y = &t.yds[r][p][s];
        if (!fgets(line, sizeof line, f) || sscanf(line, "%lf %lf %lf", a, o, y) != 3) return 0;
        if (!(*a >= 0 && *a < 1e9 && *o >= 0 && *o <= *a && *y > -1e9 && *y < 1e9)) return 0;   // also rejects nan
    }
    *b = t;
    return 1;
}

void oc_table_reset(OcTable *t) { memset(t, 0, sizeof *t); }

int oc_table_find(const OcTable *t, const void *who) {
    if (!who) return -1;
    for (int i = 0; i < t->n; i++) if (t->who[i] == who) return i;
    return -1;
}

void oc_table_set(OcTable *t, const void *who, int route, int pos) {
    int i = oc_table_find(t, who);
    if (i < 0) { if (!who || t->n >= 6) return; i = t->n++; t->who[i] = who; }
    t->route[i] = route; t->pos[i] = pos;
}
