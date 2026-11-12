// bullet_test.c - host test for src/bullet.c, run by the GitHub build:
//   cc -o build/bullet_test tests/bullet_test.c src/bullet.c -lm && build/bullet_test
// It flies the planned ball frame by frame with the game's measured physics and checks it arrives.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../src/bullet.h"

static void fly(const BulletPlan *p, double qx, double qy, double h0, double *x, double *y, double *h) {
    double v = p->speed, z = p->zvel;
    *x = qx; *y = qy; *h = h0;
    int n = (int)(p->frames + 0.5);
    for (int f = 0; f < n; f++) { *x += p->dx * v; *y += p->dy * v; *h += z; v *= 0.986; z -= 0.08; }
}

int main(void) {
    BulletPlan p;
    // a receiver 300 px downfield, running across at 2 px a frame
    assert(rb_bullet_plan(1000, 300, 10, 700, 250, 0, 2, 12, 20, &p));
    double x, y, h;
    fly(&p, 1000, 300, 10, &x, &y, &h);
    assert(fabs(x - p.lx) < 8 && fabs(y - p.ly) < 8);           // reaches the lead point...
    assert(fabs(p.ly - (250 + 2 * p.frames)) < 1e-6);           // ...which is where he'll be
    assert(fabs(h - 20) < 3);                                   // at catch height
    assert(p.frames >= 20 && p.frames <= 30);                   // fast: about 25 frames for 300 px
    double top = 10, z = p.zvel, hh = 10;                       // flat: never far above head height
    for (int f = 0; f < (int)p.frames; f++) { hh += z; z -= 0.08; if (hh > top) top = hh; }
    assert(top < 40);
    assert(!rb_bullet_plan(1000, 300, 10, 1005, 302, 0, 0, 12, 20, &p));   // too close to plan
    puts("bullet_test ok");
    return 0;
}
