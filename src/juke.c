// juke.c - see juke.h.
#include "juke.h"

int rb_juke_closing(double cx, double cy, double cvx, double cvy, double dx, double dy, double dvx, double dvy, double reach) {
    double rx = cx - dx, ry = cy - dy;                       // defender -> carrier
    if (rx * rx + ry * ry > reach * reach) return 0;
    return rx * (dvx - cvx) + ry * (dvy - cvy) > 0;         // relative velocity points at the carrier
}
