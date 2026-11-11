// rules.c - the parts of the mod the player edits from the website instead of asking for a build:
//
//   Documents/rbmod_settings.txt   tuning values             "key = value" per line (see tools/tunables.json)
//   Documents/rbmod_rules.txt      rules: when / if / then   (format below)
//   Documents/rbmod_plays.txt      one line per play, written by the mod, read by the website's report
//
// Both input files are optional and re-read at the start of every play, so a change takes effect on the
// next snap with no reinstall. With neither file present the mod behaves exactly as it did before.
//
// Rules file:
//     rule <name>
//     when <event> [argument]              presnap snap throw catch incomplete intercepted sack touchdown
//                                          play_end button <label>
//     if <variable> <op> <number>          ops: = != < <= > >=      (every "if" must hold)
//     then tune <key> <=|+|*> <number> <play|keep>
//     then var <name> <=|+> <number>
//     then msg <text to show before the next snap>
//     then button <label>                  (in a presnap rule: adds a button; pressing it fires "button <label>")
//     then legs <+|=> <percent>            the back's stamina
//     then speed <group> <percent>         this play: qb back receivers line rushers coverage defense
//     then sneak | kneel | deep            (before the snap) QB keeps it and runs / takes a knee / every route goes long
//     then hailmary                        (before the snap) receivers race to the end zone, the throw carries there
//     then push <percent>                  line and back drive forward, defenders in the pile are held
//     then penalty <yards> <replay|next>   after the play: move the ball (+ is in your favor), replay the down or not
//     end
#include "rb.h"
#include "sys.h"
#include "tune.h"
#include "rules.h"
#include "tune_table.inc"

// ------------------------------------------------------------------------------------------- files
static void doc_path(char *fp, size_t n, const char *name) {
    const char *h = getenv("HOME");
    snprintf(fp, n, "%s/Documents/%s", h ? h : ".", name);
}
// whole (small) file into buf; returns length, or -1 when it does not exist
static int slurp(const char *name, char *buf, int cap) {
    char fp[1024]; doc_path(fp, sizeof fp, name);
    FILE *f = fopen(fp, "r");
    if (!f) return -1;
    int n = 0; char line[512];
    while (fgets(line, sizeof line, f)) {
        int l = (int)strlen(line);
        if (n + l >= cap - 1) break;
        memcpy(buf + n, line, (size_t)l); n += l;
    }
    buf[n] = 0;
    fclose(f);
    return n;
}
static unsigned hash(const char *s, int n) { unsigned h = 2166136261u; for (int i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u; return h; }
// next line of a buffer (trimmed of the line end and leading blanks) into out; returns the new position
static int next_line(const char *buf, int pos, char *out, int cap) {
    int i = 0;
    while (buf[pos] == ' ' || buf[pos] == '\t') pos++;
    while (buf[pos] && buf[pos] != '\n') { if (buf[pos] != '\r' && i < cap - 1) out[i++] = buf[pos]; pos++; }
    while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\t')) i--;
    out[i] = 0;
    return buf[pos] ? pos + 1 : pos;
}

// ------------------------------------------------------------------------------------------ tuning
static double g_base[T_COUNT], g_keep[T_COUNT], g_play[T_COUNT];
static char g_has_keep[T_COUNT], g_has_play[T_COUNT];
static int g_tune_init;
static unsigned g_set_hash = 1;                 // 1 = "no file seen yet"
static void tune_init(void) {
    if (g_tune_init) return;
    g_tune_init = 1;
    for (int i = 0; i < T_COUNT; i++) g_base[i] = RB_TUNE[i].def;
}
double T(TuneId id) {
    tune_init();
    double v = g_has_play[id] ? g_play[id] : g_has_keep[id] ? g_keep[id] : g_base[id];
    if (v < RB_TUNE[id].min) v = RB_TUNE[id].min;
    if (v > RB_TUNE[id].max) v = RB_TUNE[id].max;
    return v;
}
int rb_tune_find(const char *key) {
    for (int i = 0; i < T_COUNT; i++) if (!strcmp(RB_TUNE[i].key, key)) return i;
    return -1;
}
void rb_tune_apply(int id, char op, double v, int keep) {
    if (id < 0 || id >= T_COUNT) return;
    double cur = T((TuneId)id), nv = op == '+' ? cur + v : op == '*' ? cur * v : v;
    if (keep) { g_keep[id] = nv; g_has_keep[id] = 1; } else { g_play[id] = nv; g_has_play[id] = 1; }
}
void rb_tune_play_reset(void) { memset(g_has_play, 0, sizeof g_has_play); }
void rb_tune_load(void) {
    tune_init();
    static char buf[8192];
    int n = slurp("rbmod_settings.txt", buf, sizeof buf);
    unsigned h = n < 0 ? 0 : hash(buf, n) | 2;
    if (h == g_set_hash) return;
    g_set_hash = h;
    for (int i = 0; i < T_COUNT; i++) g_base[i] = RB_TUNE[i].def;
    memset(g_has_keep, 0, sizeof g_has_keep);
    if (n < 0) { rb_log("settings: no rbmod_settings.txt - built-in values"); return; }
    int pos = 0, set = 0, bad = 0; char line[200];
    while (buf[pos]) {
        pos = next_line(buf, pos, line, sizeof line);
        if (!line[0] || line[0] == '#') continue;
        char key[64]; double v;
        if (sscanf(line, "%63[A-Za-z0-9_] = %lf", key, &v) != 2 && sscanf(line, "%63[A-Za-z0-9_] %lf", key, &v) != 2) { bad++; continue; }
        int id = rb_tune_find(key);
        if (id < 0) { bad++; rb_log("settings: unknown setting '%s' ignored", key); continue; }
        if (v != g_base[id]) rb_log("settings: %s = %g (built-in %g)", key, v, RB_TUNE[id].def);
        g_base[id] = v; set++;
    }
    rb_log("settings: rbmod_settings.txt loaded - %d values%s", set, bad ? ", some lines ignored" : "");
}

// ------------------------------------------------------------------------------------------- rules
#define MAXR 64
#define MAXC 6
#define MAXA 8
#define MAXV 32
#define MAXB 3
typedef struct { char var[40]; int op; double val; } Cond;                 // op: 0 = 1 != 2 < 3 <= 4 > 5 >=
typedef struct { char type[16]; char a[40]; char op; double v; int keep; char text[72]; } Act;
typedef struct { char name[40], ev[16], arg[40]; Cond c[MAXC]; int nc; Act a[MAXA]; int na; } Rule;
static Rule g_rules[MAXR];
static int g_nrules;
static unsigned g_rule_hash = 1;
static struct { char name[32]; double v; } g_vars[MAXV];
static int g_nvars;
static char g_btn[MAXB][32]; static int g_nbtn; static char g_btn_pressed[MAXB];
static char g_msg[200], g_carry[200], g_both[404];
static int g_firing_presnap;
RuleCtx rb_rctx;

static double *var_slot(const char *name, int create) {
    for (int i = 0; i < g_nvars; i++) if (!strcmp(g_vars[i].name, name)) return &g_vars[i].v;
    if (!create || g_nvars >= MAXV) return 0;
    snprintf(g_vars[g_nvars].name, sizeof g_vars[0].name, "%s", name);
    g_vars[g_nvars].v = 0;
    return &g_vars[g_nvars++].v;
}
// label with spaces written as underscores (how a condition names a button)
static int label_is(const char *label, const char *id) {
    for (;; label++, id++) {
        char a = *label == ' ' ? '_' : *label;
        if (a != *id) return 0;
        if (!a) return 1;
    }
}
static double value_of(const char *name, int *ok) {
    *ok = 1;
    const RuleCtx *c = &rb_rctx;
    if (!strcmp(name, "down")) return c->down;
    if (!strcmp(name, "togo")) return c->togo;
    if (!strcmp(name, "legs")) return c->legs;
    if (!strcmp(name, "run_share")) return c->run_share;
    if (!strcmp(name, "pass_dial")) return c->pass_dial;
    if (!strcmp(name, "run_dial")) return c->run_dial;
    if (!strcmp(name, "held")) return c->held;
    if (!strcmp(name, "depth")) return c->depth;
    if (!strcmp(name, "yards")) return c->yards;
    if (!strcmp(name, "is_run")) return c->is_run;
    if (!strcmp(name, "worked")) return c->worked;
    if (!strcmp(name, "play_action")) return c->play_action;
    if (!strcmp(name, "protection")) return c->protection;
    if (!strcmp(name, "scrambled")) return c->scrambled;
    if (!strcmp(name, "quarter")) return c->quarter;
    if (!strcmp(name, "clock")) return c->clock;
    if (!strcmp(name, "score_diff")) return c->score_diff;
    if (!strcmp(name, "goal")) return c->goal;
    if (!strcmp(name, "chance")) return arc4random_uniform(10000) / 100.0;
    if (!strncmp(name, "var.", 4)) { double *v = var_slot(name + 4, 0); return v ? *v : 0; }
    if (!strncmp(name, "pressed.", 8)) {
        for (int i = 0; i < g_nbtn; i++) if (label_is(g_btn[i], name + 8)) return g_btn_pressed[i];
        return 0;
    }
    *ok = 0;
    return 0;
}

static int parse_op(const char *s) {
    if (!strcmp(s, "=") || !strcmp(s, "==")) return 0;
    if (!strcmp(s, "!=")) return 1;
    if (!strcmp(s, "<")) return 2;
    if (!strcmp(s, "<=")) return 3;
    if (!strcmp(s, ">")) return 4;
    if (!strcmp(s, ">=")) return 5;
    return -1;
}
// rest of a line after its first `words` words
static const char *after_words(const char *s, int words) {
    while (words--) {
        while (*s && *s != ' ' && *s != '\t') s++;
        while (*s == ' ' || *s == '\t') s++;
    }
    return s;
}
// parse a rules buffer into g_rules; returns the number of lines it could not use
int rb_rules_parse(const char *buf) {
    g_nrules = 0;
    int pos = 0, bad = 0, in = 0; char line[240];
    Rule *r = 0;
    while (buf[pos]) {
        pos = next_line(buf, pos, line, sizeof line);
        if (!line[0] || line[0] == '#') continue;
        char w[16] = "", a[40] = "", b[40] = "", c[40] = "", d[16] = "";
        int n = sscanf(line, "%15s %39s %39s %39s %15s", w, a, b, c, d);
        if (!strcmp(w, "rule")) {
            if (g_nrules >= MAXR) { bad++; in = 0; continue; }
            r = &g_rules[g_nrules++]; memset(r, 0, sizeof *r); in = 1;
            snprintf(r->name, sizeof r->name, "%s", after_words(line, 1));
        } else if (!in) { bad++;
        } else if (!strcmp(w, "end")) { in = 0;
        } else if (!strcmp(w, "when") && n >= 2) {
            snprintf(r->ev, sizeof r->ev, "%s", a);
            snprintf(r->arg, sizeof r->arg, "%s", after_words(line, 2));
        } else if (!strcmp(w, "if") && n >= 4 && r->nc < MAXC) {
            Cond *q = &r->c[r->nc]; double v;
            int op = parse_op(b);
            if (op < 0 || sscanf(c, "%lf", &v) != 1) { bad++; continue; }
            snprintf(q->var, sizeof q->var, "%s", a); q->op = op; q->val = v; r->nc++;
        } else if (!strcmp(w, "then") && n >= 2 && r->na < MAXA) {
            Act *x = &r->a[r->na]; memset(x, 0, sizeof *x);
            snprintf(x->type, sizeof x->type, "%s", a);
            double v = 0;
            if (0) {
            } else if (!strcmp(a, "tune")) {
                char key[40], op[4], scope[8] = "play";
                if (sscanf(line, "%*s %*s %39s %3s %lf %7s", key, op, &v, scope) < 3 || rb_tune_find(key) < 0) { bad++; continue; }
                snprintf(x->a, sizeof x->a, "%s", key); x->op = op[0]; x->v = v; x->keep = !strcmp(scope, "keep");
            } else if (!strcmp(a, "var")) {
                char name[32], op[4];
                if (sscanf(line, "%*s %*s %31s %3s %lf", name, op, &v) != 3) { bad++; continue; }
                snprintf(x->a, sizeof x->a, "%s", name); x->op = op[0]; x->v = v;
            } else if (!strcmp(a, "legs")) {
                char op[4];
                if (sscanf(line, "%*s %*s %3s %lf", op, &v) != 2) { bad++; continue; }
                x->op = op[0]; x->v = v;
            } else if (!strcmp(a, "speed")) {
                char grp[16];
                if (sscanf(line, "%*s %*s %15s %lf", grp, &v) != 2) { bad++; continue; }
                snprintf(x->a, sizeof x->a, "%s", grp); x->v = v;
            } else if (!strcmp(a, "sneak") || !strcmp(a, "kneel") || !strcmp(a, "deep") || !strcmp(a, "hailmary")) {
            } else if (!strcmp(a, "push")) {
                if (sscanf(line, "%*s %*s %lf", &v) != 1) v = 100;
                x->v = v;
            } else if (!strcmp(a, "penalty")) {
                char how[12] = "next";
                if (sscanf(line, "%*s %*s %lf %11s", &v, how) < 1) { bad++; continue; }
                x->v = v; x->keep = !strcmp(how, "replay");
            } else if (!strcmp(a, "msg") || !strcmp(a, "button")) {
                snprintf(x->text, sizeof x->text, "%s", after_words(line, 2));
                if (!x->text[0]) { bad++; continue; }
            } else { bad++; continue; }
            r->na++;
        } else bad++;
    }
    return bad;
}
int rb_rules_count(void) { return g_nrules; }

void rb_rules_load(void) {
    static char buf[16384];
    int n = slurp("rbmod_rules.txt", buf, sizeof buf);
    unsigned h = n < 0 ? 0 : hash(buf, n) | 2;
    if (h == g_rule_hash) return;
    g_rule_hash = h;
    if (n < 0) { g_nrules = 0; rb_log("rules: no rbmod_rules.txt - no rules"); return; }
    int bad = rb_rules_parse(buf);
    rb_log("rules: rbmod_rules.txt loaded - %d rules%s", g_nrules, bad ? " (some lines could not be read)" : "");
    for (int i = 0; i < g_nrules; i++)
        rb_log("rules:   \"%s\" when %s %s - %d conditions, %d actions", g_rules[i].name, g_rules[i].ev, g_rules[i].arg, g_rules[i].nc, g_rules[i].na);
}

// a new play: forget this play's buttons, presses and messages
void rb_rules_new_play(void) { g_nbtn = 0; memset(g_btn_pressed, 0, sizeof g_btn_pressed); g_msg[0] = 0; }
void rb_rules_snapped(void) { g_carry[0] = 0; }           // the carried-over messages have been seen
int rb_rules_buttons(const char **labels, int max) {
    int n = 0;
    for (; n < g_nbtn && n < max; n++) labels[n] = g_btn[n];
    return n;
}
int rb_rules_button_pressed(int i) { return i >= 0 && i < g_nbtn ? g_btn_pressed[i] : 0; }
const char *rb_rules_msg(void) {
    snprintf(g_both, sizeof g_both, "%s%s%s", g_carry, g_carry[0] && g_msg[0] ? "\n" : "", g_msg);
    return g_both;
}

static int holds(const Cond *q) {
    int ok; double v = value_of(q->var, &ok);
    if (!ok) return 0;
    switch (q->op) {
        case 0: return fabs(v - q->val) < 1e-6;
        case 1: return fabs(v - q->val) >= 1e-6;
        case 2: return v < q->val;
        case 3: return v <= q->val;
        case 4: return v > q->val;
        default: return v >= q->val;
    }
}
static void run(const Rule *r) {
    for (int i = 0; i < r->na; i++) {
        const Act *x = &r->a[i];
        if (!strcmp(x->type, "tune")) rb_tune_apply(rb_tune_find(x->a), x->op, x->v, x->keep);
        else if (!strcmp(x->type, "var")) { double *v = var_slot(x->a, 1); if (v) *v = x->op == '+' ? *v + x->v : x->v; }
        else if (!strcmp(x->type, "legs")) rb_rules_do_legs(x->op, x->v);
        else if (!strcmp(x->type, "speed")) rb_rules_do_speed(x->a, x->v);
        else if (!strcmp(x->type, "sneak") || !strcmp(x->type, "kneel") || !strcmp(x->type, "deep") || !strcmp(x->type, "push") || !strcmp(x->type, "hailmary")) rb_rules_do_play(x->type, x->v);
        else if (!strcmp(x->type, "penalty")) rb_rules_do_penalty(x->v, x->keep);
        else if (!strcmp(x->type, "msg")) {
            // said before the snap: this play's panel. Said later (a result, a flag): the next play's.
            char *buf = g_firing_presnap ? g_msg : g_carry;
            int n = (int)strlen(buf);
            snprintf(buf + n, sizeof g_msg - (size_t)n, "%s%s", n ? "\n" : "", x->text);
        } else if (strcmp(x->type, "button")) {
            rb_log("rules: \"%s\" has an action this build doesn't know (%s) - skipped", r->name, x->type);
        } else if (!strcmp(x->type, "button")) {
            int have = 0;
            for (int k = 0; k < g_nbtn; k++) if (!strcmp(g_btn[k], x->text)) have = 1;
            if (!have && g_nbtn < MAXB) snprintf(g_btn[g_nbtn++], sizeof g_btn[0], "%s", x->text);
        }
    }
}
int rb_rules_fire(const char *ev, const char *arg) {
    int fired = 0;
    g_firing_presnap = !strcmp(ev, "presnap") || !strcmp(ev, "button");
    for (int i = 0; i < g_nrules; i++) {
        const Rule *r = &g_rules[i];
        if (strcmp(r->ev, ev)) continue;
        if (arg && strcmp(r->arg, arg)) continue;
        int ok = 1;
        for (int k = 0; k < r->nc && ok; k++) ok = holds(&r->c[k]);
        if (!ok) continue;
        run(r);
        fired++;
        rb_log("rules: \"%s\" fired (%s%s%s)", r->name, ev, arg ? " " : "", arg ? arg : "");
    }
    return fired;
}
// pre-snap button i was pressed: remember it for this play and fire its rules
void rb_rules_press(int i) {
    if (i < 0 || i >= g_nbtn) return;
    g_btn_pressed[i] = !g_btn_pressed[i];                    // tapping again takes it back
    rb_log("rules: button \"%s\" %s", g_btn[i], g_btn_pressed[i] ? "pressed" : "released");
    if (g_btn_pressed[i]) { rb_rules_fire("button", g_btn[i]); return; }
    // Released: undo what its rule changed for this play by starting the play's changes over - the
    // presnap rules again, then the rules of every button that is still down. (Only "tune ... play"
    // and messages can be taken back like this; put speed/legs effects in a "snap" rule that checks
    // pressed.<button>, so they happen once, when the play really starts.)
    rb_tune_play_reset();
    g_msg[0] = 0;
    rb_rules_fire("presnap", 0);
    for (int k = 0; k < g_nbtn; k++) if (g_btn_pressed[k]) rb_rules_fire("button", g_btn[k]);
}

// ------------------------------------------------------------------------------------------ report
void rb_report_line(const char *fmt, ...) {
    char fp[1024]; doc_path(fp, sizeof fp, "rbmod_plays.txt");
    FILE *f = fopen(fp, "a");
    if (!f) return;
    char line[700];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(f, "%s\n", line);
    fclose(f);
}
