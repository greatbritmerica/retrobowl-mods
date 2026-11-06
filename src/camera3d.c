// camera3d.c - experimental behind-the-play 3D camera.
// The game keeps framing the action with its normal 2D camera (s_update_camera). Right after, we
// replace that camera's matrices with a perspective view: eye low behind the play, looking downfield.
// The field is the z=0 plane in room coordinates (x downfield, y sideline to sideline). Layer depths
// are flattened (z scaled to 0) so nothing floats; draw order keeps doing the sorting.
// The view box is enlarged while drawing so the engine doesn't cull the far field, and restored
// before the game's next camera update. Taps are re-projected onto the field (mouse_x/mouse_y).
#include "rb.h"
#include "sys.h"
#include "input.h"
#include "camera3d.h"

int rb_cam3d_on = 1;

#define CAM_BACK    260.0     // eye distance behind the focus point
#define CAM_HEIGHT  150.0     // eye height above the field
#define CAM_AHEAD   220.0     // look this far downfield of the focus
#define CAM_FOV     60.0
#define VIEW_GROW   4.0       // enlarge the culling box this much while drawing

typedef struct { double v[3]; } V3;
static V3 v3(double x, double y, double z) { V3 r = {{x, y, z}}; return r; }
static V3 sub(V3 a, V3 b) { return v3(a.v[0]-b.v[0], a.v[1]-b.v[1], a.v[2]-b.v[2]); }
static V3 add(V3 a, V3 b) { return v3(a.v[0]+b.v[0], a.v[1]+b.v[1], a.v[2]+b.v[2]); }
static V3 mul(V3 a, double k) { return v3(a.v[0]*k, a.v[1]*k, a.v[2]*k); }
static V3 cross(V3 a, V3 b) { return v3(a.v[1]*b.v[2]-a.v[2]*b.v[1], a.v[2]*b.v[0]-a.v[0]*b.v[2], a.v[0]*b.v[1]-a.v[1]*b.v[0]); }
static V3 norm(V3 a) { double l = sqrt(a.v[0]*a.v[0]+a.v[1]*a.v[1]+a.v[2]*a.v[2]); return l > 0 ? mul(a, 1/l) : a; }

static struct {
    int valid;
    double gx, gy, gw, gh;          // the game's own view box (restored before its next update)
    V3 eye, f, r, u;                 // camera basis (f forward, r screen-right, u screen-up)
    double tan_half, aspect, fwd;
} C;

static void *fn(const char *name) {          // cached by the literal's address
    static const char *names[24]; static void *fns[24];
    for (int i = 0; i < 24; i++) {
        if (names[i] == name) return fns[i];
        if (!names[i]) { names[i] = name; fns[i] = rb_builtin(name); return fns[i]; }
    }
    return rb_builtin(name);
}
static unsigned g_reads_since_update;        // taps read since the camera last updated

typedef void (*BuiltinFn)(RValue *res, CInstance *self, CInstance *other, int argc, RValue *args);
static void callrv(void *f, CInstance *self, int argc, RValue *args, RValue *out) {
    out->i64 = 0; out->flags = 0; out->kind = RV_UNDEFINED;
    if (f) ((BuiltinFn)f)(out, self, self, argc, args);
}
static RValue R(double d) { RValue r; r.real = d; r.flags = 0; r.kind = RV_REAL; return r; }
static void rvfree(RValue *v) { typedef void (*F)(RValue *); ((F)RB(VA_RV_FREE))(v); }

static double callreal(const char *name, CInstance *self, int argc, double a0, double a1) {
    RValue args[2] = { R(a0), R(a1) }, out;
    callrv(fn(name), self, argc, args, &out);
    double d = rb_rv_real(&out);
    rvfree(&out);
    return d;
}

static double drive_dir(void) {
    double d = rb_get_obj_real(OBJ_CONTROLLER, 0x100fa4d48ULL);       // drivedir
    if (d > -9000 && d != 0) return d > 0 ? 1 : -1;
    d = rb_get_obj_real(35 /* obj_camera */, 0x100fa4d48ULL);
    if (d > -9000 && d != 0) return d > 0 ? 1 : -1;
    return 1;
}

// before the game's camera update: give it back its own view box
void rb_cam3d_before(CInstance *self) {
    if (!rb_cam3d_on || !C.valid) return;
    double cam = callreal("view_get_camera", self, 1, 0, 0);
    double a[3] = { cam, C.gx, C.gy };
    rb_call(fn("camera_set_view_pos"), self, 3, a);
    double b[3] = { cam, C.gw, C.gh };
    rb_call(fn("camera_set_view_size"), self, 3, b);
}

// after the game's camera update: perspective view of the same spot
void rb_cam3d_after(CInstance *self) {
    if (!rb_cam3d_on) return;
    double cam = callreal("view_get_camera", self, 1, 0, 0);
    double x = callreal("camera_get_view_x", self, 1, cam, 0), y = callreal("camera_get_view_y", self, 1, cam, 0);
    double w = callreal("camera_get_view_width", self, 1, cam, 0), h = callreal("camera_get_view_height", self, 1, cam, 0);
    if (!(w > 1 && h > 1)) return;
    C.gx = x; C.gy = y; C.gw = w; C.gh = h; C.valid = 1; g_reads_since_update = 0;
    double cx = x + w / 2, cy = y + h / 2, fwd = drive_dir();

    // bigger box so far-field tiles aren't culled
    double W = w * VIEW_GROW, H = h * VIEW_GROW;
    double ps[3] = { cam, cx - W / 2, cy - H / 2 }, ss[3] = { cam, W, H };
    rb_call(fn("camera_set_view_size"), self, 3, ss);
    rb_call(fn("camera_set_view_pos"), self, 3, ps);

    V3 eye = v3(cx - fwd * CAM_BACK, cy, -CAM_HEIGHT), at = v3(cx + fwd * CAM_AHEAD, cy, 0);
    RValue la[9] = { R(eye.v[0]), R(eye.v[1]), R(eye.v[2]), R(at.v[0]), R(at.v[1]), R(at.v[2]), R(0), R(0), R(1) };
    RValue look, flat, view, proj, tmp;
    callrv(fn("matrix_build_lookat"), self, 9, la, &look);
    RValue fb[9] = { R(0), R(0), R(0), R(0), R(0), R(0), R(1), R(1), R(0) };      // squash layer depth
    callrv(fn("matrix_build"), self, 9, fb, &flat);
    RValue mm[2] = { flat, look };
    callrv(fn("matrix_multiply"), self, 2, mm, &view);
    C.aspect = w / h;
    RValue pp[4] = { R(CAM_FOV), R(C.aspect), R(1), R(32000) };
    callrv(fn("matrix_build_projection_perspective_fov"), self, 4, pp, &proj);
    RValue sv[2] = { R(cam), view }, sp[2] = { R(cam), proj };
    callrv(fn("camera_set_view_mat"), self, 2, sv, &tmp); rvfree(&tmp);
    callrv(fn("camera_set_proj_mat"), self, 2, sp, &tmp); rvfree(&tmp);
    rvfree(&look); rvfree(&flat); rvfree(&view); rvfree(&proj);

    // basis for re-projecting taps
    C.eye = eye; C.fwd = fwd;
    C.f = norm(sub(at, eye));
    C.r = norm(cross(C.f, v3(0, 0, -1)));     // screen right
    C.u = cross(C.r, C.f);                     // screen up (toward -z, away from the field)
    C.tan_half = tan(CAM_FOV / 2 / 57.29577951308232);
}

// field point under a GUI-space touch; returns 0 if the ray misses the field
int rb_cam3d_ground(double gui_x, double gui_y, double gui_w, double gui_h, double *rx, double *ry) {
    if (!rb_cam3d_on || !C.valid || gui_w <= 0 || gui_h <= 0) return 0;
    if (++g_reads_since_update > 400) { C.valid = 0; return 0; }   // left the match: hands off
    double nx = gui_x / gui_w * 2 - 1, ny = 1 - gui_y / gui_h * 2;
    V3 d = add(C.f, add(mul(C.r, nx * C.tan_half * C.aspect), mul(C.u, ny * C.tan_half)));
    if (d.v[2] <= 1e-6) return 0;              // pointing at the sky
    double t = -C.eye.v[2] / d.v[2];
    *rx = C.eye.v[0] + d.v[0] * t;
    *ry = C.eye.v[1] + d.v[1] * t;
    return 1;
}
