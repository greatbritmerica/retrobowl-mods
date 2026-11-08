#pragma once
#include "rb.h"
void rb_dlearn_step(CInstance *ball, CInstance *other, int human_offense);   // once per ball step
void rb_dlearn_after_ai(CInstance *player);                                   // after s_get_destination
double rb_dbrain_dial(void);            // CPU defense strength 0.1-1, steered toward ~50% stops
