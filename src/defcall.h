#pragma once
#include "rb.h"
int  rb_defcall_ready(int down, double yards, unsigned look_frames);   // cpuqb, pre-snap: 1 when the call is in
void rb_defcall_snap(int is_run, int deep);                            // cpuqb, at the snap
void rb_defcall_reset(void);
void rb_defcall_disguise(int disguised);                              // formation lied this play
int  rb_defcall_blitz(void);                                            // a blitz is on this play
void rb_defcall_step(CInstance *ball, CInstance *other, int cpu_offense);
void rb_defcall_after_ai(CInstance *player);
