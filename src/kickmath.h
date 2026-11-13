// kickmath.h - pure field maths for punts (no game calls; tested in tests/kickmath_test.c)
#pragma once
#define KM_PX_PER_YD 20.0     // measured from the logs: los_x = 1310 - 20 * yard line (your team attacks toward -x)
#define KM_MID_X     1310.0

typedef struct { double lx, ly; int out; } PuntLand;
// Where a punt lands. (qx, qy) the punter, fwd +1/-1 the way the offense attacks, direc the aim in screen
// degrees (y down) as the game keeps it, yards the distance. Past a sideline (top_y / bot_y) it lands where
// it crossed, out = 1. The aim is held within aim_max degrees of straight downfield.
void km_punt_land(double qx, double qy, double fwd, double direc, double yards, double aim_max,
                  double top_y, double bot_y, PuntLand *o);
// Yard line (offense's view, -50 own goal .. +50 their goal) of field x, from this play's line.
double km_yardline(double scrim, double los_x, double fwd, double x);
// The other team's yard line after a punt that lands at our yard line yl: past the goal line is a touchback (their 20).
double km_spot_after_punt(double yl);
// Field x of a yard line for the receiving team, given which side of midfield its own goal is on (goal_side +1 = +x).
double km_x_of_yardline(double yl, double goal_side);
// Yard line of field x for a team whose own goal is on goal_side (+1 = +x).
double km_yardline_of_x(double x, double goal_side);
