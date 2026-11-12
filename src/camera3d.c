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

#ifndef RB_CAM3D
#define RB_CAM3D 0  // off until the camera gets its full pass; build with RB_CAM3D=1 to try it
#endif
int rb_cam3d_on = RB_CAM3D;

#define CAM_BACK    240.0     // eye distance behind the ball carrier (backfield stays in view)
#define CAM_HEIGHT  125.0     // eye height above the field
#define CAM_AHEAD   240.0     // look this far downfield of the ball
#define CAM_FOV     60.0
// view box x4 + sky/bleachers on (cfg 0). The A/B speed test showed no difference between settings.
int rb_cam3d_cfg;
unsigned rb_spr_calls;
#define VIEW_GROW   ((rb_cam3d_cfg & 2) ? 1.6 : 4.0)
#define STANDS_ON   (!(rb_cam3d_cfg & 1))

typedef struct { double v[3]; } V3;
static V3 v3(double x, double y, double z) { V3 r = {{x, y, z}}; return r; }
static V3 sub(V3 a, V3 b) { return v3(a.v[0]-b.v[0], a.v[1]-b.v[1], a.v[2]-b.v[2]); }
static V3 add(V3 a, V3 b) { return v3(a.v[0]+b.v[0], a.v[1]+b.v[1], a.v[2]+b.v[2]); }
static V3 mul(V3 a, double k) { return v3(a.v[0]*k, a.v[1]*k, a.v[2]*k); }
static V3 cross(V3 a, V3 b) { return v3(a.v[1]*b.v[2]-a.v[2]*b.v[1], a.v[2]*b.v[0]-a.v[0]*b.v[2], a.v[0]*b.v[1]-a.v[1]*b.v[0]); }
static V3 norm(V3 a) { double l = sqrt(a.v[0]*a.v[0]+a.v[1]*a.v[1]+a.v[2]*a.v[2]); return l > 0 ? mul(a, 1/l) : a; }

static struct { V3 at; int have; unsigned frame; } M;   // this frame's camera target

static struct {
    int valid;
    double gx, gy, gw, gh;          // the game's own view box (restored before its next update)
    V3 eye, f, r, u;                 // camera basis (f forward, r screen-right, u screen-up)
    double tan_half, aspect, fwd;
} C;

extern uint64_t mach_absolute_time(void);
static uint64_t g_t_frame, g_t_gui, g_t_world, g_t_draw, g_t_after_end; static unsigned g_t_frames, g_t_n, g_t_sprites;

static void *fn(const char *name) {          // cached by the literal's address
    static const char *names[64]; static void *fns[64];
    for (int i = 0; i < 64; i++) {
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

typedef struct { double x, y; int found; } BallCtx;
static int ball_cb(CInstance *inst, void *ctx) {
    BallCtx *b = ctx;
    if (rb_get_inst_real(inst, SLOT_STATE) < -9000) return 0;   // the master ball has a state
    b->x = rb_get_builtin_real(inst, SLOT_X); b->y = rb_get_builtin_real(inst, SLOT_Y); b->found = 1;
    return 1;
}
static int ball_pos(CInstance *self, double *x, double *y) {
    if (!rb_obj_exists(OBJ_BALL)) return 0;
    BallCtx b = { 0, 0, 0 };
    RValue r = rb_obj_ref(OBJ_BALL);
    rb_with(&r, self, self, ball_cb, &b);
    if (!b.found || b.x < -9000) return 0;
    *x = b.x; *y = b.y;
    return 1;
}

// what the camera follows: whoever holds the ball (QB on the dropback, then the receiver or runner).
// Returns 1 with a position, 2 if the ball is in the air / loose (hold still), 0 if unknown.
static int grab_cb(CInstance *inst, void *ctx) { *(CInstance **)ctx = inst; return 1; }
static int focus_pos(CInstance *self, double *x, double *y) {
    CInstance *g = *(CInstance **)RB(0x1010c82d8ULL);               // global instance
    if (!g) return 0;
    RValue *mb = rb_inst_var(g, SLOT_MASTER_BALL);
    if (!mb || mb->kind == RV_UNDEFINED) return 0;
    RValue c = *mb; CInstance *ball = 0;
    rb_with(&c, self, self, grab_cb, &ball);
    if (!ball) return 0;
    RValue *h = rb_inst_var(ball, SLOT_HOLDER);
    if (!h || h->kind == RV_UNDEFINED || (h->kind == RV_REAL && h->real < 0)) return 2;
    RValue hc = *h; CInstance *holder = 0;
    rb_with(&hc, self, self, grab_cb, &holder);
    if (!holder) return 2;
    double hx = rb_get_builtin_real(holder, SLOT_X), hy = rb_get_builtin_real(holder, SLOT_Y);
    if (hx < -9000) return 2;
    *x = hx; *y = hy;
    return 1;
}

void rb_cam3d_log_layers(CInstance *self);

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
    static unsigned calls;
    if (calls++ % 600 == 0)
        rb_log("cam3d: call %u cam %g view %.0f,%.0f %.0fx%.0f fns %p %p %p dir %g", calls, cam, x, y, w, h,
               fn("matrix_build_lookat"), fn("camera_set_view_mat"), fn("matrix_build_projection_perspective_fov"), drive_dir());
    if (!(w > 1 && h > 1)) return;
    if (!C.valid) rb_cam3d_log_layers(self);
    C.gx = x; C.gy = y; C.gw = w; C.gh = h; C.valid = 1; g_reads_since_update = 0;
    double cx = x + w / 2, cy = y + h / 2, fwd = drive_dir();
    double bx, by;
    static double sx, sy; static int have;
    int fr = focus_pos(self, &bx, &by);
    if (fr == 0 && ball_pos(self, &bx, &by)) fr = 1;           // fallback: the ball itself
    if (fr == 1) {                                             // follow the ball carrier, smoothly
        if (!have || hypot(bx - sx, by - sy) > 600) { sx = bx; sy = by; have = 1; }
        sx += (bx - sx) * 0.15; sy += (by - sy) * 0.15;
    }
    if (have) { cx = sx; cy = sy; }                            // ball in the air: stay behind the play

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
    // (GML arrays are garbage-collected: never keep one past this call; the world pass rebuilds what it needs)
    M.at = at; M.have = 1;
    M.frame++;
    {                                                                  // frame timing
        static uint64_t last; uint64_t now = mach_absolute_time();
        if (last && now > last && now - last < 24000ULL * 1000) { g_t_frame += now - last; g_t_frames++; }
        last = now;
        if (++g_t_n == 300) {                                 // ticks are 1/24 us on iPhone
            static const char *cfgname[4] = { "box x4, stands on", "box x4, stands OFF", "box x1.6, stands on", "box x1.6, stands OFF" };
            rb_log("cam3d: [%s] %.1f fps | draw cpu %.2f ms (our screen pass %.2f, sky+stands %.2f) | %u sprites, %u sprite draws/frame",
                   cfgname[rb_cam3d_cfg & 3], g_t_frames && g_t_frame ? 24000000.0 * g_t_frames / (double)g_t_frame : 0,
                   g_t_draw / 300.0 / 24000.0, g_t_gui / 300.0 / 24000.0, g_t_world / 300.0 / 24000.0, g_t_sprites, rb_spr_calls / 300);
            g_t_gui = g_t_world = g_t_draw = 0; g_t_n = 0; g_t_frame = 0; g_t_frames = 0; rb_spr_calls = 0;
        }
        g_t_after_end = mach_absolute_time();
    }
    if (calls % 600 == 1) {
        RValue a1[1] = { R(cam) }, vm;
        callrv(fn("camera_get_view_mat"), self, 1, a1, &vm);
        rb_log("cam3d: view matrix after set: kind %u", vm.kind);
        rvfree(&vm);
    }

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

// ---------------------------------------------------------------- standing sprites (screen-layer pass)
// Players and the ball are not drawn flat on the field. Instead, just before the HUD, each one's own
// draw code is re-run on the screen layer with a transform that puts its feet where it stands in 3D
// and scales it by distance, far to near. Height drawn upward in 2D (jumps, a thrown ball) stays up.
int rb_cam3d_in_gui_pass;

int rb_cam3d_active(void) { return rb_cam3d_on && C.valid && g_reads_since_update < 400; }

static int project3(double wx, double wy, double wz, double gw, double gh, double *sx, double *sy, double *depth) {
    V3 d = sub(v3(wx, wy, wz), C.eye);
    double zc = d.v[0]*C.f.v[0] + d.v[1]*C.f.v[1] + d.v[2]*C.f.v[2];
    if (zc < 5) return 0;
    double xs = (d.v[0]*C.r.v[0] + d.v[1]*C.r.v[1] + d.v[2]*C.r.v[2]) / (zc * C.tan_half * C.aspect);
    double ys = (d.v[0]*C.u.v[0] + d.v[1]*C.u.v[1] + d.v[2]*C.u.v[2]) / (zc * C.tan_half);
    *sx = (xs + 1) / 2 * gw; *sy = (1 - ys) / 2 * gh; *depth = zc;
    return 1;
}
static int project(double wx, double wy, double gw, double gh, double *sx, double *sy, double *depth) {
    return project3(wx, wy, 0, gw, gh, sx, sy, depth);
}

typedef struct { CInstance *inst; double depth, sx, sy, scale; int is_ball; int offense; } Spr;   // is_ball: 0 player, 1 ball, 2 field text
typedef struct { Spr *s; int n, cap, is_ball; double gw, gh; } SprList;

#define SCALE_MAX 2.4      // players right under the camera don't get huge
#define NEAR_CULL 20.0
static Spr g_last[48]; static int g_nlast; static double g_last_gw, g_last_gh;   // last frame's standing players (for taps)
static int spr_cb(CInstance *inst, void *ctx) {
    SprList *L = ctx;
    if (L->n >= L->cap) return 1;
    double x = rb_get_builtin_real(inst, SLOT_X), y = rb_get_builtin_real(inst, SLOT_Y), sx, sy, z;
    if (x < -9000 || !project(x, y, L->gw, L->gh, &sx, &sy, &z) || z < NEAR_CULL) return 0;
    if (sx < -200 || sx > L->gw + 200 || sy < -200 || sy > L->gh + 400) return 0;
    Spr *p = &L->s[L->n++];
    p->inst = inst; p->depth = z; p->sx = sx; p->sy = sy; p->is_ball = L->is_ball;
    p->offense = L->is_ball == 0 && rb_get_inst_real(inst, SLOT_OFFENSE) > 0.5;
    p->scale = (L->gh / 2) / (z * C.tan_half);
    if (p->scale > SCALE_MAX) p->scale = SCALE_MAX;
    return 0;
}

static void set_world(CInstance *self, double x, double y, double s, double sx, double sy) {
    RValue a[9] = { R(-x), R(-y), R(0), R(0), R(0), R(0), R(1), R(1), R(0) }, b[9] = { R(sx), R(sy), R(0), R(0), R(0), R(0), R(s), R(s), R(0) };
    RValue m1, m2, m, tmp;
    callrv(fn("matrix_build"), self, 9, a, &m1);
    callrv(fn("matrix_build"), self, 9, b, &m2);
    RValue mm[2] = { m1, m2 };
    callrv(fn("matrix_multiply"), self, 2, mm, &m);
    RValue sm[2] = { R(2), m };                          // matrix_world
    callrv(fn("matrix_set"), self, 2, sm, &tmp); rvfree(&tmp);
    rvfree(&m1); rvfree(&m2); rvfree(&m);
}
static void reset_world(CInstance *self) {
    RValue id, tmp;
    callrv(fn("matrix_build_identity"), self, 0, 0, &id);
    RValue sm[2] = { R(2), id };
    callrv(fn("matrix_set"), self, 2, sm, &tmp); rvfree(&tmp);
    rvfree(&id);
}

void rb_cam3d_gui_pass(CInstance *self, void (*draw_player)(CInstance *), void (*draw_ball)(CInstance *), void (*draw_text)(CInstance *)) {
    if (!rb_cam3d_active()) return;
    uint64_t t0 = mach_absolute_time();
    double gw = callreal("display_get_gui_width", self, 0, 0, 0), gh = callreal("display_get_gui_height", self, 0, 0, 0);
    if (!(gw > 1 && gh > 1)) return;
    static Spr buf[48];
    SprList L = { buf, 0, 48, 0, gw, gh };
    RValue r = rb_obj_ref(OBJ_PLAYER);
    if (rb_obj_exists(OBJ_PLAYER)) rb_with(&r, self, self, spr_cb, &L);
    int nplayers = L.n;
    L.is_ball = 1;
    r = rb_obj_ref(OBJ_BALL);
    if (rb_obj_exists(OBJ_BALL)) rb_with(&r, self, self, spr_cb, &L);
    L.is_ball = 2;
    r = rb_obj_ref(59);                                    // obj_fieldtxt ("GET READY!", names)
    if (rb_obj_exists(59)) rb_with(&r, self, self, spr_cb, &L);
    memcpy(g_last, buf, sizeof(Spr) * nplayers); g_nlast = nplayers; g_last_gw = gw; g_last_gh = gh;
    for (int i = 1; i < L.n; i++) {                      // far to near
        Spr k = buf[i]; int j = i - 1;
        while (j >= 0 && buf[j].depth < k.depth) { buf[j + 1] = buf[j]; j--; }
        buf[j + 1] = k;
    }
    rb_cam3d_in_gui_pass = 1;
    for (int i = 0; i < L.n; i++) {
        Spr *p = &buf[i];
        double x = rb_get_builtin_real(p->inst, SLOT_X), y = rb_get_builtin_real(p->inst, SLOT_Y);
        set_world(self, x, y, p->scale, p->sx, p->sy);
        if (p->is_ball == 1) draw_ball(p->inst); else if (p->is_ball == 2) draw_text(p->inst); else draw_player(p->inst);
    }
    reset_world(self);
    rb_cam3d_in_gui_pass = 0;
    uint64_t t1 = mach_absolute_time();
    g_t_gui += t1 - t0; g_t_sprites = L.n;
    if (g_t_after_end && t1 > g_t_after_end) g_t_draw += t1 - g_t_after_end;
}

double rb_cam3d_fwd(void) { return C.fwd ? C.fwd : 1; }

// a screen drag (dx,dy) in the 3D view -> the drag the top-down game expects
// (screen up = downfield = +fwd room x; screen right = +fwd room y)
void rb_cam3d_rotate_drag(double dx, double dy, double *ox, double *oy) {
    double f = rb_cam3d_fwd();
    *ox = -f * dy;
    *oy = f * dx;
}

// a tap on a standing player's body counts as a tap on that player (feet position)
int rb_cam3d_hit_player(double gx, double gy, double *rx, double *ry) {
    if (!rb_cam3d_active()) return 0;
    int best = -1; double bd = 1e9;
    for (int i = 0; i < g_nlast; i++) {
        Spr *p = &g_last[i];
        double hw = 11 * p->scale, top = 30 * p->scale, bot = 5 * p->scale;
        if (gx < p->sx - hw || gx > p->sx + hw || gy < p->sy - top || gy > p->sy + bot) continue;
        if (p->depth < bd) { bd = p->depth; best = i; }          // front-most
    }
    if (best < 0) return 0;
    *rx = rb_get_builtin_real(g_last[best].inst, SLOT_X);
    *ry = rb_get_builtin_real(g_last[best].inst, SLOT_Y);
    return *rx > -9000;
}

// once per match: log the room's layers (to find the crowd/stands)
static void *fnp(const char *n) { return fn(n); }
void rb_cam3d_log_layers(CInstance *self) {
    RValue all; callrv(fnp("layer_get_all"), self, 0, 0, &all);
    RValue a1[1] = { all }, n;
    callrv(fnp("array_length"), self, 1, a1, &n);
    int cnt = (int)rb_rv_real(&n);
    rb_log("cam3d: %d layers", cnt);
    for (int i = 0; i < cnt && i < 60; i++) {
        RValue ga[2] = { all, R(i) }, id, nm, dp, vis;
        callrv(fnp("array_get"), self, 2, ga, &id);
        RValue ia[1] = { id };
        callrv(fnp("layer_get_name"), self, 1, ia, &nm);
        callrv(fnp("layer_get_depth"), self, 1, ia, &dp);
        callrv(fnp("layer_get_visible"), self, 1, ia, &vis);
        char b[64];
        rb_log("cam3d: layer %g \"%s\" depth %g visible %g", rb_rv_real(&id), rb_rv_str(&nm, b, sizeof b), rb_rv_real(&dp), rb_rv_real(&vis));
        rvfree(&nm);
    }
    rvfree(&all);
}

// ---------------------------------------------------------------- sky and bleachers (world pass)
// Every sprite draw goes through the engine's draw_sprite_ext routine (hooked). In 3D:
//  - right before the field image, the sky is painted behind everything (screen-aligned);
//  - the crowd strips (spr_fieldfans: fans, railings) are drawn with a world matrix that hinges them
//    up off the ground along the edge nearest the field, like bleachers. Those draws use the real
//    (unflattened) view so the GPU does the perspective.
#define SPR_FIELD_CO   201
#define SPR_FIELD      202
#define SPR_FANS_CO    203
#define SPR_FANS       204
#define FANS_H         33.0
#define FANS_W         1300.0
#define FIELD_H        237.0
#define STAND_ANGLE    62.0

static struct {
    unsigned sky_frame, cur_frame;
    double fy, fh;                    // field image top / height in the room
    double top_fold, bot_fold;        // hinge lines (from the last frame's strips)
    double cur_top, cur_bot;
    int rs;                           // rotation sign that tilts the crowd up (toward -z)
    int nlog;
} B = { 0, 0, 0, 2 * FIELD_H, -1e9, 1e9, -1e9, 1e9, 0, 0 };

static void mset(CInstance *self, int which, RValue m) {
    RValue a[2] = { R(which), m }, t;
    callrv(fn("matrix_set"), self, 2, a, &t); rvfree(&t);
}
static void mget(CInstance *self, int which, RValue *out) {
    RValue a[1] = { R(which) };
    callrv(fn("matrix_get"), self, 1, a, out);
}
static RValue mbuild(CInstance *self, double x, double y, double z, double rx, double sx, double sy, double sz) {
    RValue a[9] = { R(x), R(y), R(z), R(rx), R(0), R(0), R(sx), R(sy), R(sz) }, m;
    callrv(fn("matrix_build"), self, 9, a, &m);
    return m;
}

static void draw_sky(CInstance *self) {
    double gw = g_last_gw, gh = g_last_gh;
    if (!(gw > 1 && gh > 1)) { gw = callreal("display_get_gui_width", self, 0, 0, 0); gh = callreal("display_get_gui_height", self, 0, 0, 0); }
    if (!(gw > 1 && gh > 1)) return;
    double hx, hy, hz, ex, ey, ez;
    if (!project(C.eye.v[0] + C.fwd * 30000, C.eye.v[1], gw, gh, &hx, &hy, &hz) || hy <= 0) return;
    double room_w = rb_get_builtin_real(self, 0x100fb0aa8ULL);          // room_width
    double far_x = C.fwd > 0 ? (room_w > 0 ? room_w : 4000) : 0;      // the stadium ends here
    if (project(far_x, C.eye.v[1], gw, gh, &ex, &ey, &ez) && ey > hy) hy = ey;
    RValue la[9] = { R(gw / 2), R(gh / 2), R(-10), R(gw / 2), R(gh / 2), R(0), R(0), R(1), R(0) }, v2, p2;
    callrv(fn("matrix_build_lookat"), self, 9, la, &v2);
    RValue po[4] = { R(gw), R(gh), R(1), R(32000) };
    callrv(fn("matrix_build_projection_ortho"), self, 4, po, &p2);
    RValue sv, sp; mget(self, 0, &sv); mget(self, 1, &sp);
    if (v2.kind == RV_UNDEFINED || p2.kind == RV_UNDEFINED || sv.kind == RV_UNDEFINED || sp.kind == RV_UNDEFINED) {
        rvfree(&v2); rvfree(&p2); rvfree(&sv); rvfree(&sp); return;
    }
    mset(self, 0, v2); mset(self, 1, p2);
    double top = 0xE0A060, low = 0xF8D8A8;            // BGR: sky blue -> pale at the horizon
    double a[9] = { 0, 0, gw, hy + 2, top, top, low, low, 0 };
    rb_call(fn("draw_rectangle_colour"), self, 9, a);
    double grass = 0x3C6A44;                          // BGR: dark grass around the stadium (no more black void)
    double g[9] = { 0, hy + 1, gw, gh, grass, grass, grass, grass, 0 };
    rb_call(fn("draw_rectangle_colour"), self, 9, g);
    mset(self, 0, sv); mset(self, 1, sp);
    rvfree(&v2); rvfree(&p2); rvfree(&sv); rvfree(&sp);
}

static double array_real(CInstance *self, RValue arr, int i) {
    RValue ga[2] = { arr, R(i) }, e;
    callrv(fn("array_get"), self, 2, ga, &e);
    double d = rb_rv_real(&e);
    rvfree(&e);
    return d;
}

// draw one strip hinged up off the ground along the line y = F (ang: tilt, sign picks the side)
static long draw_hinged(RbDrawSprFn orig, CInstance *self, long sprite, long colour, float sub, float x, float y,
                        float xs, float ys, float rot, float alpha, double F, double ang) {
    long r;
    RValue m1 = mbuild(self, 0, -F, 0, 0, 1, 1, 0);          // flatten layer depth, hinge line -> y=0
    RValue m2 = mbuild(self, 0, F, 0, ang, 1, 1, 1);         // stand up, back into place
    RValue mm[2] = { m1, m2 }, w;
    callrv(fn("matrix_multiply"), self, 2, mm, &w);
    RValue la[9] = { R(C.eye.v[0]), R(C.eye.v[1]), R(C.eye.v[2]), R(M.at.v[0]), R(M.at.v[1]), R(M.at.v[2]), R(0), R(0), R(1) };
    RValue look, oldv, oldw;
    callrv(fn("matrix_build_lookat"), self, 9, la, &look);
    mget(self, 0, &oldv); mget(self, 2, &oldw);
    if (look.kind == RV_UNDEFINED || w.kind == RV_UNDEFINED || oldv.kind == RV_UNDEFINED || oldw.kind == RV_UNDEFINED) {
        r = orig(self, sprite, colour, sub, x, y, xs, ys, rot, alpha);       // something missing: draw it flat
    } else {
        mset(self, 0, look); mset(self, 2, w);
        r = orig(self, sprite, colour, sub, x, y, xs, ys, rot, alpha);
        mset(self, 2, oldw); mset(self, 0, oldv);
    }
    rvfree(&m1); rvfree(&m2); rvfree(&w); rvfree(&look); rvfree(&oldv); rvfree(&oldw);
    return r;
}

long rb_cam3d_spr_draw(RbDrawSprFn orig, CInstance *self, long sprite, long colour, float sub, float x, float y,
                       float xs, float ys, float rot, float alpha) {
    int fans = sprite == SPR_FANS || sprite == SPR_FANS_CO, field = sprite == SPR_FIELD || sprite == SPR_FIELD_CO;
    if ((!fans && !field) || !M.have || M.frame < 10 || !STANDS_ON) return orig(self, sprite, colour, sub, x, y, xs, ys, rot, alpha);
    uint64_t t0 = mach_absolute_time();
    static int crumb;
    #define CRUMB(n, msg) do { if (crumb < (n)) { crumb = (n); rb_log("cam3d: stage %d %s (spr %ld self %p)", n, msg, sprite, (void *)self); } } while (0)
    CRUMB(1, "first field/crowd draw");
    if (!self) self = *(CInstance **)RB(0x1010c82d8ULL);      // global instance
    if (B.cur_frame != M.frame) {                     // new frame: last frame's strips set the hinges
        if (B.cur_top > -1e8) B.top_fold = B.cur_top;
        if (B.cur_bot < 1e8) B.bot_fold = B.cur_bot;
        B.cur_top = -1e9; B.cur_bot = 1e9; B.cur_frame = M.frame;
    }
    if (B.nlog < 40 && (M.frame > 30)) {
        B.nlog++;
        rb_log("cam3d: draw spr %ld sub %g at %.0f,%.0f x%g,%g rot %g a %g (folds %.0f/%.0f)", sprite, sub, x, y, xs, ys, rot, alpha, B.top_fold, B.bot_fold);
    }
    long r;
    if (field) {
        if (B.sky_frame != M.frame) { B.sky_frame = M.frame; CRUMB(2, "sky"); draw_sky(self); CRUMB(3, "sky done"); }
        B.fy = y; B.fh = FIELD_H * (ys > 0 ? ys : -ys);
        r = orig(self, sprite, colour, sub, x, y, xs, ys, rot, alpha);
        CRUMB(4, "field drawn");
    } else {
        CRUMB(5, "crowd strip");
        if (!B.rs) {                                   // which way matrix_build turns (0,-1,0) up
            RValue m = mbuild(self, 0, 0, 0, STAND_ANGLE, 1, 1, 1), v;
            RValue tv[4] = { m, R(0), R(-1), R(0) };
            callrv(fn("matrix_transform_vertex"), self, 4, tv, &v);
            double z = v.kind == RV_UNDEFINED ? 0 : array_real(self, v, 2);
            B.rs = z > 0 ? -1 : 1;
            rb_log("cam3d: bleacher rotation sign %d (z %g, kind %u)", B.rs, z, v.kind);
            rvfree(&v); rvfree(&m);
        }
        double h = FANS_H * ys, lo = h > 0 ? y : y + h, hi = h > 0 ? y + h : y;
        double mid = B.fy + B.fh / 2, F, ang;
        if ((lo + hi) / 2 < mid) { if (hi > B.cur_top) B.cur_top = hi; F = B.top_fold > -1e8 ? B.top_fold : hi; ang = B.rs * STAND_ANGLE; }
        else                     { if (lo < B.cur_bot) B.cur_bot = lo; F = B.bot_fold < 1e8 ? B.bot_fold : lo; ang = -B.rs * STAND_ANGLE; }
        r = draw_hinged(orig, self, sprite, colour, sub, x, y, xs, ys, rot, alpha, F, ang);
        CRUMB(8, "strip drawn");
        if ((lo + hi) / 2 < mid && B.bot_fold > 1e8) {          // the game has no stand on the near side: mirror this one there
            double Fb = B.fy + B.fh;                            // near edge of the field image
            draw_hinged(orig, self, sprite, colour, sub, x + FANS_W * xs, Fb + (hi - y), -xs, -ys, rot, alpha, Fb, -B.rs * STAND_ANGLE);
        }
    }
    g_t_world += mach_absolute_time() - t0;
    return r;
}

// tap low on the screen before the snap = hand off: the offensive player nearest the camera
// that isn't the ball carrier (the running back lines up behind the QB, i.e. toward the camera)
int rb_cam3d_handoff_target(double gx, double gy, double *rx, double *ry) {
    if (!rb_cam3d_active() || g_last_gh <= 0 || gy < g_last_gh * 0.88) return 0;
    int best = -1; double bd = 1e9;
    double qx = C.eye.v[0] + C.fwd * CAM_BACK, qy = C.eye.v[1];            // focus = ball carrier
    for (int i = 0; i < g_nlast; i++) {
        Spr *p = &g_last[i];
        if (!p->offense) continue;
        double x = rb_get_builtin_real(p->inst, SLOT_X), y = rb_get_builtin_real(p->inst, SLOT_Y);
        if (hypot(x - qx, y - qy) < 15) continue;                           // that's the QB
        if ((x - qx) * C.fwd > -20) continue;                               // must be behind him
        if (p->depth < bd) { bd = p->depth; best = i; *rx = x; *ry = y; }
    }
    (void)gx;
    return best >= 0;
}
