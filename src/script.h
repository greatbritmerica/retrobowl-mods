// script.h - play scripts: a small language the player edits in the Workshop (Documents/rbmod_script.txt).
// The language lives in script.c and knows nothing about the game; everything it can do to the game goes
// through the sh_* functions below (script_game.c in the mod, a stub in the host tests).
#pragma once

void rb_script_load(void);                      // re-read the file if it changed (once per play)
int  rb_script_parse(const char *src);           // (load's parser) 0 = ok; errors are logged
void rb_script_fire(const char *ev, const char *arg);   // run the handlers for an event
int  rb_script_active(void);                     // handlers loaded and enabled
void rb_script_end_play(void);                   // the play is over: drop per-play state
// (the language itself, script.c)
int  rb_script_run(const char *ev, const char *arg);    // handlers run
const char *rb_script_error(int *line);          // last parse error ("" if none)
int  rb_script_handlers(void);
int  rb_script_enabled_handlers(void);
void rb_script_reset_vars(void);
const char *rb_script_fn_name(int i);

// ---- the host: what a script can reach. Instances are opaque pointers, valid for one handler run.
typedef struct { int kind; double n; const char *s; } SArg;   // kind 0 number, 1 string
int    sh_list(const char *which, void **out, int max);        // receivers defenders offense defense
void  *sh_one(const char *which);                              // qb back ball
double sh_info(const char *name, int *ok);                     // down togo quarter clock lead goal ...
double sh_pos(void *p, int want_y);
int    sh_get(void *p, const char *name, double *num, char *str, int n);       // 0 unknown, 1 number, 2 string
int    sh_set(void *p, const char *name, double v);
int    sh_scope_get(int global, const char *name, double *num, char *str, int n);   // game (controller) or global
int    sh_scope_set(int global, const char *name, double v);
int    sh_call(void *self, const char *script, int argc, const SArg *args, double *num, char *str, int n);
void   sh_place(void *p, double x, double y);
void   sh_steer(void *p, double x, double y, double force);
void   sh_speed(void *p, double pct);
int    sh_route(void *p, const char *route);
int    sh_carry(double x, double arc);
void   sh_icon(void *p);
void   sh_log(const char *msg);
// bridge to the rules (rules.c)
int    sh_pressed(const char *label);
void   sh_button(const char *label);
void   sh_msg(const char *text);
int    sh_tune(const char *key, double v, int add);
