// ui.c - native iOS popups (UIAlertController via the Objective-C runtime).
#include "rb.h"
#include "ui.h"
#include "sys.h"

#ifndef RB_FREESTANDING
#include <objc/runtime.h>
#include <objc/message.h>
#include <dispatch/dispatch.h>
#else
typedef struct objc_object *id;
typedef struct objc_selector *SEL;
id objc_getClass(const char *name);
SEL sel_registerName(const char *name);
void objc_msgSend(void);
typedef struct dispatch_queue_s *dispatch_queue_t;
extern struct dispatch_queue_s _dispatch_main_q;
void dispatch_async(dispatch_queue_t q, void (^block)(void));
#define dispatch_get_main_queue() (&_dispatch_main_q)
#endif

// typed objc_msgSend shapes
#define MSG(ret, ...) ((ret (*)(id, SEL, ##__VA_ARGS__))objc_msgSend)
static SEL S(const char *n) { return sel_registerName(n); }
static id C(const char *n) { return (id)objc_getClass(n); }

static id nsstr(const char *s) {
    return MSG(id, const char *)(C("NSString"), S("stringWithUTF8String:"), s);
}

static id top_view_controller(void) {
    id app = MSG(id)(C("UIApplication"), S("sharedApplication"));
    if (!app) return 0;
    id win = MSG(id)(app, S("keyWindow"));
    if (!win) {
        id wins = MSG(id)(app, S("windows"));
        win = wins ? MSG(id)(wins, S("firstObject")) : 0;
    }
    if (!win) return 0;
    id vc = MSG(id)(win, S("rootViewController"));
    while (vc) {
        id p = MSG(id)(vc, S("presentedViewController"));
        if (!p) break;
        vc = p;
    }
    return vc;
}

// Two-button alert. cb(1) for the first button, cb(2) for the second; cb(0) if it could not be shown.
// The callback runs on the main thread.
void rb_ui_choice(const char *title, const char *message, const char *b1, const char *b2, void (*cb)(int)) {
    dispatch_async(dispatch_get_main_queue(), ^{
        id vc = top_view_controller();
        if (!vc) { rb_log("ui: no view controller"); cb(0); return; }
        id alert = MSG(id, id, id, long)(C("UIAlertController"),
                       S("alertControllerWithTitle:message:preferredStyle:"), nsstr(title), nsstr(message), 1);
        id a1 = MSG(id, id, long, void (^)(id))(C("UIAlertAction"), S("actionWithTitle:style:handler:"),
                       nsstr(b1), 0, ^(id act) { cb(1); });
        id a2 = MSG(id, id, long, void (^)(id))(C("UIAlertAction"), S("actionWithTitle:style:handler:"),
                       nsstr(b2), 0, ^(id act) { cb(2); });
        MSG(void, id)(alert, S("addAction:"), a1);
        MSG(void, id)(alert, S("addAction:"), a2);
        MSG(void, id, _Bool, id)(vc, S("presentViewController:animated:completion:"), alert, 1, 0);
    });
}

// Up to 6 buttons as an action sheet (keeps the middle of the field visible). cb(i) is 1-based.
void rb_ui_choice_n(const char *title, const char *message, const char **buttons, int n, void (*cb)(int)) {
    if (n > 6) n = 6;
    const char *b0 = n > 0 ? buttons[0] : 0, *b1 = n > 1 ? buttons[1] : 0, *b2 = n > 2 ? buttons[2] : 0,
               *b3 = n > 3 ? buttons[3] : 0, *b4 = n > 4 ? buttons[4] : 0, *b5 = n > 5 ? buttons[5] : 0;
    dispatch_async(dispatch_get_main_queue(), ^{
        id vc = top_view_controller();
        if (!vc) { rb_log("ui: no view controller"); cb(0); return; }
        id alert = MSG(id, id, id, long)(C("UIAlertController"),
                       S("alertControllerWithTitle:message:preferredStyle:"), nsstr(title), nsstr(message), 0);
        for (int i = 0; i < n; i++) {
            int idx = i + 1;
            const char *bt = i == 0 ? b0 : i == 1 ? b1 : i == 2 ? b2 : i == 3 ? b3 : i == 4 ? b4 : b5;
            id a = MSG(id, id, long, void (^)(id))(C("UIAlertAction"), S("actionWithTitle:style:handler:"),
                       nsstr(bt), 0, ^(id act) { cb(idx); });
            MSG(void, id)(alert, S("addAction:"), a);
        }
        id pop = MSG(id)(alert, S("popoverPresentationController"));   // iPad needs an anchor
        if (pop) {
            id view = MSG(id)(vc, S("view"));
            MSG(void, id)(pop, S("setSourceView:"), view);
        }
        MSG(void, id, _Bool, id)(vc, S("presentViewController:animated:completion:"), alert, 1, 0);
    });
}

// ---------------------------------------------------------------- compact side menu
// A small panel down the left edge so the formation stays visible. cb(i) is 1-based.
typedef struct { double x, y, w, h; } RbRect;
typedef struct { double top, left, bottom, right; } RbInsets;

static id make_label(const char *text, double size, double x, double y, double w, double h, int bold) {
    id l = MSG(id, RbRect)(MSG(id)(C("UILabel"), S("alloc")), S("initWithFrame:"), (RbRect){ x, y, w, h });
    MSG(void, id)(l, S("setText:"), nsstr(text));
    MSG(void, long)(l, S("setNumberOfLines:"), 0);
    MSG(void, id)(l, S("setTextColor:"), MSG(id)(C("UIColor"), S("whiteColor")));
    id font = bold ? MSG(id, double)(C("UIFont"), S("boldSystemFontOfSize:"), size)
                   : MSG(id, double)(C("UIFont"), S("systemFontOfSize:"), size);
    MSG(void, id)(l, S("setFont:"), font);
    return l;
}

// Compact panels docked to a screen edge (several can exist, one per id). Tapping a button closes the
// panel and calls cb(i), 1-based. side: 0 = left edge, 1 = right edge.
static id g_panel[RB_UI_PANELS];

void rb_ui_panel_close(int pid) {
    if (pid < 0 || pid >= RB_UI_PANELS) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_panel[pid]) { MSG(void)(g_panel[pid], S("removeFromSuperview")); g_panel[pid] = 0; }
    });
}

void rb_ui_panel(int pid, int side, double width, const char *title, const char *message,
                 const char **buttons, int n, void (*cb)(int)) {
    if (pid < 0 || pid >= RB_UI_PANELS) return;
    if (n > 10) n = 10;
    struct { char t[96], m[520], b[10][40]; int n; } c;
    memset(&c, 0, sizeof c);
    snprintf(c.t, sizeof c.t, "%s", title ? title : "");
    snprintf(c.m, sizeof c.m, "%s", message ? message : "");
    for (int i = 0; i < n; i++) snprintf(c.b[i], sizeof c.b[i], "%s", buttons[i]);
    c.n = n;
    dispatch_async(dispatch_get_main_queue(), ^{
        id app = MSG(id)(C("UIApplication"), S("sharedApplication"));
        id win = app ? MSG(id)(app, S("keyWindow")) : 0;
        if (!win) { id ws = app ? MSG(id)(app, S("windows")) : 0; win = ws ? MSG(id)(ws, S("firstObject")) : 0; }
        if (!win) { rb_log("ui: no window"); cb(0); return; }
        if (g_panel[pid]) { MSG(void)(g_panel[pid], S("removeFromSuperview")); g_panel[pid] = 0; }
        RbRect wb = MSG(RbRect)(win, S("bounds"));
        RbInsets in = MSG(RbInsets)(win, S("safeAreaInsets"));
        double W = width, bh = 38, gap = 6, pad = 10;
        double th = c.t[0] ? 20 : 0, mh = c.m[0] ? (W > 150 ? 104 : 60) : 0;
        double fixed = 2 * pad - gap + th + (mh ? mh + 4 : 0);
        int cols = fixed + c.n * (bh + gap) > wb.h - in.top - in.bottom - 12 ? 2 : 1;   // too tall for one column: two
        int rows = (c.n + cols - 1) / cols;
        W = cols * width - (cols - 1) * pad;
        double bw = (W - 2 * pad - (cols - 1) * gap) / cols;
        double H = pad + th + (mh ? mh + 4 : 0) + rows * (bh + gap) + pad - gap;
        int right = side & 1, bottom = side & 2;                // 0 left, 1 right, 2 left-bottom, 3 right-bottom
        double x = right ? wb.w - in.right - 6 - W : in.left + 6;
        double y = bottom ? wb.h - in.bottom - 8 - H : (wb.h - H) / 2;
        if (y < in.top + 4) y = in.top + 4;
        id panel = MSG(id, RbRect)(MSG(id)(C("UIView"), S("alloc")), S("initWithFrame:"), (RbRect){ x, y, W, H });
        MSG(void, id)(panel, S("setBackgroundColor:"), MSG(id, double, double)(C("UIColor"), S("colorWithWhite:alpha:"), 0.0, 0.62));
        MSG(void, double)(MSG(id)(panel, S("layer")), S("setCornerRadius:"), 12.0);
        double cy = pad;
        if (th) { MSG(void, id)(panel, S("addSubview:"), make_label(c.t, 15, pad, cy, W - 2 * pad, th, 1)); cy += th; }
        if (mh) { MSG(void, id)(panel, S("addSubview:"), make_label(c.m, 11, pad, cy, W - 2 * pad, mh, 0)); cy += mh + 4; }
        for (int i = 0; i < c.n; i++) {
            int idx = i + 1;
            id act = MSG(id, id, id, id, void (^)(id))(C("UIAction"), S("actionWithTitle:image:identifier:handler:"),
                         nsstr(c.b[i]), 0, 0, ^(id a) {
                             if (g_panel[pid]) { MSG(void)(g_panel[pid], S("removeFromSuperview")); g_panel[pid] = 0; }
                             cb(idx);
                         });
            id btn = MSG(id, long, id)(C("UIButton"), S("buttonWithType:primaryAction:"), 1, act);
            MSG(void, RbRect)(btn, S("setFrame:"), (RbRect){ pad + (i % cols) * (bw + gap), cy + (i / cols) * (bh + gap), bw, bh });
            MSG(void, id)(btn, S("setBackgroundColor:"), MSG(id, double, double)(C("UIColor"), S("colorWithWhite:alpha:"), 1.0, 0.16));
            MSG(void, id, unsigned long)(btn, S("setTitleColor:forState:"), MSG(id)(C("UIColor"), S("whiteColor")), 0);
            MSG(void, double)(MSG(id)(btn, S("layer")), S("setCornerRadius:"), 8.0);
            MSG(void, id)(panel, S("addSubview:"), btn);
        }
        MSG(void, id)(win, S("addSubview:"), panel);
        g_panel[pid] = panel;
    });
}

void rb_ui_side_menu(const char *title, const char *message, const char **buttons, int n, void (*cb)(int)) {
    rb_ui_panel(RB_PANEL_DEFCALL, 0, 176, title, message, buttons, n, cb);
}
