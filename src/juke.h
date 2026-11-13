// juke.h - when a tackler is closing on the ball carrier (the minute drill's juke prompt, offtools.c).
// Pure math (no game calls), so tests/juke_test.c can check it on any machine.
#pragma once

// 1 when the defender is within reach of the carrier and their gap is shrinking (px and px per frame).
int rb_juke_closing(double cx, double cy, double cvx, double cvy, double dx, double dy, double dvx, double dvy, double reach);
