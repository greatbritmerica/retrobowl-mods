// punt.h - punts: yours played live (kick mode), the CPU's run back like a kickoff (src/punt.c)
#pragma once
#include "rb.h"
enum { PR_PASS, PR_SWALLOW, PR_AS_PUNT };
int  rb_punt_action_result(double code);                      // before s_action_result: PR_*
int  rb_punt_ball(CInstance *ball, CInstance *other);         // each ball step, before the game's: 1 = skip the game's step
void rb_punt_after_ai(CInstance *player);                     // after the AI moves a player
void rb_punt_header(const char *stage_name, double stage);    // a commentary stage starts (its name and number)
void rb_punt_after_commentary(void);                          // after the outermost s_update_commentary call
void rb_punt_possession(unsigned long long caller, double before, double after, double human);   // after s_change_possession
void rb_punt_slog(const char *line);                          // every line of the game's own log
void rb_punt_destroy(void);                                   // match objects about to be freed
void rb_punt_gui(CInstance *self);                            // draw on the GUI layer (kick power)
int  rb_punt_play(void);                                      // 1 = this play of yours is a punt play (no pump fake / super tap)
