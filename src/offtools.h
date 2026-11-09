#pragma once
#include "rb.h"
void rb_off_step(CInstance *ball, CInstance *other, int human_offense);   // every ball step
void rb_off_after_ai(CInstance *player);                                  // after the AI moves a player
int  rb_off_scrambled(void);                                              // the player's QB scrambled and pulled up this play
