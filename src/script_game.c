// script_game.c - play scripts, game side: loading Documents/rbmod_script.txt, the startup self-test,
// and the host functions that reach game variables and routines by name. The functions that move
// players and the ball live in offtools.c, next to the play they act on.
#include "rb.h"
#include "sys.h"
#include "script.h"
#include "script_names.inc"

#define VA_RV_FROM_CSTR   0x100bd6f74ULL     // YYRValue(const char*)
#define VA_G_GLOBAL       0x1010c82d8ULL     // CInstance* of the global object
extern CInstance *rb_game_controller;

// ---- names -> addresses (both tables are sorted by name)
static int bsearch_name(const NameVA *t, int n, const char *name) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(t[mid].n, name);
        if (!c) return mid;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}
static uint64_t var_va(const char *name) {
    int i = bsearch_name(RB_VARS, (int)(sizeof RB_VARS / sizeof RB_VARS[0]), name);
    return i < 0 ? 0 : 0x100000000ULL + RB_VARS[i].off;
}
static uint64_t script_va(const char *name) {
    if (!strncmp(name, "gml_Script_", 11)) name += 11;
    int i = bsearch_name(RB_SCRIPTS, (int)(sizeof RB_SCRIPTS / sizeof RB_SCRIPTS[0]), name);
    return i < 0 ? 0 : 0x100000000ULL + RB_SCRIPTS[i].off;
}
// GameMaker's own per-instance variables live in a different store from the game's variables
static int is_builtin(const char *n) {
    static const char *B[] = { "x", "y", "xstart", "ystart", "xprevious", "yprevious", "direction", "speed", "hspeed", "vspeed",
                               "image_angle", "image_alpha", "image_index", "image_speed", "image_xscale", "image_yscale",
                               "image_blend", "sprite_index", "depth", "visible", "object_index", "id", 0 };
    for (int i = 0; B[i]; i++) if (!strcmp(B[i], n)) return 1;
    return 0;
}

// ---- reading and writing a variable on an instance
static int read_var(CInstance *inst, const char *name, double *num, char *str, int n) {
    if (!inst) return 0;
    uint64_t va = var_va(name);
    if (!va) return 0;
    if (is_builtin(name)) { *num = rb_get_builtin_real(inst, va); return 1; }
    RValue *v = rb_inst_var(inst, va);
    if (!v) return 0;
    unsigned k = v->kind & 0xffffff;
    if (k == RV_STRING) { rb_rv_str(v, str, n); return 2; }
    if (k == RV_UNDEFINED) return 0;
    *num = rb_rv_real(v);
    return 1;
}
static int write_var(CInstance *inst, const char *name, double v) {
    if (!inst) return 0;
    uint64_t va = var_va(name);
    if (!va) return 0;
    if (is_builtin(name)) rb_set_builtin_real(inst, va, v);
    else rb_set_inst_real(inst, va, v);
    return 1;
}
int sh_get(void *p, const char *name, double *num, char *str, int n) { return read_var(p, name, num, str, n); }
int sh_set(void *p, const char *name, double v) { return write_var(p, name, v); }
static CInstance *scope_inst(int global) {
    if (global) return *(CInstance **)RB(VA_G_GLOBAL);
    return rb_game_controller;
}
int sh_scope_get(int global, const char *name, double *num, char *str, int n) { return read_var(scope_inst(global), name, num, str, n); }
int sh_scope_set(int global, const char *name, double v) { return write_var(scope_inst(global), name, v); }

// ---- calling one of the game's routines by name, as `self`
int sh_call(void *self, const char *script, int argc, const SArg *args, double *num, char *str, int n) {
    uint64_t va = script_va(script);
    if (!va || !self) return -1;
    RValue av[6], *ap[6];
    memset(av, 0, sizeof av);
    for (int i = 0; i < argc && i < 6; i++) {
        if (args[i].kind == 1) ((void (*)(RValue *, const char *))RB(VA_RV_FROM_CSTR))(&av[i], args[i].s);
        else { av[i].real = args[i].n; av[i].kind = RV_REAL; }
        ap[i] = &av[i];
    }
    RValue res = {0}; res.kind = RV_UNDEFINED;
    ((GMLScript)RB(va))(self, self, &res, argc, ap);
    int r = 0;
    unsigned k = res.kind & 0xffffff;
    if (k == RV_STRING) { rb_rv_str(&res, str, n); r = 2; }
    else if (k != RV_UNDEFINED) { *num = rb_rv_real(&res); r = 1; }
    for (int i = 0; i < argc && i < 6; i++) if (args[i].kind == 1) ((void (*)(RValue *))RB(VA_RV_FREE))(&av[i]);
    ((void (*)(RValue *))RB(VA_RV_FREE))(&res);
    rb_log("script: called %s (%d values)", script, argc);
    return r;
}
void sh_log(const char *msg) { rb_log("script: %s", msg); }

// ---- loading
static unsigned g_hash = 1;
static int g_selftest;
static void doc_path(char *fp, size_t n) { const char *h = getenv("HOME"); snprintf(fp, n, "%s/Documents/rbmod_script.txt", h ? h : "."); }
static void self_test(void) {
    // the tables were built from this exact binary; check them against what the mod already knows
    int ok = var_va("down") == SLOT_DOWN && var_va("x") == SLOT_X && script_va("s_spike_ball") == 0x1005760bcULL;
    double a = 0, b = rb_get_obj_real(OBJ_CONTROLLER, SLOT_DOWN);
    char s[8];
    int live = rb_game_controller ? read_var(rb_game_controller, "down", &a, s, sizeof s) : -1;
    rb_log("script: self-test %s - %d game variables and %d routines by name%s%s",
           ok && (live != 1 || a == b) ? "passed" : "FAILED - scripts are off",
           (int)(sizeof RB_VARS / sizeof RB_VARS[0]), (int)(sizeof RB_SCRIPTS / sizeof RB_SCRIPTS[0]),
           live == 1 ? (a == b ? ", reading a live game value by name works" : ", reading by name gave a wrong value") : "",
           live == -1 ? " (live read checked on the next play)" : "");
    g_selftest = ok && (live != 1 || a == b) ? (live == 1 ? 2 : 1) : -1;
}
void rb_script_load(void) {
    if (g_selftest == 0 || (g_selftest == 1 && rb_game_controller)) self_test();
    char fp[1024]; doc_path(fp, sizeof fp);
    static char buf[32768];
    int n = 0;
    FILE *f = fopen(fp, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            int l = (int)strlen(line);
            if (n + l >= (int)sizeof buf - 1) break;
            memcpy(buf + n, line, (size_t)l); n += l;
        }
        fclose(f);
    }
    buf[n] = 0;
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ (unsigned char)buf[i]) * 16777619u;
    h = f ? (h | 2) : 0;
    if (h == g_hash) return;
    g_hash = h;
    rb_script_reset_vars();
    if (!f) { rb_script_parse(""); rb_log("script: no rbmod_script.txt - no scripts"); return; }
    if (g_selftest < 0) { rb_script_parse(""); rb_log("script: rbmod_script.txt found, but scripts are off (self-test failed)"); return; }
    if (rb_script_parse(buf)) {
        int line; const char *e = rb_script_error(&line);
        rb_log("script: rbmod_script.txt NOT loaded - line %d: %s", line, e);
        rb_script_parse("");
        return;
    }
    rb_log("script: rbmod_script.txt loaded - %d on blocks", rb_script_handlers());
}
int rb_script_active(void) { return rb_script_enabled_handlers() > 0; }
void rb_script_fire(const char *ev, const char *arg) { if (rb_script_enabled_handlers()) rb_script_run(ev, arg); }
