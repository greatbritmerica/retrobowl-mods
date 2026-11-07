#pragma once
// Two-button native alert; cb(1)/cb(2) for the buttons, cb(0) if it could not be shown. Main thread.
void rb_ui_choice(const char *title, const char *message, const char *b1, const char *b2, void (*cb)(int));
void rb_ui_choice_n(const char *title, const char *message, const char **buttons, int n, void (*cb)(int));
void rb_ui_side_menu(const char *title, const char *message, const char **buttons, int n, void (*cb)(int));
