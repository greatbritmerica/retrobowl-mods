#pragma once
// Two-button native alert; cb(1)/cb(2) for the buttons, cb(0) if it could not be shown. Main thread.
void rb_ui_choice(const char *title, const char *message, const char *b1, const char *b2, void (*cb)(int));
void rb_ui_choice_n(const char *title, const char *message, const char **buttons, int n, void (*cb)(int));
void rb_ui_side_menu(const char *title, const char *message, const char **buttons, int n, void (*cb)(int));
#define RB_UI_PANELS     4
#define RB_PANEL_DEFCALL 0        // defensive call menu (CPU drives)
#define RB_PANEL_PRESNAP 1        // play action / hot route (player's offense, before the snap)
#define RB_PANEL_PUMP    2        // pump fake (player's offense, while aiming)
void rb_ui_panel(int pid, int side, double width, const char *title, const char *message,
                 const char **buttons, int n, void (*cb)(int));
void rb_ui_panel_close(int pid);
