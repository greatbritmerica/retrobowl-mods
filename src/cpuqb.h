#pragma once
#include "rb.h"
// once per ball step, before the game's own ball update; cpu_offense = CPU has the ball on the field
void rb_cpuqb_step(CInstance *ball, CInstance *other, int cpu_offense);
void rb_cpuqb_info(double *los_x, double *fwd, double *qbx, double *qby);
extern int rb_cpuqb_punt_pending;
