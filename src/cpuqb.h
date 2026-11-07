#pragma once
#include "rb.h"
// once per ball step, before the game's own ball update; cpu_offense = CPU has the ball on the field
void rb_cpuqb_step(CInstance *ball, CInstance *other, int cpu_offense);
void rb_cpuqb_info(double *los_x, double *fwd, double *qbx, double *qby);
enum { ACT_NONE, ACT_PUNT, ACT_FIELD_GOAL, ACT_PLAY_4TH };
extern int rb_cpuqb_action;      // run by hooks.c after the ball update returns
extern int rb_cpuqb_fourth_msg;  // set by the message hook: 1 = FG offered, 2 = punt offered
int rb_cpuqb_target(double *x, double *y);
void rb_cpuqb_fourth_closed(void);
int rb_cpuqb_receiver(double *x, double *y);
void rb_cpuqb_fourth_tick(void);
extern double rb_cpuqb_fg_dist;
