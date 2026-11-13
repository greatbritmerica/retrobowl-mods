// practice.h - weekly practice (src/practice.c)
#pragma once
#include "rb.h"
void rb_practice_slog(const char *line);     // every line of the game's own log (finds the week change)
void rb_practice_tick(CInstance *self);      // any game-thread frame: carry out the week's pick
