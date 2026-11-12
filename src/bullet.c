// bullet.c - see bullet.h. Ball physics measured in cpuqb.c: speed *= 0.986 per frame, gravity 0.08.
#include "sys.h"
#include "bullet.h"

#define DECAY   0.986
#define GRAVITY 0.08

int rb_bullet_plan(double qx, double qy, double h0, double rx, double ry, double rvx, double rvy,
                   double pace, double catch_h, BulletPlan *out) {
    if (pace < 4) pace = 4;
    double lx = rx, ly = ry, D = 0, T = 0;
    for (int k = 0; k < 4; k++) {                  // lead him: where he'll be when the ball gets there
        D = hypot(lx - qx, ly - qy);
        T = D / pace;
        if (T < 6) T = 6;
        lx = rx + rvx * T; ly = ry + rvy * T;
    }
    D = hypot(lx - qx, ly - qy);
    if (D < 20) return 0;
    out->lx = lx; out->ly = ly;
    out->dx = (lx - qx) / D; out->dy = (ly - qy) / D;
    out->frames = T;
    out->speed = D * (1.0 - DECAY) / (1.0 - pow(DECAY, T));           // covers exactly D in T frames
    out->zvel = (catch_h - h0 + 0.5 * GRAVITY * T * T) / T;            // at catch height after T frames
    return 1;
}
