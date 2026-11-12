// bullet.h - flight plan for the super throw: a fast, flat ball that reaches a moving receiver.
// Pure math (no game calls), so tests/bullet_test.c can check it on any machine.
#pragma once

typedef struct {
    double lx, ly;      // where the ball meets the receiver (his lead point)
    double dx, dy;      // unit direction from the QB to it
    double speed;       // ball speed at release (decays x0.986 per frame, as the game's does)
    double zvel;        // upward speed at release (gravity 0.08 per frame)
    double frames;      // frames in the air until it reaches him
} BulletPlan;

// qx,qy: the ball at release; h0: its height then. rx,ry / rvx,rvy: the receiver and his velocity
// (px per frame). pace: average ball speed wanted, px per frame. catch_h: ball height on arrival.
// Returns 0 when the receiver is too close to plan for (under 20 px).
int rb_bullet_plan(double qx, double qy, double h0, double rx, double ry, double rvx, double rvy,
                   double pace, double catch_h, BulletPlan *out);
