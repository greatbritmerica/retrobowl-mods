#pragma once
#include "rb.h"
void rb_off_step(CInstance *ball, CInstance *other, int human_offense);   // every ball step
void rb_off_after_ai(CInstance *player);                                  // after the AI moves a player
double rb_off_pa_bite(void);                                           // how hard the defense bit on play action this play (0 = none)
int  rb_off_blocker_ignores_ball(CInstance *player);                      // the back, kept in to block, as the ball leaves the QB
void rb_off_cpu_drive(void);                                              // the CPU has the ball (resets the back's legs)
void rb_off_forget(void);                                                 // match objects destroyed: drop pointers
int  rb_off_scrambled(void);                                              // the player's QB scrambled and pulled up this play
