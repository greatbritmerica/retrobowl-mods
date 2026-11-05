#pragma once
#include "rb.h"
void rb_def_step(CInstance *ball, CInstance *other, int cpu_offense, double los_x, double fwd, double qbx, double qby);
void rb_def_after_ai(CInstance *player);   // after s_get_destination
void rb_def_draw(CInstance *player);       // before obj_player Draw
