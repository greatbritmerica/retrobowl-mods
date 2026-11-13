// kickmath_test.c - host test for src/kickmath.c, run by the GitHub build:
//   cc -o build/kickmath_test tests/kickmath_test.c src/kickmath.c -lm && build/kickmath_test
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../src/kickmath.h"

int main(void) {
    PuntLand p;
    // straight downfield (offense attacks toward -x: aim 180 deg), 40 yards from x 1800
    km_punt_land(1800, 300, -1, 180, 40, 20, 141, 458, &p);
    assert(fabs(p.lx - 1000) < 1e-6 && fabs(p.ly - 300) < 1e-6 && !p.out);
    // aim held to 20 degrees off straight even when the swipe was 60 off
    km_punt_land(1800, 300, -1, 120, 10, 20, 0, 1000, &p);
    assert(fabs(atan2(p.ly - 300, 1800 - p.lx) * 57.29578 - 20) < 1e-6 || fabs(atan2(300 - p.ly, 1800 - p.lx) * 57.29578 - 20) < 1e-6);
    // wide toward the top sideline: lands where it crossed
    km_punt_land(1800, 200, -1, 200, 50, 30, 141, 458, &p);
    assert(p.out && fabs(p.ly - 141) < 1e-6 && p.lx < 1800 && p.lx > 1000);
    // the other way (+x offense): aim 0 deg
    km_punt_land(500, 300, 1, 0, 30, 20, 141, 458, &p);
    assert(fabs(p.lx - 1100) < 1e-6 && !p.out);
    // yard lines: line at x 1700 is your 30 going -x (scrim -20); 40 yards on is their 30 -> they take it at their 30... (-yl)
    double yl = km_yardline(-20, 1700, -1, 1700 - 40 * KM_PX_PER_YD);
    assert(fabs(yl - 20) < 1e-6);
    assert(fabs(km_spot_after_punt(yl) + 20) < 1e-6);
    assert(km_spot_after_punt(55) == -30);                       // into the end zone: touchback at their 20
    // field x <-> yard line round trip, both goal sides; your own 20 going -x is x 1910
    assert(fabs(km_x_of_yardline(-30, 1) - 1910) < 1e-6);
    assert(fabs(km_yardline_of_x(1910, 1) + 30) < 1e-6);
    assert(fabs(km_yardline_of_x(km_x_of_yardline(12.5, -1), -1) - 12.5) < 1e-6);
    puts("kickmath_test ok");
    return 0;
}
