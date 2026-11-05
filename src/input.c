// input.c - wraps the game's touch/mouse built-ins.
// Built-in functions live in a heap table (Function_Add): base ptr at VA_THE_FUNCTIONS, count at
// VA_THE_FUNCTIONS_N, 24-byte entries { const char *name; BuiltinFn fn; int argc; }.
// Built-in variables live in a static table at VA_BUILTIN_VARS, count at VA_BUILTIN_VARS_N,
// 32-byte entries { const char *name; BuiltinGet get; BuiltinSet set; bool settable; }.
// The GML code dispatches through these tables on every call, so swapping a pointer redirects it.
#include "rb.h"
#include "sys.h"
#include "input.h"

#define VA_THE_FUNCTIONS    0x1010926e8ULL
#define VA_THE_FUNCTIONS_N  0x1010926f0ULL
#define VA_BUILTIN_VARS     0x101348830ULL
#define VA_BUILTIN_VARS_N   0x10134c6b0ULL

typedef void (*BuiltinFn)(RValue *res, CInstance *self, CInstance *other, int argc, RValue *args);
typedef int (*BuiltinGet)(CInstance *self, int arr, RValue *out);
typedef struct { const char *name; BuiltinFn fn; int argc; int pad; } RFunction;
typedef struct { const char *name; BuiltinGet get; void *set; uint8_t settable; uint8_t pad[7]; } RBuiltinVar;

int rb_input_log = 0;           // log every change of an input value
unsigned rb_frame;              // advanced by the ball update hook
VirtualFinger rb_vf;

void rb_vf_frame(void) { rb_vf.prev_down = rb_vf.down; }

// value the virtual finger reports for each built-in (argc/args as the game passed them)
static int vf_value(const char *id, int argc, RValue *args, double *out) {
    if (!rb_vf.active) return 0;
    int dev = 0;
    if (!strncmp(id, "device_", 7) && argc > 0) dev = (int)rb_rv_real(&args[0]);
    int other_dev = dev != 0;
    int down = rb_vf.down && !other_dev, prev = rb_vf.prev_down && !other_dev;
    if (strstr(id, "check_button_pressed"))       *out = down && !prev;
    else if (strstr(id, "check_button_released")) *out = !down && prev;
    else if (strstr(id, "check_button"))          *out = down;
    else if (!strcmp(id, "mouse_button"))         *out = down ? 1 : 0;
    else if (strstr(id, "x_to_gui") || strstr(id, "raw_x") || !strcmp(id, "window_mouse_get_x")) *out = rb_vf.gui_x;
    else if (strstr(id, "y_to_gui") || strstr(id, "raw_y") || !strcmp(id, "window_mouse_get_y")) *out = rb_vf.gui_y;
    else if (strstr(id, "_x"))                    *out = rb_vf.room_x;
    else if (strstr(id, "_y"))                    *out = rb_vf.room_y;
    else return 0;
    return 1;
}

// ---------------------------------------------------------------- change-only logging
typedef struct { const char *name; double a0, a1, val; int used; } Seen;
static Seen g_seen[64];

static void note(const char *name, int argc, RValue *args, double val) {
    double a0 = argc > 0 ? rb_rv_real(&args[0]) : 0, a1 = argc > 1 ? rb_rv_real(&args[1]) : 0;
    for (int i = 0; i < 64; i++) {
        Seen *s = &g_seen[i];
        if (s->used && s->name == name && s->a0 == a0 && s->a1 == a1) {
            if (s->val == val) return;
            s->val = val;
            rb_log("in f%u %s(%g,%g) = %g", rb_frame, name, a0, a1, val);
            return;
        }
        if (!s->used) {
            s->used = 1; s->name = name; s->a0 = a0; s->a1 = a1; s->val = val;
            rb_log("in f%u %s(%g,%g) = %g (first)", rb_frame, name, a0, a1, val);
            return;
        }
    }
}

// ---------------------------------------------------------------- wrappers
#define WRAP_FN(id)                                                                         \
    static BuiltinFn orig_##id;                                                             \
    static void wrap_##id(RValue *res, CInstance *self, CInstance *other, int argc, RValue *args) { \
        double v;                                                                           \
        if (vf_value(#id, argc, args, &v)) { res->real = v; res->flags = 0; res->kind = RV_REAL; return; } \
        orig_##id(res, self, other, argc, args);                                            \
        if (rb_input_log) note(#id, argc, args, rb_rv_real(res));                           \
    }
#define WRAP_VAR(id)                                                                        \
    static BuiltinGet origv_##id;                                                           \
    static int wrapv_##id(CInstance *self, int arr, RValue *out) {                          \
        double v;                                                                           \
        if (vf_value(#id, 0, 0, &v)) { out->real = v; out->flags = 0; out->kind = RV_REAL; return 1; } \
        int r = origv_##id(self, arr, out);                                                 \
        if (rb_input_log) note(#id, 0, 0, rb_rv_real(out));                                 \
        return r;                                                                           \
    }

#define INPUT_FNS(X) X(mouse_check_button) X(mouse_check_button_pressed) X(mouse_check_button_released) \
    X(device_mouse_check_button) X(device_mouse_check_button_pressed) X(device_mouse_check_button_released) \
    X(device_mouse_x) X(device_mouse_y) X(device_mouse_raw_x) X(device_mouse_raw_y) \
    X(device_mouse_x_to_gui) X(device_mouse_y_to_gui) X(window_mouse_get_x) X(window_mouse_get_y)
#define INPUT_VARS(X) X(mouse_x) X(mouse_y) X(mouse_button)

INPUT_FNS(WRAP_FN)
INPUT_VARS(WRAP_VAR)

static int g_installed;

int rb_input_install(void) {
    if (g_installed) return 1;
    RFunction *fns = *(RFunction **)RB(VA_THE_FUNCTIONS);
    int nf = *(int *)RB(VA_THE_FUNCTIONS_N);
    RBuiltinVar *vars = (RBuiltinVar *)RB(VA_BUILTIN_VARS);
    int nv = *(int *)RB(VA_BUILTIN_VARS_N);
    if (!fns || nf < 100 || nv < 50) return 0;        // runtime not set up yet
    int found = 0;
    for (int i = 0; i < nf; i++) {
        if (!fns[i].name) continue;
#define SWAP_FN(id) if (!strcmp(fns[i].name, #id) && !orig_##id) { orig_##id = fns[i].fn; fns[i].fn = wrap_##id; found++; }
        INPUT_FNS(SWAP_FN)
    }
    for (int i = 0; i < nv; i++) {
        if (!vars[i].name) continue;
#define SWAP_VAR(id) if (!strcmp(vars[i].name, #id) && !origv_##id) { origv_##id = vars[i].get; vars[i].get = wrapv_##id; found++; }
        INPUT_VARS(SWAP_VAR)
    }
    g_installed = 1;
    rb_log("input: %d functions, %d variables in runtime; wrapped %d", nf, nv, found);
    return 1;
}
