#pragma once
#include "rb.h"
void rb_off_step(CInstance *ball, CInstance *other, int human_offense);   // every ball step
void rb_off_after_ai(CInstance *player);                                  // after the AI moves a player
double rb_off_pa_bite(void);                                           // how hard the defense bit on play action this play (0 = none)
int  rb_off_blocker_ignores_ball(CInstance *player);                      // the back, kept in to block, as the ball leaves the QB
void rb_off_cpu_drive(void);                                              // the CPU has the ball (resets the back's legs)
double rb_off_los(void);                                                  // real line of scrimmage (x) this play, or < -1e8
double rb_off_snap_seconds(void);                                         // seconds since the snap on your pass play, or -1
int  rb_off_not_a_target(CInstance *inst);                                // the QB or an interior lineman
void rb_off_report(char *buf, size_t n);                                  // this play's offense fields for the report
void rb_off_apply_penalty(void);                                          // a rule's penalty, as the play is torn down
void rb_off_gui(CInstance *self);                                         // draw on the GUI layer (Hail Mary receiver icons)
void rb_off_forget(void);                                                 // match objects destroyed: drop pointers
int  rb_off_scrambled(void);                                              // the player's QB scrambled and pulled up this play
void rb_off_oc_lesson(CInstance *rcv, int ok, double yds);              // a pass result: your coordinator learns from it
