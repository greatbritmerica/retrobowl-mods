// ui.c - native iOS popups (UIAlertController via the Objective-C runtime).
#include "rb.h"
#include "ui.h"

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
