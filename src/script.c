// script.c - the play-script language. Self-contained: parsing, checking and running scripts; the game
// is reached only through the sh_* host functions (script.h).
//
//   # a comment
//   on snap                          events: presnap  button "Label"  snap  release  frame  throw  catch
//     if pressed("Hail Mary")                incomplete  intercepted  sack  touchdown  play_end
//       for r in receivers()
//         place(r, endzone_x(2), r.y)
//       end
//     end
//   end
//
// Statements:   name = expr     p.var = expr     if / elif / else / end     for x in list / end
//               repeat n / end  stop             any function call
// Expressions:  numbers, "strings", true false nil, + - * / %, == != < <= > >=, and or not, ( ),
//               p.var (any game variable of a player), function calls.
// Variables keep their value from play to play (numbers and text). Players and lists only last for the
// handler run that made them, because the game rebuilds every player each play.
#ifdef RB_SCRIPT_TEST
#include "test_sys.h"
#else
#include "rb.h"
#include "sys.h"
#endif
#include "script.h"

#define MAXTOK    6000
#define MAXNODE   4000
#define MAXH      48
#define MAXVAR    128
#define POOL      24000
#define RPOOL     4096
#define MAXLIST   48
#define LISTCAP   32
#define STEPS     20000
#define STEPS_FRAME 4000

// ------------------------------------------------------------------------------------------- tokens
enum { T_EOF, T_NL, T_NUM, T_STR, T_ID, T_OP };
typedef struct { unsigned char t; char op[3]; int line; double n; const char *s; } Tok;
static Tok tk[MAXTOK];
static int ntk, tp;
static char pool[POOL];
static int npool;
static char g_err[160];
static int g_err_line;

static const char *keep(const char *s, int n) {
    if (npool + n + 1 > POOL) return 0;
    char *d = pool + npool;
    memcpy(d, s, (size_t)n); d[n] = 0; npool += n + 1;
    return d;
}
static int is_al(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_dg(int c) { return c >= '0' && c <= '9'; }
static int in_set(const char *set, char c) { for (; *set; set++) if (*set == c) return 1; return 0; }
static void fail(int line, const char *msg, const char *what) {
    if (g_err[0]) return;
    g_err_line = line;
    snprintf(g_err, sizeof g_err, "%s%s%s", msg, what ? " " : "", what ? what : "");
}
static int lex(const char *src) {
    ntk = 0; npool = 0;
    int line = 1;
    const char *p = src;
    while (*p) {
        if (ntk >= MAXTOK - 2) { fail(line, "script is too long", 0); return 0; }
        char c = *p;
        if (c == '#') { while (*p && *p != '\n') p++; continue; }
        if (c == '\n') {
            if (ntk && tk[ntk - 1].t != T_NL) { tk[ntk].t = T_NL; tk[ntk].line = line; ntk++; }
            line++; p++; continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') { p++; continue; }
        Tok *t = &tk[ntk]; t->line = line; t->s = 0; t->n = 0; t->op[0] = 0;
        if (is_dg(c) || (c == '.' && is_dg(p[1]))) {
            double v = 0, f = 0.1; int frac = 0;
            while (is_dg(*p) || (*p == '.' && !frac)) {
                if (*p == '.') frac = 1;
                else if (!frac) v = v * 10 + (*p - '0');
                else { v += (*p - '0') * f; f *= 0.1; }
                p++;
            }
            t->t = T_NUM; t->n = v; ntk++; continue;
        }
        if (is_al(c)) {
            const char *b = p;
            while (is_al(*p) || is_dg(*p)) p++;
            t->t = T_ID; t->s = keep(b, (int)(p - b));
            if (!t->s) { fail(line, "script is too long", 0); return 0; }
            ntk++; continue;
        }
        if (c == '"') {
            const char *b = ++p;
            while (*p && *p != '"' && *p != '\n') p++;
            if (*p != '"') { fail(line, "text is missing its closing quote", 0); return 0; }
            t->t = T_STR; t->s = keep(b, (int)(p - b)); p++;
            if (!t->s) { fail(line, "script is too long", 0); return 0; }
            ntk++; continue;
        }
        static const char *two[] = { "==", "!=", "<=", ">=" };
        int k2 = 0;
        for (int i = 0; i < 4; i++) if (p[0] == two[i][0] && p[1] == two[i][1]) { t->op[0] = p[0]; t->op[1] = p[1]; t->op[2] = 0; k2 = 1; }
        if (k2) { t->t = T_OP; p += 2; ntk++; continue; }
        if (in_set("+-*/%<>=().,", c)) { t->t = T_OP; t->op[0] = c; t->op[1] = 0; p++; ntk++; continue; }
        char bad[2] = { c, 0 };
        fail(line, "unexpected character", bad);
        return 0;
    }
    if (ntk && tk[ntk - 1].t != T_NL) { tk[ntk].t = T_NL; tk[ntk].line = line; ntk++; }
    tk[ntk].t = T_EOF; tk[ntk].line = line; ntk++;
    return 1;
}

// ------------------------------------------------------------------------------------------ functions
enum {
    F_RECEIVERS, F_DEFENDERS, F_OFFENSE, F_DEFENSE, F_DEEPEST, F_NEAREST, F_SORT_Y, F_COUNT, F_ITEM,
    F_QB, F_BACK, F_BALL,
    F_DOWN, F_TOGO, F_QUARTER, F_CLOCK, F_LEAD, F_GOAL, F_LOS_X, F_FWD, F_FIELD_TOP, F_FIELD_BOTTOM, F_LEGS,
    F_LANDING_X, F_LANDING_Y, F_STATE, F_SINCE_SNAP, F_THROWN, F_TEMPO, F_TACKLER, F_JUKE_READY,
    F_YARD, F_ENDZONE_X, F_RANDOM, F_CHANCE, F_ABS, F_MIN, F_MAX, F_FLOOR, F_ROUND, F_SQRT, F_DIST, F_STR,
    F_GAME, F_SET_GAME, F_GLOBAL, F_SET_GLOBAL, F_CALL, F_CALL_ON, F_GET, F_SET,
    F_PLACE, F_STEER, F_SPEED, F_ROUTE, F_CARRY, F_ICON, F_ICONS, F_SPREAD,
    F_PRESSED, F_BUTTON, F_MSG, F_TUNE, F_TUNE_ADD, F_LOG,
    F_N
};
typedef struct { const char *name; signed char lo, hi; } FnDef;
static const FnDef FN[F_N] = {
    { "receivers", 0, 0 }, { "defenders", 0, 0 }, { "offense", 0, 0 }, { "defense", 0, 0 }, { "deepest", 2, 2 },
    { "nearest", 4, 4 }, { "sort_y", 1, 1 }, { "count", 1, 1 }, { "item", 2, 2 },
    { "qb", 0, 0 }, { "back", 0, 0 }, { "ball", 0, 0 },
    { "down", 0, 0 }, { "togo", 0, 0 }, { "quarter", 0, 0 }, { "clock", 0, 0 }, { "lead", 0, 0 }, { "goal", 0, 0 },
    { "los_x", 0, 0 }, { "fwd", 0, 0 }, { "field_top", 0, 0 }, { "field_bottom", 0, 0 }, { "legs", 0, 0 },
    { "landing_x", 0, 0 }, { "landing_y", 0, 0 }, { "state", 0, 0 }, { "since_snap", 0, 0 }, { "thrown", 0, 0 },
    { "tempo", 0, 0 }, { "tackler", 0, 0 }, { "juke_ready", 1, 1 },
    { "yard", 1, 1 }, { "endzone_x", 1, 1 }, { "random", 2, 2 }, { "chance", 1, 1 }, { "abs", 1, 1 }, { "min", 2, 2 },
    { "max", 2, 2 }, { "floor", 1, 1 }, { "round", 1, 1 }, { "sqrt", 1, 1 }, { "dist", 4, 4 }, { "str", 1, 1 },
    { "game", 1, 1 }, { "set_game", 2, 2 }, { "global", 1, 1 }, { "set_global", 2, 2 }, { "call", 1, 7 }, { "call_on", 2, 8 },
    { "get", 2, 2 }, { "set", 3, 3 },
    { "place", 3, 3 }, { "steer", 3, 4 }, { "speed", 2, 2 }, { "route", 2, 2 }, { "carry", 1, 2 }, { "icon", 1, 1 },
    { "icons", 1, 1 }, { "spread", 4, 4 },
    { "pressed", 1, 1 }, { "button", 1, 1 }, { "msg", 1, 1 }, { "tune", 2, 2 }, { "tune_add", 2, 2 }, { "log", 1, 6 },
};
const char *rb_script_fn_name(int i) { return i >= 0 && i < F_N ? FN[i].name : 0; }

// ------------------------------------------------------------------------------------------- nodes
enum { N_NUM, N_STR, N_NIL, N_VAR, N_CALL, N_MEMBER, N_UN, N_BIN, N_AND, N_OR,
       N_SET, N_SETM, N_IF, N_FOR, N_REPEAT, N_STOP, N_EXPR, N_BLOCK };
typedef struct { unsigned char k; char op; short fn; int line; double n; const char *s; int a, b, c, next; } Node;
static Node nd[MAXNODE];
static int nnd;
typedef struct { char ev[16]; char arg[40]; int block, line, off; } Handler;
static Handler H[MAXH];
static int nh;

static int mk(int k, int line) {
    if (nnd >= MAXNODE) { fail(line, "script is too long", 0); return -1; }
    Node *n = &nd[nnd]; memset(n, 0, sizeof *n);
    n->k = (unsigned char)k; n->line = line; n->a = n->b = n->c = n->next = -1;
    return nnd++;
}
static Tok *cur(void) { return &tk[tp]; }
static int is_kw(Tok *t, const char *w) { return t->t == T_ID && !strcmp(t->s, w); }
static int is_op(Tok *t, const char *o) { return t->t == T_OP && !strcmp(t->op, o); }
static int reserved(const char *s) {
    static const char *kw[] = { "on", "end", "if", "elif", "else", "for", "in", "repeat", "stop", "and", "or", "not", "true", "false", "nil", 0 };
    for (int i = 0; kw[i]; i++) if (!strcmp(kw[i], s)) return 1;
    return 0;
}
static int expect_nl(void) {
    if (cur()->t == T_NL) { tp++; return 1; }
    if (is_op(cur(), "=")) fail(cur()->line, "use == to compare (a single = sets a variable)", 0);
    else fail(cur()->line, "expected the end of the line here", 0);
    return 0;
}

static int expr(void);
static int primary(void) {
    Tok *t = cur();
    int line = t->line;
    if (t->t == T_NUM) { int n = mk(N_NUM, line); if (n >= 0) nd[n].n = t->n; tp++; return n; }
    if (t->t == T_STR) { int n = mk(N_STR, line); if (n >= 0) nd[n].s = t->s; tp++; return n; }
    if (is_op(t, "(")) {
        tp++;
        int e = expr();
        if (e < 0) return -1;
        if (!is_op(cur(), ")")) { fail(cur()->line, "missing )", 0); return -1; }
        tp++;
        return e;
    }
    if (t->t == T_ID) {
        if (is_kw(t, "true") || is_kw(t, "false")) { int n = mk(N_NUM, line); if (n >= 0) nd[n].n = is_kw(t, "true"); tp++; return n; }
        if (is_kw(t, "nil")) { tp++; return mk(N_NIL, line); }
        if (reserved(t->s)) { fail(line, "unexpected word", t->s); return -1; }
        tp++;
        if (is_op(cur(), "(")) {                                         // function call
            int f = -1;
            for (int i = 0; i < F_N; i++) if (!strcmp(FN[i].name, t->s)) f = i;
            if (f < 0) { fail(line, "unknown function", t->s); return -1; }
            tp++;
            int n = mk(N_CALL, line), last = -1, argc = 0;
            if (n < 0) return -1;
            nd[n].fn = (short)f;
            if (!is_op(cur(), ")")) {
                for (;;) {
                    int e = expr();
                    if (e < 0) return -1;
                    if (last < 0) nd[n].a = e; else nd[last].next = e;
                    last = e; argc++;
                    if (is_op(cur(), ",")) { tp++; continue; }
                    break;
                }
            }
            if (!is_op(cur(), ")")) { fail(cur()->line, "missing ) after the arguments of", FN[f].name); return -1; }
            tp++;
            if (argc < FN[f].lo || argc > FN[f].hi) {
                char m[64]; snprintf(m, sizeof m, "%s() takes %d%s%d values, got %d", FN[f].name, FN[f].lo,
                                     FN[f].lo == FN[f].hi ? "" : " to ", FN[f].hi, argc);
                if (FN[f].lo == FN[f].hi) snprintf(m, sizeof m, "%s() takes %d value%s, got %d", FN[f].name, FN[f].lo, FN[f].lo == 1 ? "" : "s", argc);
                fail(line, m, 0); return -1;
            }
            nd[n].b = argc;
            return n;
        }
        int n = mk(N_VAR, line);
        if (n >= 0) nd[n].s = t->s;
        return n;
    }
    fail(line, "expected a value here", t->t == T_OP ? t->op : 0);
    return -1;
}
static int postfix(void) {
    int e = primary();
    while (e >= 0 && is_op(cur(), ".")) {
        tp++;
        Tok *t = cur();
        if (t->t != T_ID) { fail(t->line, "expected a name after .", 0); return -1; }
        int n = mk(N_MEMBER, t->line);
        if (n < 0) return -1;
        nd[n].a = e; nd[n].s = t->s; tp++;
        e = n;
    }
    return e;
}
static int unary(void) {
    if (is_op(cur(), "-")) { int line = cur()->line; tp++; int a = unary(); if (a < 0) return -1; int n = mk(N_UN, line); if (n >= 0) { nd[n].op = '-'; nd[n].a = a; } return n; }
    if (is_kw(cur(), "not")) { int line = cur()->line; tp++; int a = unary(); if (a < 0) return -1; int n = mk(N_UN, line); if (n >= 0) { nd[n].op = '!'; nd[n].a = a; } return n; }
    return postfix();
}
static int binary(int level) {
    // 0 or, 1 and, 2 comparison, 3 + -, 4 * / %
    if (level > 4) return unary();
    int a = binary(level + 1);
    while (a >= 0) {
        Tok *t = cur();
        char op = 0;
        if (level == 0 && is_kw(t, "or")) op = 'o';
        else if (level == 1 && is_kw(t, "and")) op = 'a';
        else if (level == 2 && t->t == T_OP) {
            if (!strcmp(t->op, "==")) op = 'e'; else if (!strcmp(t->op, "!=")) op = 'n';
            else if (!strcmp(t->op, "<")) op = '<'; else if (!strcmp(t->op, ">")) op = '>';
            else if (!strcmp(t->op, "<=")) op = 'l'; else if (!strcmp(t->op, ">=")) op = 'g';
        } else if (level == 3 && t->t == T_OP && (t->op[0] == '+' || t->op[0] == '-') && !t->op[1]) op = t->op[0];
        else if (level == 4 && t->t == T_OP && (t->op[0] == '*' || t->op[0] == '/' || t->op[0] == '%') && !t->op[1]) op = t->op[0];
        if (!op) break;
        int line = t->line;
        tp++;
        int b = binary(level + 1);
        if (b < 0) return -1;
        int n = mk(op == 'o' ? N_OR : op == 'a' ? N_AND : N_BIN, line);
        if (n < 0) return -1;
        nd[n].op = op; nd[n].a = a; nd[n].b = b;
        a = n;
    }
    return a;
}
static int expr(void) { return binary(0); }

static int block(int *ended_by);
// ended_by: 1 end, 2 elif, 3 else
static int stmt(void) {
    Tok *t = cur();
    int line = t->line;
    if (is_kw(t, "if")) {
        tp++;
        int c = expr(); if (c < 0) return -1;
        if (!expect_nl()) return -1;
        int how = 0, b = block(&how); if (b < 0) return -1;
        int n = mk(N_IF, line); if (n < 0) return -1;
        nd[n].a = c; nd[n].b = b;
        int at = n;
        while (how == 2) {                                               // elif chain
            int l2 = cur()->line;
            tp++;
            int c2 = expr(); if (c2 < 0) return -1;
            if (!expect_nl()) return -1;
            int b2 = block(&how); if (b2 < 0) return -1;
            int n2 = mk(N_IF, l2); if (n2 < 0) return -1;
            nd[n2].a = c2; nd[n2].b = b2;
            nd[at].c = n2; at = n2;
        }
        if (how == 3) {
            tp++;
            if (!expect_nl()) return -1;
            int b3 = block(&how); if (b3 < 0) return -1;
            if (how != 1) { fail(cur()->line, "else must be followed by end", 0); return -1; }
            nd[at].c = b3;
        }
        tp++;                                                            // end
        if (!expect_nl()) return -1;
        return n;
    }
    if (is_kw(t, "for")) {
        tp++;
        Tok *v = cur();
        if (v->t != T_ID || reserved(v->s)) { fail(v->line, "expected a name after for", 0); return -1; }
        tp++;
        if (!is_kw(cur(), "in")) { fail(cur()->line, "expected in", 0); return -1; }
        tp++;
        int l = expr(); if (l < 0) return -1;
        if (!expect_nl()) return -1;
        int how = 0, b = block(&how); if (b < 0) return -1;
        if (how != 1) { fail(cur()->line, "for must end with end", 0); return -1; }
        tp++; if (!expect_nl()) return -1;
        int n = mk(N_FOR, line); if (n < 0) return -1;
        nd[n].s = v->s; nd[n].a = l; nd[n].b = b;
        return n;
    }
    if (is_kw(t, "repeat")) {
        tp++;
        int c = expr(); if (c < 0) return -1;
        if (!expect_nl()) return -1;
        int how = 0, b = block(&how); if (b < 0) return -1;
        if (how != 1) { fail(cur()->line, "repeat must end with end", 0); return -1; }
        tp++; if (!expect_nl()) return -1;
        int n = mk(N_REPEAT, line); if (n < 0) return -1;
        nd[n].a = c; nd[n].b = b;
        return n;
    }
    if (is_kw(t, "stop")) { tp++; if (!expect_nl()) return -1; return mk(N_STOP, line); }
    if (t->t == T_ID && !reserved(t->s) && is_op(&tk[tp + 1], "=")) {    // name = expr
        tp += 2;
        int e = expr(); if (e < 0) return -1;
        if (!expect_nl()) return -1;
        int n = mk(N_SET, line); if (n < 0) return -1;
        nd[n].s = t->s; nd[n].a = e;
        return n;
    }
    int e = expr(); if (e < 0) return -1;
    if (is_op(cur(), "=")) {                                             // p.var = expr
        if (nd[e].k != N_MEMBER) { fail(line, "only a name or a player's .variable can be set", 0); return -1; }
        tp++;
        int v = expr(); if (v < 0) return -1;
        if (!expect_nl()) return -1;
        int n = mk(N_SETM, line); if (n < 0) return -1;
        nd[n].a = nd[e].a; nd[n].s = nd[e].s; nd[n].b = v;
        return n;
    }
    if (nd[e].k != N_CALL) { fail(line, "this line doesn't do anything (expected a function call or an assignment)", 0); return -1; }
    if (!expect_nl()) return -1;
    int n = mk(N_EXPR, line); if (n < 0) return -1;
    nd[n].a = e;
    return n;
}
static int block(int *ended_by) {
    int b = mk(N_BLOCK, cur()->line), last = -1;
    if (b < 0) return -1;
    for (;;) {
        Tok *t = cur();
        if (t->t == T_NL) { tp++; continue; }
        if (t->t == T_EOF) { fail(t->line, "missing end", 0); return -1; }
        if (is_kw(t, "end")) { *ended_by = 1; return b; }
        if (is_kw(t, "elif")) { *ended_by = 2; return b; }
        if (is_kw(t, "else")) { *ended_by = 3; return b; }
        if (is_kw(t, "on")) { fail(t->line, "missing end before the next on", 0); return -1; }
        int s = stmt();
        if (s < 0) return -1;
        if (last < 0) nd[b].a = s; else nd[last].next = s;
        last = s;
    }
}
static const char *EVENTS[] = { "presnap", "button", "snap", "release", "frame", "throw", "catch", "incomplete",
                                "intercepted", "sack", "touchdown", "play_end", "drive_end", "juke", 0 };
int rb_script_parse(const char *src) {
    g_err[0] = 0; g_err_line = 0; nnd = 0; nh = 0; tp = 0;
    if (!lex(src)) return -1;
    for (;;) {
        Tok *t = cur();
        if (t->t == T_NL) { tp++; continue; }
        if (t->t == T_EOF) break;
        if (!is_kw(t, "on")) { fail(t->line, "everything must be inside an  on <event> ... end  block", 0); return -1; }
        int line = t->line;
        tp++;
        Tok *e = cur();
        int ok = 0;
        for (int i = 0; e->t == T_ID && EVENTS[i]; i++) if (!strcmp(EVENTS[i], e->s)) ok = 1;
        if (!ok) { fail(e->line, "unknown event", e->t == T_ID ? e->s : 0); return -1; }
        if (nh >= MAXH) { fail(line, "too many on blocks", 0); return -1; }
        Handler *h = &H[nh];
        memset(h, 0, sizeof *h);
        snprintf(h->ev, sizeof h->ev, "%s", e->s);
        h->line = line;
        tp++;
        if (!strcmp(h->ev, "button")) {
            if (cur()->t != T_STR) { fail(cur()->line, "on button needs the button's label in quotes", 0); return -1; }
            snprintf(h->arg, sizeof h->arg, "%s", cur()->s);
            tp++;
        }
        if (!expect_nl()) return -1;
        int how = 0, b = block(&how);
        if (b < 0) return -1;
        if (how != 1) { fail(cur()->line, how == 2 ? "elif without if" : "else without if", 0); return -1; }
        tp++;
        if (cur()->t != T_EOF && !expect_nl()) return -1;
        h->block = b;
        nh++;
    }
    return 0;
}
const char *rb_script_error(int *line) { if (line) *line = g_err_line; return g_err; }
int rb_script_handlers(void) { return nh; }

// ------------------------------------------------------------------------------------------- values
enum { V_NIL, V_NUM, V_STR, V_INST, V_LIST };
typedef struct { void *v[LISTCAP]; int n; } List;
typedef struct { unsigned char t; double n; const char *s; void *p; List *l; } Val;
static List lists[MAXLIST];
static int nlist;
static char rpool[RPOOL];
static int nrpool;
static struct { char name[32]; Val v; char sbuf[64]; int temp; } V[MAXVAR];
static int nvar;
static int steps, step_limit, stopping, rerr;
static int run_line;
static char rmsg[160];

static Val vnil(void) { Val v; memset(&v, 0, sizeof v); return v; }
static Val vnum(double n) { Val v = vnil(); v.t = V_NUM; v.n = n; return v; }
static Val vstr(const char *s) { Val v = vnil(); v.t = V_STR; v.s = s ? s : ""; return v; }
static Val vinst(void *p) { Val v = vnil(); if (p) { v.t = V_INST; v.p = p; } return v; }
static const char *rkeep(const char *s) {
    int n = (int)strlen(s);
    if (nrpool + n + 1 > RPOOL) nrpool = 0;                              // (wrap: old temporaries are gone)
    char *d = rpool + nrpool; memcpy(d, s, (size_t)n + 1); nrpool += n + 1;
    return d;
}
static List *newlist(void) {
    if (nlist >= MAXLIST) return 0;
    List *l = &lists[nlist++]; l->n = 0;
    return l;
}
static void rfail(int line, const char *fmt, const char *a) {
    if (rerr) return;
    rerr = 1; run_line = line;
    snprintf(rmsg, sizeof rmsg, fmt, a ? a : "");
}
static int truthy(Val v) {
    switch (v.t) {
        case V_NUM: return v.n != 0;
        case V_STR: return v.s[0] != 0;
        case V_INST: return 1;
        case V_LIST: return v.l && v.l->n > 0;
        default: return 0;
    }
}
static const char *tostr(Val v, char *buf, int n) {
    switch (v.t) {
        case V_NUM: {
            double x = v.n;
            if (x == (long long)x && fabs(x) < 1e15) snprintf(buf, (size_t)n, "%lld", (long long)x);
            else snprintf(buf, (size_t)n, "%.3f", x);
            return buf;
        }
        case V_STR: return v.s;
        case V_INST: return "(player)";
        case V_LIST: snprintf(buf, (size_t)n, "(list of %d)", v.l ? v.l->n : 0); return buf;
        default: return "nil";
    }
}
static int var_index(const char *name, int create) {
    for (int i = 0; i < nvar; i++) if (!strcmp(V[i].name, name)) return i;
    if (!create || nvar >= MAXVAR) return -1;
    snprintf(V[nvar].name, sizeof V[0].name, "%s", name);
    V[nvar].v = vnil(); V[nvar].temp = 0;
    return nvar++;
}
static Val *var_slot(const char *name, int create) { int i = var_index(name, create); return i < 0 ? 0 : &V[i].v; }
static void var_set(const char *name, Val v, int line) {
    int i = var_index(name, 1);
    if (i < 0) { rfail(line, "too many variables%s", ""); return; }
    if (v.t == V_STR) {                                                  // text keeps its own copy
        if (v.s != V[i].sbuf) snprintf(V[i].sbuf, sizeof V[i].sbuf, "%s", v.s);
        v.s = V[i].sbuf;
    }
    V[i].temp = v.t == V_INST || v.t == V_LIST;
    V[i].v = v;
}
// end of a handler run: players and lists don't outlive it
static void drop_temps(void) {
    for (int i = 0; i < nvar; i++) if (V[i].temp) { V[i].v = vnil(); V[i].temp = 0; }
}

// ------------------------------------------------------------------------------------------- running
static Val eval(int n);
static int num_arg(Val v, double *out, int line, const char *fn) {
    if (v.t == V_NUM) { *out = v.n; return 1; }
    rfail(line, "%s() expected a number", fn);
    return 0;
}
static List *list_arg(Val v, int line, const char *fn) {
    if (v.t == V_LIST && v.l) return v.l;
    rfail(line, "%s() expected a list (like receivers())", fn);
    return 0;
}
static void *inst_arg(Val v, int line, const char *fn) {
    if (v.t == V_INST) return v.p;
    rfail(line, "%s() expected a player", fn);
    return 0;
}
static const char *str_arg(Val v, int line, const char *fn) {
    if (v.t == V_STR) return v.s;
    rfail(line, "%s() expected text in quotes", fn);
    return 0;
}
static Val mklist(void **items, int n) {
    List *l = newlist();
    if (!l) { rfail(0, "too many lists in one run", 0); return vnil(); }
    if (n > LISTCAP) n = LISTCAP;
    for (int i = 0; i < n; i++) l->v[i] = items[i];
    l->n = n;
    Val v = vnil(); v.t = V_LIST; v.l = l;
    return v;
}
static Val info(const char *name, int line) {
    int ok = 0;
    double v = sh_info(name, &ok);
    if (!ok) return vnil();
    return vnum(v);
}
static Val call(int n) {
    Node *c = &nd[n];
    Val a[8]; int argc = 0;
    for (int k = c->a; k >= 0 && argc < 8; k = nd[k].next) { a[argc++] = eval(k); if (rerr) return vnil(); }
    const char *fn = FN[c->fn].name;
    int line = c->line;
    double x, y, z, w;
    void *p;
    switch (c->fn) {
    case F_RECEIVERS: case F_DEFENDERS: case F_OFFENSE: case F_DEFENSE: {
        void *items[LISTCAP];
        int k = sh_list(fn, items, LISTCAP);
        return mklist(items, k);
    }
    case F_DEEPEST: {
        List *l = list_arg(a[0], line, fn); if (!l || !num_arg(a[1], &x, line, fn)) return vnil();
        double f = 0; int ok = 0; f = sh_info("fwd", &ok); if (!ok || !f) f = 1;
        void *items[LISTCAP]; int k = 0, used[LISTCAP] = {0};
        for (int want = 0; want < (int)x && want < l->n; want++) {
            int best = -1; double bd = -1e18;
            for (int i = 0; i < l->n; i++) if (!used[i]) { double d = sh_pos(l->v[i], 0) * f; if (d > bd) { bd = d; best = i; } }
            if (best < 0) break;
            used[best] = 1; items[k++] = l->v[best];
        }
        return mklist(items, k);
    }
    case F_NEAREST: {
        List *l = list_arg(a[0], line, fn);
        if (!l || !num_arg(a[1], &x, line, fn) || !num_arg(a[2], &y, line, fn) || !num_arg(a[3], &z, line, fn)) return vnil();
        void *items[LISTCAP]; int k = 0, used[LISTCAP] = {0};
        for (int want = 0; want < (int)z && want < l->n; want++) {
            int best = -1; double bd = 1e18;
            for (int i = 0; i < l->n; i++) if (!used[i]) { double d = hypot(sh_pos(l->v[i], 0) - x, sh_pos(l->v[i], 1) - y); if (d < bd) { bd = d; best = i; } }
            if (best < 0) break;
            used[best] = 1; items[k++] = l->v[best];
        }
        return mklist(items, k);
    }
    case F_SORT_Y: {
        List *l = list_arg(a[0], line, fn); if (!l) return vnil();
        void *items[LISTCAP]; int k = l->n;
        for (int i = 0; i < k; i++) items[i] = l->v[i];
        for (int i = 0; i < k; i++) for (int j = i + 1; j < k; j++)
            if (sh_pos(items[j], 1) < sh_pos(items[i], 1)) { void *t = items[i]; items[i] = items[j]; items[j] = t; }
        return mklist(items, k);
    }
    case F_COUNT: { List *l = list_arg(a[0], line, fn); return l ? vnum(l->n) : vnil(); }
    case F_ITEM: {
        List *l = list_arg(a[0], line, fn); if (!l || !num_arg(a[1], &x, line, fn)) return vnil();
        int i = (int)x;
        return i >= 0 && i < l->n ? vinst(l->v[i]) : vnil();
    }
    case F_QB: case F_BACK: case F_BALL: case F_TACKLER: return vinst(sh_one(fn));
    case F_JUKE_READY: { void *p = inst_arg(a[0], line, fn); if (p) sh_juke_ready(p); return vnil(); }
    case F_DOWN: case F_TOGO: case F_QUARTER: case F_CLOCK: case F_LEAD: case F_GOAL: case F_LOS_X: case F_FWD:
    case F_FIELD_TOP: case F_FIELD_BOTTOM: case F_LEGS: case F_LANDING_X: case F_LANDING_Y: case F_STATE:
    case F_SINCE_SNAP: case F_THROWN: case F_TEMPO:
        return info(fn, line);
    case F_YARD: if (!num_arg(a[0], &x, line, fn)) return vnil(); return vnum(x * 20.5);
    case F_ENDZONE_X: {
        if (!num_arg(a[0], &x, line, fn)) return vnil();
        int ok1 = 0, ok2 = 0, ok3 = 0;
        double los = sh_info("los_x", &ok1), g = sh_info("goal", &ok2), f = sh_info("fwd", &ok3);
        if (!ok1 || !ok2 || !ok3) return vnil();
        return vnum(los + f * (g + x) * 20.5);
    }
    case F_RANDOM: if (!num_arg(a[0], &x, line, fn) || !num_arg(a[1], &y, line, fn)) return vnil();
        return vnum(x + (y - x) * (arc4random_uniform(1000001) / 1000000.0));
    case F_CHANCE: if (!num_arg(a[0], &x, line, fn)) return vnil(); return vnum(arc4random_uniform(10000) < (unsigned)(x * 100));
    case F_ABS: if (!num_arg(a[0], &x, line, fn)) return vnil(); return vnum(fabs(x));
    case F_MIN: if (!num_arg(a[0], &x, line, fn) || !num_arg(a[1], &y, line, fn)) return vnil(); return vnum(x < y ? x : y);
    case F_MAX: if (!num_arg(a[0], &x, line, fn) || !num_arg(a[1], &y, line, fn)) return vnil(); return vnum(x > y ? x : y);
    case F_FLOOR: if (!num_arg(a[0], &x, line, fn)) return vnil(); return vnum(floor(x));
    case F_ROUND: if (!num_arg(a[0], &x, line, fn)) return vnil(); return vnum(floor(x + 0.5));
    case F_SQRT: if (!num_arg(a[0], &x, line, fn)) return vnil(); return vnum(x > 0 ? sqrt(x) : 0);
    case F_DIST: if (!num_arg(a[0], &x, line, fn) || !num_arg(a[1], &y, line, fn) || !num_arg(a[2], &z, line, fn) || !num_arg(a[3], &w, line, fn)) return vnil();
        return vnum(hypot(z - x, w - y));
    case F_STR: { char b[48]; return vstr(rkeep(tostr(a[0], b, sizeof b))); }
    case F_GAME: case F_GLOBAL: {
        const char *nm = str_arg(a[0], line, fn); if (!nm) return vnil();
        char sb[128]; double v;
        int r = sh_scope_get(c->fn == F_GLOBAL, nm, &v, sb, sizeof sb);
        if (r == 1) return vnum(v);
        if (r == 2) return vstr(rkeep(sb));
        rfail(line, "the game has no variable called %s", nm);
        return vnil();
    }
    case F_SET_GAME: case F_SET_GLOBAL: {
        const char *nm = str_arg(a[0], line, fn); if (!nm || !num_arg(a[1], &x, line, fn)) return vnil();
        if (!sh_scope_set(c->fn == F_SET_GLOBAL, nm, x)) rfail(line, "the game has no variable called %s", nm);
        return vnil();
    }
    case F_GET: {
        p = inst_arg(a[0], line, fn); const char *nm = str_arg(a[1], line, fn); if (!p || !nm) return vnil();
        char sb[128]; double v;
        int r = sh_get(p, nm, &v, sb, sizeof sb);
        if (r == 1) return vnum(v);
        if (r == 2) return vstr(rkeep(sb));
        rfail(line, "players have no variable called %s", nm);
        return vnil();
    }
    case F_SET: {
        p = inst_arg(a[0], line, fn); const char *nm = str_arg(a[1], line, fn);
        if (!p || !nm || !num_arg(a[2], &x, line, fn)) return vnil();
        if (!sh_set(p, nm, x)) rfail(line, "players have no variable called %s", nm);
        return vnil();
    }
    case F_CALL: case F_CALL_ON: {
        int base = c->fn == F_CALL_ON ? 1 : 0;
        void *self = base ? inst_arg(a[0], line, fn) : sh_one("ball");
        const char *nm = str_arg(a[base], line, fn);
        if (!nm || (base && !self)) return vnil();
        SArg sa[6]; int k = 0;
        for (int i = base + 1; i < argc && k < 6; i++, k++) {
            if (a[i].t == V_NUM) { sa[k].kind = 0; sa[k].n = a[i].n; sa[k].s = 0; }
            else if (a[i].t == V_STR) { sa[k].kind = 1; sa[k].s = a[i].s; sa[k].n = 0; }
            else { rfail(line, "call() can only pass numbers and text", 0); return vnil(); }
        }
        char sb[128]; double v = 0;
        int r = sh_call(self, nm, k, sa, &v, sb, sizeof sb);
        if (r < 0) { rfail(line, "the game has no routine called %s", nm); return vnil(); }
        if (r == 1) return vnum(v);
        if (r == 2) return vstr(rkeep(sb));
        return vnil();
    }
    case F_PLACE:
        p = inst_arg(a[0], line, fn);
        if (!p || !num_arg(a[1], &x, line, fn) || !num_arg(a[2], &y, line, fn)) return vnil();
        sh_place(p, x, y); return vnil();
    case F_STEER:
        p = inst_arg(a[0], line, fn);
        if (!p || !num_arg(a[1], &x, line, fn) || !num_arg(a[2], &y, line, fn)) return vnil();
        z = 1;
        if (argc > 3 && !num_arg(a[3], &z, line, fn)) return vnil();
        sh_steer(p, x, y, z); return vnil();
    case F_SPEED:
        p = inst_arg(a[0], line, fn);
        if (!p || !num_arg(a[1], &x, line, fn)) return vnil();
        sh_speed(p, x); return vnil();
    case F_ROUTE: {
        p = inst_arg(a[0], line, fn); const char *nm = str_arg(a[1], line, fn); if (!p || !nm) return vnil();
        if (!sh_route(p, nm)) rfail(line, "the route %s didn't take", nm);
        return vnil();
    }
    case F_CARRY:
        if (!num_arg(a[0], &x, line, fn)) return vnil();
        y = 5;
        if (argc > 1 && !num_arg(a[1], &y, line, fn)) return vnil();
        return vnum(sh_carry(x, y));                                     // 0 when it can't (not on release, or not aimed downfield)
    case F_ICON: p = inst_arg(a[0], line, fn); if (p) sh_icon(p); return vnil();
    case F_ICONS: { List *l = list_arg(a[0], line, fn); if (l) for (int i = 0; i < l->n; i++) sh_icon(l->v[i]); return vnil(); }
    case F_SPREAD: {                                                     // spread(list, x, y_from, y_to): evenly, keeping their order
        List *l = list_arg(a[0], line, fn);
        if (!l || !num_arg(a[1], &x, line, fn) || !num_arg(a[2], &y, line, fn) || !num_arg(a[3], &z, line, fn)) return vnil();
        void *items[LISTCAP]; int k = l->n;
        for (int i = 0; i < k; i++) items[i] = l->v[i];
        for (int i = 0; i < k; i++) for (int j = i + 1; j < k; j++)
            if (sh_pos(items[j], 1) < sh_pos(items[i], 1)) { void *t = items[i]; items[i] = items[j]; items[j] = t; }
        for (int i = 0; i < k; i++) sh_place(items[i], x, k == 1 ? (y + z) / 2 : y + (z - y) * i / (k - 1));
        return vnil();
    }
    case F_PRESSED: { const char *s = str_arg(a[0], line, fn); return s ? vnum(sh_pressed(s)) : vnil(); }
    case F_BUTTON: { const char *s = str_arg(a[0], line, fn); if (s) sh_button(s); return vnil(); }
    case F_MSG: { char b[48]; sh_msg(tostr(a[0], b, sizeof b)); return vnil(); }
    case F_TUNE: case F_TUNE_ADD: {
        const char *s = str_arg(a[0], line, fn); if (!s || !num_arg(a[1], &x, line, fn)) return vnil();
        if (!sh_tune(s, x, c->fn == F_TUNE_ADD)) rfail(line, "there is no setting called %s", s);
        return vnil();
    }
    case F_LOG: {
        char out[200]; int o = 0; out[0] = 0;
        for (int i = 0; i < argc; i++) { char b[48]; o += snprintf(out + o, sizeof out - (size_t)o, "%s%s", i ? " " : "", tostr(a[i], b, sizeof b)); if (o >= (int)sizeof out) break; }
        sh_log(out);
        return vnil();
    }
    }
    return vnil();
}
static Val member(Val o, const char *name, int line) {
    if (o.t != V_INST) { rfail(line, "only a player has .%s", name); return vnil(); }
    if (!strcmp(name, "x")) return vnum(sh_pos(o.p, 0));
    if (!strcmp(name, "y")) return vnum(sh_pos(o.p, 1));
    char sb[128]; double v;
    int r = sh_get(o.p, name, &v, sb, sizeof sb);
    if (r == 1) return vnum(v);
    if (r == 2) return vstr(rkeep(sb));
    rfail(line, "players have no variable called %s", name);
    return vnil();
}
static Val eval(int n) {
    if (rerr) return vnil();
    if (++steps > step_limit) { rfail(nd[n].line, "the script ran too long (is a loop running away?)", 0); return vnil(); }
    Node *e = &nd[n];
    switch (e->k) {
    case N_NUM: return vnum(e->n);
    case N_STR: return vstr(e->s);
    case N_NIL: return vnil();
    case N_VAR: { Val *v = var_slot(e->s, 0); return v ? *v : vnil(); }
    case N_CALL: return call(n);
    case N_MEMBER: { Val o = eval(e->a); if (rerr) return vnil(); return member(o, e->s, e->line); }
    case N_UN: {
        Val a = eval(e->a); if (rerr) return vnil();
        if (e->op == '!') return vnum(!truthy(a));
        if (a.t != V_NUM) { rfail(e->line, "- needs a number", 0); return vnil(); }
        return vnum(-a.n);
    }
    case N_AND: { Val a = eval(e->a); if (rerr || !truthy(a)) return vnum(0); return vnum(truthy(eval(e->b))); }
    case N_OR: { Val a = eval(e->a); if (rerr) return vnil(); if (truthy(a)) return vnum(1); return vnum(truthy(eval(e->b))); }
    case N_BIN: {
        Val a = eval(e->a), b = eval(e->b);
        if (rerr) return vnil();
        char op = e->op;
        if (op == 'e' || op == 'n') {
            int eq;
            if (a.t != b.t) eq = 0;
            else if (a.t == V_NUM) eq = fabs(a.n - b.n) < 1e-9;
            else if (a.t == V_STR) eq = !strcmp(a.s, b.s);
            else if (a.t == V_INST) eq = a.p == b.p;
            else eq = a.t == V_NIL;
            return vnum(op == 'e' ? eq : !eq);
        }
        if (op == '+' && (a.t == V_STR || b.t == V_STR)) {
            char b1[48], b2[48], out[256];
            snprintf(out, sizeof out, "%s%s", tostr(a, b1, sizeof b1), tostr(b, b2, sizeof b2));
            return vstr(rkeep(out));
        }
        if (a.t != V_NUM || b.t != V_NUM) {
            rfail(e->line, a.t == V_NIL || b.t == V_NIL ? "a value is missing (nil) in this sum or comparison%s" : "this needs numbers on both sides%s", "");
            return vnil();
        }
        switch (op) {
            case '+': return vnum(a.n + b.n);
            case '-': return vnum(a.n - b.n);
            case '*': return vnum(a.n * b.n);
            case '/': if (b.n == 0) { rfail(e->line, "division by zero%s", ""); return vnil(); } return vnum(a.n / b.n);
            case '%': if (b.n == 0) { rfail(e->line, "division by zero%s", ""); return vnil(); } return vnum(a.n - b.n * floor(a.n / b.n));
            case '<': return vnum(a.n < b.n);
            case '>': return vnum(a.n > b.n);
            case 'l': return vnum(a.n <= b.n);
            case 'g': return vnum(a.n >= b.n);
        }
        return vnil();
    }
    }
    return vnil();
}
static void exec_block(int b);
static void exec(int s) {
    if (rerr || stopping) return;
    if (++steps > step_limit) { rfail(nd[s].line, "the script ran too long (is a loop running away?)", 0); return; }
    Node *e = &nd[s];
    switch (e->k) {
    case N_SET: { Val v = eval(e->a); if (!rerr) var_set(e->s, v, e->line); return; }
    case N_SETM: {
        Val o = eval(e->a), v = eval(e->b);
        if (rerr) return;
        if (o.t != V_INST) { rfail(e->line, "only a player's variables can be set with .%s", e->s); return; }
        if (v.t != V_NUM) { rfail(e->line, "a player's .%s can only be set to a number", e->s); return; }
        if (!strcmp(e->s, "x") || !strcmp(e->s, "y")) {
            double x = sh_pos(o.p, 0), y = sh_pos(o.p, 1);
            if (e->s[0] == 'x') x = v.n; else y = v.n;
            sh_place(o.p, x, y);
        } else if (!sh_set(o.p, e->s, v.n)) rfail(e->line, "players have no variable called %s", e->s);
        return;
    }
    case N_IF: {
        for (int at = s; at >= 0; ) {
            Node *c = &nd[at];
            if (c->k != N_IF) { exec_block(at); return; }
            Val v = eval(c->a);
            if (rerr) return;
            if (truthy(v)) { exec_block(c->b); return; }
            at = c->c;
        }
        return;
    }
    case N_FOR: {
        Val l = eval(e->a);
        if (rerr) return;
        if (l.t != V_LIST || !l.l) { rfail(e->line, "for needs a list (like receivers())%s", ""); return; }
        List copy = *l.l;                                                // (the body may make new lists)
        for (int i = 0; i < copy.n && !rerr && !stopping; i++) {
            var_set(e->s, vinst(copy.v[i]), e->line);
            exec_block(e->b);
        }
        return;
    }
    case N_REPEAT: {
        Val c = eval(e->a);
        if (rerr) return;
        if (c.t != V_NUM) { rfail(e->line, "repeat needs a number%s", ""); return; }
        for (int i = 0; i < (int)c.n && !rerr && !stopping; i++) exec_block(e->b);
        return;
    }
    case N_STOP: stopping = 1; return;
    case N_EXPR: eval(e->a); return;
    case N_BLOCK: exec_block(s); return;
    }
}
static void exec_block(int b) {
    for (int s = nd[b].a; s >= 0 && !rerr && !stopping; s = nd[s].next) exec(s);
}

// run every enabled handler for an event; a handler that errors is switched off (until the file changes)
static int script_run_all(const char *ev, const char *arg);
int rb_script_run(const char *ev, const char *arg) {
    static int depth;                                        // an event fired from inside a running block would corrupt it
    if (depth) { char m[96]; snprintf(m, sizeof m, "script: %s skipped (another block is running)", ev); sh_log(m); return 0; }
    depth++;
    int r = script_run_all(ev, arg);
    depth--;
    return r;
}
static int script_run_all(const char *ev, const char *arg) {
    int ran = 0;
    for (int i = 0; i < nh; i++) {
        Handler *h = &H[i];
        if (h->off || strcmp(h->ev, ev)) continue;
        if (arg && strcmp(h->arg, arg)) continue;
        if (!arg && !strcmp(ev, "button")) continue;
        steps = 0; step_limit = !strcmp(ev, "frame") ? STEPS_FRAME : STEPS; stopping = 0; rerr = 0; nlist = 0;
        exec_block(h->block);
        drop_temps();
        ran++;
        if (rerr) {
            h->off = 1;
            char m[300];
            snprintf(m, sizeof m, "error on line %d: %s - the \"on %s%s%s\" block (line %d) is off until the script file changes",
                     run_line ? run_line : h->line, rmsg, h->ev, h->arg[0] ? " " : "", h->arg, h->line);
            sh_log(m);
        }
    }
    return ran;
}
void rb_script_reset_vars(void) { nvar = 0; }
int rb_script_enabled_handlers(void) { int n = 0; for (int i = 0; i < nh; i++) n += !H[i].off; return n; }
