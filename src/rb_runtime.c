// rb_runtime.c - loader, pointer-table install, logging and variable helpers.
#include "rb.h"
#include "sys.h"

extern const struct mach_header *_dyld_get_image_header(uint32_t image_index);
void rb_install_hooks(void **tab);   // hooks.c

uintptr_t rb_base;

typedef void (*fn_get_inst_var)(RValue *inst, int slot, int arr, RValue *out, int a, int b);
typedef void (*fn_set_ref_var)(RValue *target, int slot, int arr, RValue *val);
typedef void (*fn_rv_free)(RValue *v);

// ---------------------------------------------------------------- logging: Documents/rbmod.log
static FILE *g_log;

void rb_log(const char *fmt, ...) {
    if (!g_log) {
        const char *home = getenv("HOME");
        if (!home) return;
        char path[1024];
        snprintf(path, sizeof path, "%s/Documents/rbmod.log", home);
        g_log = fopen(path, "a");
        if (!g_log) return;
    }
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    time_t t = time(NULL);
    fprintf(g_log, "[%ld] %s\n", (long)t, buf);
    fflush(g_log);
}

// ---------------------------------------------------------------- variables
RValue rb_obj_ref(int object_index) {
    RValue r;
    r.i64 = 0x0100000000000000LL | (int64_t)object_index;
    r.flags = 0;
    r.kind = RV_REF;
    return r;
}

// Never hand the engine's converter a value it can't convert: it raises a GML error (abort).
double rb_rv_real(RValue *v) {
    if (!v) return 0;
    switch (v->kind & 0xffffff) {
    case RV_REAL: case RV_BOOL: return v->real;
    case 7:  return (double)(int32_t)v->i64;      // int32
    case 10: return (double)v->i64;               // int64
    default: return 0;                            // undefined, string, array, struct, ...
    }
}

const char *rb_rv_str(RValue *v, char *buf, int n) {
    if (!v) return "(null)";
    switch (v->kind & 0xffffff) {
    case RV_STRING: {
        const char *s = v->ptr ? *(const char **)v->ptr : 0;   // RefString { const char *thing; ... }
        return s ? s : "(nullstr)";
    }
    case RV_REAL: case RV_BOOL: snprintf(buf, n, "%g", v->real); return buf;
    case RV_UNDEFINED: return "undefined";
    default: snprintf(buf, n, "<kind %u>", v->kind); return buf;
    }
}

static int count_cb(CInstance *inst, void *ctx) { (void)inst; (*(int *)ctx)++; return 1; }

// does at least one instance of the object exist? (reading a variable of a missing object
// raises a GML error, which aborts the app)
int rb_obj_exists(int object_index) {
    if (!rb_base) return 0;
    CInstance *g = *(CInstance **)RB(0x1010c82d8ULL);      // global instance: any valid self works
    if (!g) return 0;
    RValue ref = rb_obj_ref(object_index);
    int n = 0;
    rb_with(&ref, g, g, count_cb, &n);
    return n > 0;
}

double rb_get_obj_real(int object_index, uint64_t slot_va) {
    if (!rb_obj_exists(object_index)) return -9999.0;
    RValue ref = rb_obj_ref(object_index);
    RValue out = {0};
    out.kind = RV_UNDEFINED;
    ((fn_get_inst_var)RB(VA_GET_INST_VAR))(&ref, rb_slot(slot_va), (int)0x80000000, &out, 0, 0);
    ((fn_rv_free)RB(VA_RV_FREE))(&ref);
    double d = (out.kind == RV_UNDEFINED) ? -9999.0 : rb_rv_real(&out);
    ((fn_rv_free)RB(VA_RV_FREE))(&out);
    return d;
}

double rb_get_inst_real(CInstance *inst, uint64_t slot_va) {
    if (!inst) return -9999.0;
    typedef RValue *(*getref)(CInstance *, int);
    getref f = (getref)((*(void ***)inst)[2]);   // vtable +0x10: variable reference (read)
    RValue *v = f(inst, rb_slot(slot_va));
    if (!v || v->kind == RV_UNDEFINED) return -9999.0;
    return rb_rv_real(v);
}

typedef void (*fn_get_builtin)(CInstance *self, int slot, int arr, RValue *out);
double rb_get_builtin_real(CInstance *inst, uint64_t slot_va) {
    if (!inst) return -9999.0;
    RValue out = {0};
    out.kind = RV_UNDEFINED;
    ((fn_get_builtin)RB(VA_GET_BUILTIN))(inst, rb_slot(slot_va), (int)0x80000000, &out);
    return out.kind == RV_UNDEFINED ? -9999.0 : rb_rv_real(&out);
}

void rb_set_builtin_real(CInstance *inst, uint64_t slot_va, double val) {
    if (!inst) return;
    RValue v = {0};
    v.real = val; v.kind = RV_REAL;
    typedef void (*fn_set_builtin)(CInstance *, int, int, RValue *);
    ((fn_set_builtin)RB(VA_SET_BUILTIN))(inst, rb_slot(slot_va), (int)0x80000000, &v);
}

void rb_set_obj_real(int object_index, uint64_t slot_va, double val) {
    if (!rb_obj_exists(object_index)) return;
    RValue ref = rb_obj_ref(object_index);
    RValue v = {0};
    v.real = val;
    v.kind = RV_REAL;
    ((fn_set_ref_var)RB(VA_SET_REF_VAR))(&ref, rb_slot(slot_va), (int)0x80000000, &v);
    ((fn_rv_free)RB(VA_RV_FREE))(&ref);
}

typedef void (*fn_rv_free_content)(RValue *v);
void rb_set_inst_real(CInstance *inst, uint64_t slot_va, double val) {
    if (!inst) return;
    typedef RValue *(*getref)(CInstance *, int);
    RValue *v = ((getref)((*(void ***)inst)[3]))(inst, rb_slot(slot_va));   // vtable +0x18: for writing
    if (!v) return;
    if ((1u << (v->kind & 31)) & 0x46) ((fn_rv_free_content)RB(0x100004c9cULL))(v);   // string/array/struct
    v->real = val; v->flags = 0; v->kind = RV_REAL;
}

// ---------------------------------------------------------------- instances
typedef struct { uint8_t raw[0x40]; } WithIter;
typedef int (*fn_with_begin)(WithIter *, CInstance **, CInstance **, RValue *);
typedef int (*fn_with_next)(WithIter *, CInstance **, CInstance **);
typedef void (*fn_with_end)(WithIter *, CInstance **, CInstance **);
typedef void (*fn_with_dtor)(WithIter *);

int rb_with(RValue *target, CInstance *self, CInstance *other, int (*fn)(CInstance *, void *), void *ctx) {
    WithIter it;
    memset(&it, 0, sizeof it);
    CInstance *s = self, *o = other;
    int visited = 0;
    int n = ((fn_with_begin)RB(VA_WITH_BEGIN))(&it, &s, &o, target);
    if (n >= 1) {
        do {
            visited++;
            if (fn(s, ctx)) break;
        } while (((fn_with_next)RB(VA_WITH_NEXT))(&it, &s, &o));
    }
    ((fn_with_end)RB(VA_WITH_END))(&it, &s, &o);
    ((fn_with_dtor)RB(VA_WITH_DTOR))(&it);
    return visited;
}

RValue *rb_inst_var(CInstance *inst, uint64_t slot_va) {
    if (!inst) return 0;
    typedef RValue *(*getref)(CInstance *, int);
    return ((getref)((*(void ***)inst)[2]))(inst, rb_slot(slot_va));
}

// ---------------------------------------------------------------- load
__attribute__((constructor)) static void rb_init(void) {
    rb_base = (uintptr_t)_dyld_get_image_header(0);
    void **tab = (void **)RB(RB_TAB_VA);
    rb_log("RBMod loaded: image base %p, table %p", (void *)rb_base, (void *)tab);
    if (tab[0] != 0) {
        rb_log("table not empty (%p) - refusing to install", tab[0]);
        return;
    }
    rb_install_hooks(tab);
    tab[TAB_MAGIC] = (void *)RB_TAB_MAGIC;
    rb_log("hooks installed");
}
