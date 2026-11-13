// kickmath.c - see kickmath.h
#include "sys.h"
#include "kickmath.h"

void km_punt_land(double qx, double qy, double fwd, double direc, double yards, double aim_max,
                  double top_y, double bot_y, PuntLand *o) {
    double ahead = fwd > 0 ? 0 : 180;
    double off = fmod(direc - ahead + 540.0, 360.0) - 180.0;        // -180..180 from straight downfield
    if (off > aim_max) off = aim_max;
    if (off < -aim_max) off = -aim_max;
    double a = (ahead + off) * 3.14159265358979 / 180.0, d = yards * KM_PX_PER_YD;
    double dx = cos(a), dy = sin(a);
    o->lx = qx + dx * d; o->ly = qy + dy * d; o->out = 0;
    double edge = o->ly < top_y ? top_y : o->ly > bot_y ? bot_y : o->ly;
    if (edge != o->ly && fabs(dy) > 1e-9) {                          // went out: where it crossed
        double t = (edge - qy) / (dy * d);
        o->lx = qx + dx * d * t; o->ly = edge; o->out = 1;
    }
}

double km_yardline(double scrim, double los_x, double fwd, double x) { return scrim + (x - los_x) * fwd / KM_PX_PER_YD; }

double km_spot_after_punt(double yl) {
    if (yl >= 50) return -30;              // touchback: their 20
    if (yl < -49) yl = -49;
    return -yl;
}

double km_x_of_yardline(double yl, double goal_side) { return KM_MID_X - goal_side * yl * KM_PX_PER_YD; }
double km_yardline_of_x(double x, double goal_side) { return (KM_MID_X - x) * goal_side / KM_PX_PER_YD; }
