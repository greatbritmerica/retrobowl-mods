#pragma once
#include "rb.h"
void rb_def_step(CInstance *ball, CInstance *other, int cpu_offense, double los_x, double fwd, double qbx, double qby);
void rb_def_after_ai(CInstance *player);   // after s_get_destination
void rb_def_draw(CInstance *player);       // before obj_player Draw
int rb_def_human(double *x, double *y);   // position of the defender the player controls
int rb_def_jumped(void);
CInstance *rb_def_controlled(void);      // the defender the player controls (0 if none)                  // did the player jump the route this play
